/*
 * 后台线程：QQ 复制时把内容放到 Wayland 剪贴板，QQ 粘贴时从 Wayland 剪贴板取。
 *
 * 只有这个线程使用自己的 X 连接和 Wayland 连接。读写管道交给短命的辅助线程：
 * QQ 粘贴时在主线程上一直等应答，这里被哪个慢程序拖住，QQ 的界面就跟着卡住。
 */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "clipbridge.h"

#define READ_TIMEOUT_MS 3000     /* QQ 粘贴时，从 Wayland 读数据最多等这么久，到时回「没有」 */
#define WRITE_TIMEOUT_MS 30000   /* 别的程序粘贴 QQ 的内容却迟迟不读，到时放弃 */

/*
 * Wayland 剪贴板上现在的内容（也可能是我们自己放的）。QQ 复制的内容是否还在剪贴板上，
 * 只看我们的数据源有没有被合成器取消；不靠格式辨认，因为 wl-clip-persist 这类工具
 * 会把我们的内容连同全部格式原样重新放一遍。
 */
static struct cb_offer *current;
static Window source_owner;        /* 我们的数据源对应的 QQ 复制窗口 */
static uint32_t source_generation;

/* ---------------- 辅助线程 ---------------- */

static int ms_left(const struct timespec *end)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)((end->tv_sec - now.tv_sec) * 1000 + (end->tv_nsec - now.tv_nsec) / 1000000);
}

static struct timespec deadline(int ms)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec += 1;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

static int spawn(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0)
        return -1;
    pthread_detach(t);
    return 0;
}

/* 一次「QQ 粘贴 → 从 Wayland 读」：读完（或超时）后交回后台线程写给 QQ */
struct read_job {
    struct cb_request req;
    char *target, *mime;
    int fd, to_gnome, complete;
    unsigned char *data;
    long len;
    struct read_job *next;
};

static pthread_mutex_t done_mu = PTHREAD_MUTEX_INITIALIZER;
static struct read_job *done;

static void *reader(void *arg)
{
    struct read_job *j = arg;
    int fd = j->fd;
    size_t cap = 65536, len = 0;
    unsigned char *buf = malloc(cap);
    struct timespec end = deadline(READ_TIMEOUT_MS);
    int complete = 0;
    for (;;) {
        int left = ms_left(&end);
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = left > 0 ? poll(&p, 1, left) : 0;
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr <= 0)
            break;
        if (len + 65536 > cap) {
            unsigned char *nb = realloc(buf, cap * 2);
            if (!nb)
                break;
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            complete = r == 0;
            break;
        }
        len += r;
    }
    j->data = buf;
    j->len = (long)len;
    j->complete = complete;
    pthread_mutex_lock(&done_mu);
    j->next = done;
    done = j;
    pthread_mutex_unlock(&done_mu);
    cb_wake();

    /* 提前放弃时读到 EOF 再关：否则数据源下一次 write() 会吃到 EPIPE（见 main.c 的 SIGPIPE 说明） */
    if (!complete) {
        char tmp[65536];
        for (;;) {
            ssize_t r = read(fd, tmp, sizeof(tmp));
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
        }
    }
    close(fd);
    return NULL;
}

/* 一次「别的程序粘贴 QQ 的内容」：把数据写进对方给的管道 */
struct write_job {
    int fd;
    unsigned char *data;
    long len;
};

static void *writer(void *arg)
{
    struct write_job *j = arg;
    fcntl(j->fd, F_SETFL, fcntl(j->fd, F_GETFL) | O_NONBLOCK);
    struct timespec end = deadline(WRITE_TIMEOUT_MS);
    long off = 0;
    while (off < j->len) {
        ssize_t w = write(j->fd, j->data + off, j->len - off);
        if (w > 0) {
            off += w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno != EAGAIN)
            break;
        int left = ms_left(&end);
        struct pollfd p = { j->fd, POLLOUT, 0 };
        if (left <= 0 || poll(&p, 1, left) <= 0 || (p.revents & (POLLERR | POLLHUP)))
            break;
    }
    close(j->fd);
    free(j->data);
    free(j);
    return NULL;
}

/* ---------------- QQ 复制 → Wayland ---------------- */

/*
 * QQ 的剪贴板线程只在 QQ 自己复制、粘贴的那一刻处理 X 事件，之后别人再来要数据，它要等到
 * QQ 下一次用剪贴板才回应（实测）。所以 QQ 一复制就把所有格式取齐缓存起来，之后不论
 * 别的程序还是 QQ 自己粘贴，都从这里给。
 */
#define CACHE_LIMIT (64L << 20)   /* 缓存总量上限，超出的格式粘贴时再向 QQ 要 */

static struct {
    char *target;
    unsigned char *data;
    long len;
} cache[MAX_MIMES];
static int n_cache;
static Atom cached_targets[MAX_MIMES];
static long n_cached_targets;

static void cache_clear(void)
{
    for (int i = 0; i < n_cache; ++i) {
        free(cache[i].target);
        free(cache[i].data);
    }
    n_cache = 0;
    n_cached_targets = 0;
}

static int cache_find(const char *target)
{
    for (int i = 0; i < n_cache; ++i)
        if (!strcmp(cache[i].target, target))
            return i;
    return -1;
}

/* 一种格式的数据：先查缓存，没有再向 QQ 要。返回 malloc 的副本。 */
static long qq_data(const char *target, unsigned char **out)
{
    int c = cache_find(target);
    if (c < 0)
        return cb_x11_fetch(source_owner, cb_x11_atom(target), out);
    *out = malloc(cache[c].len + 1);
    memcpy(*out, cache[c].data, cache[c].len + 1);
    return cache[c].len;
}

/* 取齐 QQ 这次复制的各种格式；QQ 一次没回应就不再等后面的 */
static void cache_fill(Window owner, const struct cb_mimes *targets)
{
    long total = 0;
    for (int i = 0; i < targets->n && n_cache < MAX_MIMES; ++i) {
        const char *t = targets->v[i];
        if (!cb_is_content_name(t))
            continue;
        unsigned char *data;
        long n = cb_x11_fetch(owner, cb_x11_atom(t), &data);
        if (n < 0) {
            LOG("QQ did not provide %s, not caching the rest", t);
            break;
        }
        if (total + n > CACHE_LIMIT) {
            free(data);
            continue;
        }
        total += n;
        cache[n_cache].target = strdup(t);
        cache[n_cache].data = data;
        cache[n_cache].len = n;
        ++n_cache;
    }
}

/* QQ 复制的文件是否都在（复制图片时给的是缓存路径，原图没下载时并不存在） */
static int files_exist(void)
{
    unsigned char *raw;
    long n = qq_data("x-special/gnome-copied-files", &raw);
    if (n < 0)
        n = qq_data("text/uri-list", &raw);
    if (n < 0)
        return 0;
    int ok = 1, any = 0;
    char *save = NULL;
    for (char *line = strtok_r((char *)raw, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!strcmp(line, "copy") || !strcmp(line, "cut") || !*line)
            continue;
        char path[4096];
        const char *p = !strncmp(line, "file://", 7) ? line + 7 : line;
        size_t o = 0;
        for (; *p && o + 1 < sizeof(path); ++p) {
            if (*p == '%' && p[1] && p[2]) {
                char h[3] = { p[1], p[2], 0 };
                path[o++] = (char)strtol(h, NULL, 16);
                p += 2;
            } else {
                path[o++] = *p;
            }
        }
        path[o] = 0;
        any = 1;
        if (access(path, R_OK) != 0) {
            LOG("copied file missing: %s", path);
            ok = 0;
        }
    }
    free(raw);
    return ok && any;
}

static void on_qq_copy(uint32_t generation)
{
    Window owner = cb_qq_owner();
    if (owner == None)
        return;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned char *raw;
    long n = cb_x11_fetch(owner, cb_A_TARGETS, &raw);
    if (n <= 0) {
        LOG("QQ copied but did not answer TARGETS");
        free(raw);
        return;
    }
    cache_clear();
    source_owner = owner;
    source_generation = generation;
    struct cb_mimes targets = { 0 }, offer = { 0 };
    for (long i = 0; i < n / (long)sizeof(Atom); ++i) {
        Atom a = ((Atom *)raw)[i];
        char *name = cb_x11_name(a);
        if (name)
            cb_mimes_add(&targets, name);
        free(name);
        if (n_cached_targets < MAX_MIMES)
            cached_targets[n_cached_targets++] = a;
    }
    free(raw);
    cache_fill(owner, &targets);
    int files = cb_mimes_has(&targets, "x-special/gnome-copied-files") || cb_mimes_has(&targets, "text/uri-list");
    cb_offers_from_qq(&targets, !files || files_exist(), &offer);
    cb_mimes_clear(&targets);
    if (!offer.n) {
        LOG("QQ copied but no transferable format");
        return;
    }
    if (cb_wl_set_source(&offer) == 0) {
        char list[1024];
        cb_mimes_join(&offer, list, sizeof(list));
        LOG("QQ copied -> Wayland (%d formats cached in %d ms): %s", n_cache, -ms_left(&t0), list);
    }
    cb_mimes_clear(&offer);
}

/* 别的程序要我们放上去的某个格式（文本、文件列表按需转换），交给写线程 */
void cb_on_send(const char *mime, int fd)
{
    unsigned char *data = NULL;
    long n = -1;
    if (cb_is_text_name(mime)) {
        n = qq_data("UTF8_STRING", &data);
    } else if (!strcmp(mime, "text/uri-list") || !strcmp(mime, "x-special/gnome-copied-files")) {
        unsigned char *raw;
        long rn = qq_data("x-special/gnome-copied-files", &raw);
        if (rn < 0)
            rn = qq_data("text/uri-list", &raw);
        if (rn >= 0) {
            long len;
            char *uris = cb_paths_to_uris(raw, rn, !strcmp(mime, "text/uri-list") ? "\r\n" : "\n", &len);
            if (!strcmp(mime, "text/uri-list")) {
                data = (unsigned char *)uris;
                n = uris ? len : 0;
            } else {   /* "copy\n" + 每行一个 URI，末尾不带换行 */
                n = 5 + (len > 0 ? len - 1 : 0);
                data = malloc(n + 1);
                memcpy(data, "copy\n", 5);
                if (len > 0)
                    memcpy(data + 5, uris, len - 1);
                free(uris);
            }
            free(raw);
        }
    } else {
        n = qq_data(mime, &data);
    }
    if (n < 0) {
        LOG("paste request %s: QQ did not provide data", mime);
        free(data);
        close(fd);
        return;
    }
    struct write_job *j = malloc(sizeof(*j));
    *j = (struct write_job){ fd, data, n };
    if (spawn(writer, j) < 0) {
        close(fd);
        free(data);
        free(j);
    }
}

/* 我们放上去的内容被别人替换了：告诉 QQ 它不再是剪贴板的主人 */
void cb_on_cancelled(void)
{
    cache_clear();
    Window w = cb_qq_lost(source_generation);
    if (w != None)
        cb_x11_clear(w);
}

void cb_on_selection(struct cb_offer *offer)
{
    cb_offer_free(current);
    current = offer;
    cb_set_foreign(current != NULL);
}

/* 按 QQ 那次复制缓存的内容回答；缓存里没有就返回 0 */
static int serve_cached(const struct cb_request *r)
{
    if (r->target == cb_A_TARGETS) {
        if (!n_cached_targets)
            return 0;
        cb_x11_reply(r, XA_ATOM, 32, cached_targets, n_cached_targets);
        return 1;
    }
    char *target = cb_x11_name(r->target);
    int c = target ? cache_find(target) : -1;
    free(target);
    if (c < 0)
        return 0;
    cb_x11_reply(r, r->target, 8, cache[c].data, cache[c].len);
    return 1;
}

/* ---------------- QQ 粘贴 ← Wayland ---------------- */

static void handle_request(const struct cb_request *r)
{
    /* 剪贴板上是 QQ 自己复制的内容：从缓存给；QQ 刚又复制、还没取齐，或者缓存里没有，才交给 QQ 自己回应 */
    Window owner = cb_qq_owner();
    if (owner != None) {
        int fresh = owner == source_owner && cb_copy_generation() == source_generation;
        if (!fresh || !serve_cached(r))
            cb_x11_forward(owner, r);
        return;
    }
    if (!current) {
        cb_x11_refuse(r);
        return;
    }
    if (r->target == cb_A_TARGETS) {
        Atom t[MAX_MIMES + 8];
        long n = cb_targets_for_qq(&current->mimes, t, sizeof(t) / sizeof(*t));
        cb_x11_reply(r, XA_ATOM, 32, t, n);
        return;
    }
    char *target = cb_x11_name(r->target);
    int to_gnome = 0;
    const char *mime = target ? cb_mime_for_target(&current->mimes, target, &to_gnome) : NULL;
    int fd = mime ? cb_wl_receive(current, mime) : -1;
    if (fd < 0) {
        LOG("QQ paste %s: not available", target ? target : "?");
        cb_x11_refuse(r);
        free(target);
        return;
    }
    struct read_job *j = calloc(1, sizeof(*j));
    j->req = *r;
    j->target = target;
    j->mime = strdup(mime);
    j->fd = fd;
    j->to_gnome = to_gnome;
    if (spawn(reader, j) < 0) {
        close(fd);
        cb_x11_refuse(r);
        free(j->target);
        free(j->mime);
        free(j);
    }
}

static void finish_reads(void)
{
    pthread_mutex_lock(&done_mu);
    struct read_job *j = done;
    done = NULL;
    pthread_mutex_unlock(&done_mu);
    while (j) {
        struct read_job *next = j->next;
        if (!j->complete) {
            LOG("QQ paste %s: %s did not arrive within %d ms", j->target, j->mime, READ_TIMEOUT_MS);
            cb_x11_refuse(&j->req);
        } else {
            unsigned char *data = j->data;
            long len = j->len;
            if (j->to_gnome) {
                data = (unsigned char *)cb_uris_to_gnome(j->data, j->len, &len);
                free(j->data);
            }
            cb_x11_reply(&j->req, j->req.target, 8, data, len);
            LOG("QQ paste %s <- %s: %ld bytes", j->target, j->mime, len);
            j->data = data;
        }
        free(j->data);
        free(j->target);
        free(j->mime);
        free(j);
        j = next;
    }
}

/* ---------------- 主循环 ---------------- */

void *cb_worker(void *arg)
{
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    int xfd = cb_x11_open();
    if (xfd < 0) {
        LOG("cannot connect to X11 (DISPLAY=%s)", getenv("DISPLAY"));
        return NULL;
    }
    int wfd = cb_wl_open();
    if (wfd < 0)
        return NULL;
    cb_go_native(cb_xwin, cb_A_CLIPBOARD);
    LOG("ready (pid %d, %s): QQ's clipboard is the Wayland clipboard", (int)getpid(), cb_wl_protocol());

    uint32_t seen = cb_copy_generation();
    for (;;) {
        while (wl_display_prepare_read(cb_wdpy) != 0)
            wl_display_dispatch_pending(cb_wdpy);
        wl_display_flush(cb_wdpy);

        struct pollfd p[3] = {
            { wfd, POLLIN, 0 },
            { cb_wake_fd(), POLLIN, 0 },
            { xfd, POLLIN, 0 },
        };
        int r = poll(p, 3, XPending(cb_xdpy) ? 0 : -1);
        if (r > 0 && (p[0].revents & POLLIN))
            wl_display_read_events(cb_wdpy);
        else
            wl_display_cancel_read(cb_wdpy);
        if (wl_display_dispatch_pending(cb_wdpy) < 0 || (p[0].revents & (POLLERR | POLLHUP))) {
            LOG("Wayland connection lost; QQ falls back to the X11 clipboard");
            cb_go_x11();
            return NULL;
        }
        if (p[1].revents & POLLIN) {
            char buf[64];
            (void)!read(cb_wake_fd(), buf, sizeof(buf));
        }
        cb_x11_drain();

        uint32_t g = cb_copy_generation();
        if (g != seen) {
            seen = g;
            on_qq_copy(g);
        }
        finish_reads();
        struct cb_request req;
        while (cb_pop_request(&req))
            handle_request(&req);
    }
}
