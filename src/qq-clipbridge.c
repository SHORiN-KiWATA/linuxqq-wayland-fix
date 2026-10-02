/*
 * linuxqq-wayland-clipboard-fix：QQ（X11 剪贴板）与 Wayland 剪贴板的双向桥接。
 *
 * QQ 的剪贴板（wrapper.node 里的 ClipBoardHelper）只用 Xlib 实现。这里拦截
 * XSetSelectionOwner：QQ 一取得 CLIPBOARD，后台线程就用自己的 X 连接取 TARGETS，
 * 再通过 ext-data-control 在 Wayland 上提供同样的格式；别的程序粘贴时，
 * 再按需向 QQ 取对应格式的数据（支持 INCR）写给对方。
 *
 * 反方向：其它程序设置 Wayland 剪贴板时，后台线程用自己的 X 窗口接管 CLIPBOARD，
 * QQ 粘贴时再按需从 Wayland 读取（支持 INCR）。我们自己写到 Wayland 的内容带私有标记格式，
 * 不会被同步回来。
 *
 * 只在 QQ 主进程（/proc/self/exe 为 qq 且没有 --type=）、且 QQ 以原生 Wayland 运行时启用：
 * QQ 跑在 XWayland 下时，XWayland / xwayland-satellite 自己会同步剪贴板，再桥接会重复。
 *
 * Wayland 侧优先使用 ext-data-control-v1，没有时退回 wlr-data-control-unstable-v1。
 * 两者的请求和事件完全一致，只有「创建对象」的两个请求需要区分协议；
 * 其余调用都用 ext 的函数，libwayland 按对象自身的接口编码消息，对 wlr 对象同样正确。
 */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

/* 我们自己写到 Wayland 的内容带这个私有格式，用来识别、避免来回同步。 */
#define MARKER_MIME "application/x-qq-clipbridge"

#define LOG(...) do { fprintf(stderr, "[qq-clipbridge] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---------------- 进程判断 ---------------- */

/*
 * 返回 1：QQ 主进程且以原生 Wayland 运行。
 * Electron 的平台选择：命令行最后一个 --ozone-platform= 生效；没有时看
 * --ozone-platform-hint= / ELECTRON_OZONE_PLATFORM_HINT（Electron 38 起默认 auto，
 * 即有 Wayland 会话就用 Wayland）。
 */
static int is_qq_main_on_wayland(void)
{
    char exe[4096], args[65536];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return 0;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    if (!base || strcmp(base + 1, "qq"))
        return 0;
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = read(fd, args, sizeof(args) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    args[n] = 0;

    const char *platform = NULL, *hint = getenv("ELECTRON_OZONE_PLATFORM_HINT");
    for (ssize_t pos = 0; pos < n; pos += strlen(args + pos) + 1) {
        const char *a = args + pos;
        if (!strncmp(a, "--type=", 7))
            return 0;
        if (!strncmp(a, "--ozone-platform=", 17))
            platform = a + 17;
        else if (!strncmp(a, "--ozone-platform-hint=", 22))
            hint = a + 22;
    }
    if (!platform)
        platform = hint && *hint ? hint : "auto";
    if (!strcmp(platform, "wayland"))
        return 1;
    if (!strcmp(platform, "auto"))
        return getenv("WAYLAND_DISPLAY") != NULL;
    LOG("QQ is running on X11 (ozone=%s); XWayland syncs the clipboard itself, bridge disabled", platform);
    return 0;
}

/* ---------------- 与后台线程的通信 ---------------- */

static int enabled;
static int wake_pipe[2] = { -1, -1 };
static volatile uint32_t copy_generation;   /* QQ 每复制一次 +1 */

static void wake(void)
{
    char c = 1;
    if (wake_pipe[1] >= 0)
        (void)!write(wake_pipe[1], &c, 1);
}

/* ---------------- X11：从 QQ 取剪贴板数据 ---------------- */

static Display *xdpy;
static Window xwin;
static Atom A_CLIPBOARD, A_TARGETS, A_INCR, A_PROP, A_UTF8, A_GNOME_FILES;

/*
 * 请求方（SelectionRequest->requestor）可能在收到应答前就销毁窗口，这时对它
 * XChangeProperty / XSendEvent 会得到 BadWindow。libX11 的默认错误处理会直接
 * exit(1)，把整个 QQ 一起带走。后台线程的 X 连接只做剪贴板搬运，这里接管错误：
 * 属于本连接的忽略掉，其它连接的仍交给原有处理（没有就忽略，避免终止进程）。
 */
static XErrorHandler prev_x_error_handler;

static int clip_x_error(Display *dpy, XErrorEvent *e)
{
    if (dpy == xdpy)
        return 0;
    if (prev_x_error_handler)
        return prev_x_error_handler(dpy, e);
    return 0;
}

/* 等待期间收到的 SelectionRequest（QQ 来要我们提供的 Wayland 内容）先存起来，回主循环再处理。 */
#define MAX_PENDING 16
static XSelectionRequestEvent pending[MAX_PENDING];
static int n_pending;

/* 等某类事件，超时返回 0。 */
static int wait_event(int type, Window w, XEvent *ev, int timeout_ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        while (XPending(xdpy)) {
            XNextEvent(xdpy, ev);
            if (ev->type == type && ev->xany.window == w)
                return 1;
            if (ev->type == SelectionRequest && n_pending < MAX_PENDING)
                pending[n_pending++] = ev->xselectionrequest;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int left = timeout_ms - (int)((now.tv_sec - start.tv_sec) * 1000 +
                                      (now.tv_nsec - start.tv_nsec) / 1000000);
        if (left <= 0)
            return 0;
        struct pollfd p = { ConnectionNumber(xdpy), POLLIN, 0 };
        poll(&p, 1, left);
    }
}

/*
 * 把 CLIPBOARD 转换成 target，结果放在 *out（malloc），返回长度；失败返回 -1。
 * *type_out 返回实际类型（TARGETS 时为 ATOM）。
 */
static long fetch(Atom target, unsigned char **out, Atom *type_out)
{
    XEvent ev;
    *out = NULL;
    XDeleteProperty(xdpy, xwin, A_PROP);
    XConvertSelection(xdpy, A_CLIPBOARD, target, A_PROP, xwin, CurrentTime);
    XFlush(xdpy);
    if (!wait_event(SelectionNotify, xwin, &ev, 3000) || ev.xselection.property == None)
        return -1;

    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(xdpy, xwin, A_PROP, 0, 0x7fffffff, True, AnyPropertyType,
                           &type, &format, &nitems, &after, &data) != Success)
        return -1;

    if (type != A_INCR) {
        long len = nitems * (format == 32 ? sizeof(long) : format / 8);
        *out = malloc(len + 1);
        memcpy(*out, data, len);
        (*out)[len] = 0;
        *type_out = type;
        XFree(data);
        return len;
    }

    /* INCR：分块传输。删除属性表示「准备好接收下一块」，长度为 0 的块表示结束。 */
    XFree(data);
    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    for (;;) {
        if (!wait_event(PropertyNotify, xwin, &ev, 3000)) {
            free(buf);
            return -1;
        }
        if (ev.xproperty.atom != A_PROP || ev.xproperty.state != PropertyNewValue)
            continue;
        if (XGetWindowProperty(xdpy, xwin, A_PROP, 0, 0x7fffffff, True, AnyPropertyType,
                               &type, &format, &nitems, &after, &data) != Success) {
            free(buf);
            return -1;
        }
        size_t n = nitems * (format == 32 ? sizeof(long) : format / 8);
        if (n == 0) {
            XFree(data);
            break;
        }
        if (len + n + 1 > cap) {
            while (len + n + 1 > cap)
                cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, data, n);
        len += n;
        XFree(data);
    }
    buf[len] = 0;
    *out = buf;
    *type_out = type;
    return len;
}

/* ---------------- 格式映射 ---------------- */

#define MAX_MIMES 64
static char *offered[MAX_MIMES];
static int n_offered;

static void offer_add(const char *mime)
{
    for (int i = 0; i < n_offered; ++i)
        if (!strcmp(offered[i], mime))
            return;
    if (n_offered < MAX_MIMES)
        offered[n_offered++] = strdup(mime);
}

static void offers_clear(void)
{
    for (int i = 0; i < n_offered; ++i)
        free(offered[i]);
    n_offered = 0;
}

static int has_target(Atom *t, long n, Atom a)
{
    for (long i = 0; i < n; ++i)
        if (t[i] == a)
            return 1;
    return 0;
}

/*
 * 根据 QQ 提供的 X11 TARGETS 生成要在 Wayland 上提供的 MIME 列表。
 * QQ 复制图片/文件时给的是 text/html（<img src=缓存路径>）+ 文件列表 + image/png，
 * 很多程序会优先选 HTML，结果只拿到一个指向 QQ 缓存的标签。所以有文件时去掉 text/html，
 * 并把 uri-list 放在最前面。
 */
static int has_files;

static void build_offers(Atom *targets, long n)
{
    offers_clear();
    Atom uri = XInternAtom(xdpy, "text/uri-list", False);
    has_files = has_target(targets, n, A_GNOME_FILES) || has_target(targets, n, uri);
    if (has_files) {
        offer_add("text/uri-list");
        offer_add("x-special/gnome-copied-files");
    }
    for (long i = 0; i < n; ++i) {
        char *name = XGetAtomName(xdpy, targets[i]);
        if (!name)
            continue;
        if (!strcmp(name, "UTF8_STRING") || !strcmp(name, "STRING") || !strcmp(name, "TEXT")) {
            offer_add("text/plain;charset=utf-8");
            offer_add("text/plain");
            offer_add("UTF8_STRING");
            offer_add("STRING");
            offer_add("TEXT");
        } else if (strchr(name, '/') && !(has_files && !strcmp(name, "text/html"))) {
            offer_add(name);
        }
        XFree(name);
    }
}

/* ---- 文件列表归一化：QQ 给的常是裸路径，转成 file:// URI ---- */

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap)
            *cap = *cap ? *cap * 2 : 256;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

/* 把一段「每行一个路径或 URI」的文本转成 URI 列表（行分隔符为 sep），跳过 copy/cut 行。 */
static char *to_uris(const unsigned char *in, long n, const char *sep, long *outlen)
{
    char *out = NULL;
    size_t len = 0, cap = 0;
    const char *p = (const char *)in, *end = p + n;
    while (p < end) {
        const char *e = memchr(p, '\n', end - p);
        if (!e)
            e = end;
        const char *q = e;
        while (q > p && (q[-1] == '\r' || q[-1] == ' '))
            --q;
        size_t l = q - p;
        if (l && !(l == 4 && !memcmp(p, "copy", 4)) && !(l == 3 && !memcmp(p, "cut", 3))) {
            if (*p == '/') {
                append(&out, &len, &cap, "file://", 7);
                for (const char *c = p; c < q; ++c) {
                    unsigned char ch = *c;
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || strchr("/-_.~!$&'()*+,;=:@", ch)) {
                        append(&out, &len, &cap, c, 1);
                    } else {
                        char hex[4];
                        snprintf(hex, sizeof(hex), "%%%02X", ch);
                        append(&out, &len, &cap, hex, 3);
                    }
                }
            } else {
                append(&out, &len, &cap, p, l);
            }
            append(&out, &len, &cap, sep, strlen(sep));
        }
        p = e + 1;
    }
    *outlen = len;
    return out;
}

/* QQ 当前复制的文件是否都存在。 */
static int files_exist(void)
{
    unsigned char *raw;
    Atom type;
    long n = fetch(A_GNOME_FILES, &raw, &type);
    if (n < 0)
        n = fetch(XInternAtom(xdpy, "text/uri-list", False), &raw, &type);
    if (n < 0)
        return 0;
    int ok = 1, any = 0;
    char *save = NULL;
    for (char *line = strtok_r((char *)raw, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (!strcmp(line, "copy") || !strcmp(line, "cut") || !*line)
            continue;
        char path[4096];
        const char *p = line;
        if (!strncmp(p, "file://", 7))
            p += 7;
        /* 解码 %XX */
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

/* Wayland 侧请求某个 MIME 时，决定向 QQ 要哪个 X11 target，以及是否需要转换。 */
static long fetch_for_mime(const char *mime, unsigned char **out)
{
    Atom type;
    if (!strncmp(mime, "text/plain", 10) || !strcmp(mime, "UTF8_STRING") ||
        !strcmp(mime, "STRING") || !strcmp(mime, "TEXT")) {
        long n = fetch(A_UTF8, out, &type);
        if (n >= 0)
            return n;
    }
    if (!strcmp(mime, "text/uri-list") || !strcmp(mime, "x-special/gnome-copied-files")) {
        unsigned char *raw;
        long n = fetch(A_GNOME_FILES, &raw, &type);
        if (n < 0)
            n = fetch(XInternAtom(xdpy, "text/uri-list", False), &raw, &type);
        if (n < 0)
            return n;
        long len;
        char *uris;
        if (!strcmp(mime, "text/uri-list")) {
            uris = to_uris(raw, n, "\r\n", &len);
        } else {
            char *list = to_uris(raw, n, "\n", &len);
            uris = NULL;
            size_t l = 0, cap = 0;
            append(&uris, &l, &cap, "copy\n", 5);
            if (list && len > 0)
                append(&uris, &l, &cap, list, len - 1);   /* 去掉末尾换行 */
            free(list);
            len = l;
        }
        free(raw);
        *out = (unsigned char *)uris;
        return uris ? len : -1;
    }
    return fetch(XInternAtom(xdpy, mime, False), out, &type);
}

/* ---------------- Wayland：ext-data-control ---------------- */

static struct wl_display *wdpy;
static struct wl_seat *seat;
static struct ext_data_control_manager_v1 *dcm;     /* ext 管理器 */
static struct zwlr_data_control_manager_v1 *wlr_dcm; /* wlr 管理器（没有 ext 时用） */
static struct ext_data_control_device_v1 *device;
static struct ext_data_control_source_v1 *source;

static void write_all(int fd, const unsigned char *p, long n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += w;
        n -= w;
    }
}

static void source_send(void *data, struct ext_data_control_source_v1 *src,
                        const char *mime, int32_t fd)
{
    (void)data; (void)src;
    unsigned char *buf;
    long n = fetch_for_mime(mime, &buf);
    if (n >= 0) {
        write_all(fd, buf, n);
        LOG("paste request %s: %ld bytes", mime, n);
    } else {
        LOG("paste request %s: QQ did not provide data", mime);
    }
    free(buf);
    close(fd);
}

static void source_cancelled(void *data, struct ext_data_control_source_v1 *src)
{
    (void)data;
    ext_data_control_source_v1_destroy(src);
    if (src == source)
        source = NULL;
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send,
    .cancelled = source_cancelled,
};

/* ---------------- 第二阶段：Wayland → QQ ---------------- */

/*
 * 其它程序设置 Wayland 剪贴板时，compositor 先发 data_offer（随后若干 offer 事件列出格式），
 * 再发 selection。我们用自己的 X 窗口接管 X11 CLIPBOARD；QQ 来要时按需从 Wayland 读取。
 */
struct offer_info {
    struct ext_data_control_offer_v1 *offer;
    char *mimes[MAX_MIMES];
    int n;
};

static struct offer_info *current;   /* 当前提供给 X11 的 Wayland 内容 */
static Window last_owner;            /* 上次处理 Wayland 变化（或 QQ 复制）时 X11 CLIPBOARD 的主人 */
static volatile Window qq_owner;     /* QQ 自己设置 CLIPBOARD 时用的窗口 */

/* XFixes：别的 X11 程序每次设置 CLIPBOARD（即复制）都会通知，同一个窗口连续复制也会 */
static int fixes_event_base = -1;
static Window x11_copy_owner;        /* 最近一次复制的普通 X11 程序的窗口 */
static struct timespec x11_copy_at;
typedef struct {                     /* XFixesSelectionNotifyEvent */
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    int subtype;
    Window owner;
    Atom selection;
    Time timestamp, selection_timestamp;
} FixesSelectionNotify;

/*
 * X11 窗口 w 是不是合成器的 X11 剪贴板代理（wlroots / KWin 的 xwm 在合成器进程里，
 * niri 用的是单独的 xwayland-satellite）。用 XRes 查出窗口所属的进程来判断；
 * libXRes 运行时 dlopen，拿不到时当作普通程序（宁可不抢，也不把别人的剪贴板弄坏）。
 */
typedef struct { XID client; unsigned int mask; } ResClientIdSpec;           /* XResClientIdSpec */
typedef struct { ResClientIdSpec spec; long length; void *value; } ResClientIdValue;  /* XResClientIdValue */
#define RES_CLIENT_ID_PID_MASK 2                                                 /* XRES_CLIENT_ID_PID_MASK */

static pid_t window_pid(Window w)
{
    static int (*query)(Display *, long, ResClientIdSpec *, long *, ResClientIdValue **);
    static pid_t (*get_pid)(ResClientIdValue *);
    static void (*destroy)(long, ResClientIdValue *);
    static int loaded;

    if (!loaded) {
        loaded = 1;
        void *h = dlopen("libXRes.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            query = dlsym(h, "XResQueryClientIds");
            get_pid = dlsym(h, "XResGetClientPid");
            destroy = dlsym(h, "XResClientIdsDestroy");
        }
    }
    if (!query || !get_pid || !destroy)
        return -1;
    ResClientIdSpec spec = { w, RES_CLIENT_ID_PID_MASK };
    long n = 0;
    ResClientIdValue *ids = NULL;
    pid_t pid = -1;
    if (query(xdpy, 1, &spec, &n, &ids) == Success) {
        for (long i = 0; i < n && pid < 0; i++)
            pid = get_pid(&ids[i]);
        destroy(n, ids);
    }
    return pid;
}

static int is_bridge_window(Window w)
{
    pid_t pid = window_pid(w);
    if (pid <= 0)
        return 0;

    static pid_t compositor = -1;
    if (compositor < 0) {
        struct ucred cred;
        socklen_t len = sizeof(cred);
        compositor = getsockopt(wl_display_get_fd(wdpy), SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0
                         ? cred.pid : 0;
    }
    if (pid == compositor)
        return 1;

    char path[64], comm[64] = "";
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(comm, sizeof(comm), f))
            comm[strcspn(comm, "\n")] = 0;
        fclose(f);
    }
    return !strcmp(comm, "xwayland-satell");   /* comm 最长 15 个字符 */
}

/* 记下「普通 X11 程序（包括 QQ 自己）刚复制」，供 dev_selection 判断随后的 Wayland 变化是不是同步过来的 */
static void note_x11_owner(Window owner)
{
    if (owner == None || owner == xwin || (owner != qq_owner && is_bridge_window(owner)))
        return;
    x11_copy_owner = owner;
    clock_gettime(CLOCK_MONOTONIC, &x11_copy_at);
}

/* 先处理已经到达的 XFixes 通知（主循环每轮先处理 Wayland 事件，再处理 X 事件） */
static void drain_fixes(void)
{
    XEvent ev;
    while (fixes_event_base >= 0 && XCheckTypedEvent(xdpy, fixes_event_base, &ev))
        note_x11_owner(((FixesSelectionNotify *)&ev)->owner);
}

static int ms_since(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)((now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000);
}

static void info_free(struct offer_info *info)
{
    if (!info)
        return;
    ext_data_control_offer_v1_destroy(info->offer);
    for (int i = 0; i < info->n; ++i)
        free(info->mimes[i]);
    free(info);
}

static int info_has(struct offer_info *info, const char *mime)
{
    for (int i = 0; info && i < info->n; ++i)
        if (!strcmp(info->mimes[i], mime))
            return 1;
    return 0;
}

static void offer_mime(void *data, struct ext_data_control_offer_v1 *offer, const char *mime)
{
    (void)offer;
    struct offer_info *info = data;
    if (info->n < MAX_MIMES)
        info->mimes[info->n++] = strdup(mime);
}

static const struct ext_data_control_offer_v1_listener offer_listener = { .offer = offer_mime };

static void dev_data_offer(void *d, struct ext_data_control_device_v1 *dev,
                           struct ext_data_control_offer_v1 *offer)
{
    (void)d; (void)dev;
    struct offer_info *info = calloc(1, sizeof(*info));
    info->offer = offer;
    ext_data_control_offer_v1_add_listener(offer, &offer_listener, info);
}

static void dev_selection(void *d, struct ext_data_control_device_v1 *dev,
                          struct ext_data_control_offer_v1 *offer)
{
    (void)d; (void)dev;
    struct offer_info *info = offer ? ext_data_control_offer_v1_get_user_data(offer) : NULL;

    if (info && info_has(info, MARKER_MIME)) {   /* 是我们自己从 QQ 转过去的 */
        info_free(info);
        return;
    }
    info_free(current);
    current = info;
    if (!info)
        return;

    /* 先往返一次（XGetSelectionOwner），把已经发出的 XFixes 通知读进队列，再处理它们 */
    Window owner = XGetSelectionOwner(xdpy, A_CLIPBOARD);
    drain_fixes();

    /*
     * 连上 Wayland 时会先收到一次「当前剪贴板」。这时若 X11 剪贴板已经有主人（比如 QQ 刚复制过），
     * 它的内容可能更新，不去抢；之后的变化通知一定比 X11 上的新，照常接管。
     */
    static int initial = 1;
    if (initial) {
        initial = 0;
        last_owner = owner;
        if (owner != None && owner != xwin) {
            LOG("startup: X11 clipboard already owned, keep it");
            return;
        }
    }

    /*
     * 这次 Wayland 变化是合成器（xwayland-satellite、KWin、wlroots）在同步某个 X11 程序的复制
     * （X11 窗口有焦点时它们会这样做）时，X11 一侧本来就是对的，不能去抢：抢了之后合成器又会
     * 把我们同步回 Wayland，来回循环，谁都读不到内容。判断依据：
     *   - 普通 X11 程序刚设置过 CLIPBOARD（XFixes 通知，1.5 秒内）且仍是主人，一次复制可能被同步多次；
     *   - 格式里有 TIMESTAMP / TARGETS / MULTIPLE 这类 X11 才有的名字（satellite 会原样转过来）；
     *   - 没有 XFixes 时退而求其次：主人刚换成了别的 X11 程序。
     * 主人若是合成器自己的代理窗口（它在把 Wayland 内容同步给 X11，但不一定能用），照常接管。
     * QQ 自己刚复制时同理：随后 1.5 秒内的 Wayland 变化是在同步 QQ 的复制，不能从 QQ 手里抢。
     */
    int x11_names = info_has(info, "TIMESTAMP") || info_has(info, "TARGETS") || info_has(info, "MULTIPLE");
    int recent = owner == x11_copy_owner && ms_since(&x11_copy_at) < 1500;
    if (owner != None && owner != xwin &&
        (recent ||
         (owner != qq_owner && (x11_names || (fixes_event_base < 0 && owner != last_owner)) &&
          !is_bridge_window(owner)))) {
        last_owner = owner;
        LOG("Wayland clipboard follows X11 client 0x%lx, leave X11 as is", (unsigned long)owner);
        return;
    }

    /*
     * 已经是 X11 剪贴板的主人时只更新内容，不重新 SetSelectionOwner：每次换主人，satellite、
     * fcitx5 等监听 X11 剪贴板的程序都会来读、甚至写回 Wayland，又触发我们，形成来回。
     * QQ 粘贴时总会来要数据，拿到的就是最新的内容。
     */
    if (owner == xwin) {
        LOG("Wayland clipboard changed, X11 already ours: updated content");
        return;
    }
    XSetSelectionOwner(xdpy, A_CLIPBOARD, xwin, CurrentTime);
    XFlush(xdpy);
    last_owner = xwin;
    char list[512] = "";
    for (int i = 0; i < info->n && strlen(list) + strlen(info->mimes[i]) + 2 < sizeof(list); ++i) {
        strcat(list, info->mimes[i]);
        strcat(list, " ");
    }
    LOG("Wayland clipboard changed -> X11 for QQ: %s", list);
}

static void dev_finished(void *d, struct ext_data_control_device_v1 *dev)
{ (void)d; ext_data_control_device_v1_destroy(dev); device = NULL; }

static void dev_primary(void *d, struct ext_data_control_device_v1 *dev,
                        struct ext_data_control_offer_v1 *offer)
{
    (void)d; (void)dev;
    if (offer)
        info_free(ext_data_control_offer_v1_get_user_data(offer));
}

/* 从当前 Wayland 内容读取某个 MIME 的数据。 */
static long wl_receive(const char *mime, unsigned char **out)
{
    *out = NULL;
    if (!current)
        return -1;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return -1;
    ext_data_control_offer_v1_receive(current->offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(wdpy);

    size_t cap = 65536, len = 0;
    unsigned char *buf = malloc(cap);
    for (;;) {
        struct pollfd p = { fds[0], POLLIN, 0 };
        if (poll(&p, 1, 3000) <= 0)
            break;
        if (len + 65536 > cap)
            buf = realloc(buf, cap *= 2);
        ssize_t r = read(fds[0], buf + len, cap - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        len += r;
    }
    close(fds[0]);
    *out = buf;
    return len;
}

static int is_text_name(const char *n)
{
    return !strcmp(n, "UTF8_STRING") || !strcmp(n, "STRING") || !strcmp(n, "TEXT") ||
           !strncmp(n, "text/plain", 10);
}

/* QQ 要某个 X11 target 时，从 Wayland 的哪个 MIME 取。返回 NULL 表示提供不了。 */
static const char *mime_for_target(const char *name, int *to_gnome)
{
    *to_gnome = 0;
    if (is_text_name(name)) {
        static const char *pref[] = { "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT" };
        for (size_t i = 0; i < sizeof(pref) / sizeof(*pref); ++i)
            if (info_has(current, pref[i]))
                return pref[i];
        return NULL;
    }
    if (!strcmp(name, "x-special/gnome-copied-files") && !info_has(current, name) &&
        info_has(current, "text/uri-list")) {
        *to_gnome = 1;
        return "text/uri-list";
    }
    return info_has(current, name) ? name : NULL;
}

/* 当前 Wayland 内容对应的 X11 TARGETS。 */
static long build_x_targets(Atom *out, long cap)
{
    long n = 0;
    out[n++] = A_TARGETS;
    /* 带上私有标记：合成器若把我们的 X11 剪贴板同步回 Wayland，dev_selection 能认出来并忽略 */
    out[n++] = XInternAtom(xdpy, MARKER_MIME, False);
    int text = 0, uris = 0;
    for (int i = 0; current && i < current->n && n < cap - 8; ++i) {
        const char *m = current->mimes[i];
        if (is_text_name(m)) {
            text = 1;
            continue;
        }
        if (!strcmp(m, "text/uri-list"))
            uris = 1;
        if (strchr(m, '/'))
            out[n++] = XInternAtom(xdpy, m, False);
    }
    if (text) {
        out[n++] = A_UTF8;
        out[n++] = XInternAtom(xdpy, "STRING", False);
        out[n++] = XInternAtom(xdpy, "TEXT", False);
        out[n++] = XInternAtom(xdpy, "text/plain;charset=utf-8", False);
        out[n++] = XInternAtom(xdpy, "text/plain", False);
    }
    if (uris && !info_has(current, "x-special/gnome-copied-files"))
        out[n++] = A_GNOME_FILES;
    return n;
}

#define INCR_CHUNK (64 * 1024)

/* 把数据写给请求方；超过一个请求能装下的大小时走 INCR。返回是否成功。 */
static int send_data(XSelectionRequestEvent *req, Atom prop, XSelectionEvent *notify,
                     const unsigned char *data, long len)
{
    long max = XExtendedMaxRequestSize(xdpy);
    if (!max)
        max = XMaxRequestSize(xdpy);
    if (len < max * 4 - 1024) {
        XChangeProperty(xdpy, req->requestor, prop, req->target, 8, PropModeReplace, data, len);
        XSendEvent(xdpy, req->requestor, False, 0, (XEvent *)notify);
        XFlush(xdpy);
        return 1;
    }

    /* INCR：先告诉对方总大小，之后每次对方删掉属性就写下一块，最后写一个空块。 */
    long total = len;
    XSelectInput(xdpy, req->requestor, PropertyChangeMask);
    XChangeProperty(xdpy, req->requestor, prop, A_INCR, 32, PropModeReplace,
                    (unsigned char *)&total, 1);
    XSendEvent(xdpy, req->requestor, False, 0, (XEvent *)notify);
    XFlush(xdpy);
    long off = 0;
    for (;;) {
        XEvent ev;
        if (!wait_event(PropertyNotify, req->requestor, &ev, 3000))
            break;
        if (ev.xproperty.atom != prop || ev.xproperty.state != PropertyDelete)
            continue;
        long n = len - off < INCR_CHUNK ? len - off : INCR_CHUNK;
        XChangeProperty(xdpy, req->requestor, prop, req->target, 8, PropModeReplace, data + off, n);
        XFlush(xdpy);
        if (n == 0)
            break;
        off += n;
    }
    XSelectInput(xdpy, req->requestor, NoEventMask);
    XFlush(xdpy);
    return off == len;
}

static void serve_request(XSelectionRequestEvent *req)
{
    Atom prop = req->property != None ? req->property : req->target;
    XSelectionEvent notify = {
        .type = SelectionNotify, .display = xdpy, .requestor = req->requestor,
        .selection = req->selection, .target = req->target, .property = prop, .time = req->time,
    };

    if (!current || req->selection != A_CLIPBOARD) {
        notify.property = None;
        XSendEvent(xdpy, req->requestor, False, 0, (XEvent *)&notify);
        XFlush(xdpy);
        return;
    }

    if (req->target == A_TARGETS) {
        Atom t[MAX_MIMES + 16];
        long n = build_x_targets(t, sizeof(t) / sizeof(*t));
        XChangeProperty(xdpy, req->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)t, n);
        XSendEvent(xdpy, req->requestor, False, 0, (XEvent *)&notify);
        XFlush(xdpy);
        return;
    }

    char *name = XGetAtomName(xdpy, req->target);
    int to_gnome;
    const char *mime = name ? mime_for_target(name, &to_gnome) : NULL;
    unsigned char *data = NULL;
    long len = mime ? wl_receive(mime, &data) : -1;
    if (len >= 0 && to_gnome) {
        /* text/uri-list（CRLF 分隔）→ x-special/gnome-copied-files（"copy\n" + 每行一个 URI） */
        char *g = NULL;
        size_t gl = 0, gc = 0;
        append(&g, &gl, &gc, "copy", 4);
        char *save = NULL;
        data = realloc(data, len + 1);
        data[len] = 0;
        for (char *l = strtok_r((char *)data, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
            if (*l == '#' || !*l)
                continue;
            append(&g, &gl, &gc, "\n", 1);
            append(&g, &gl, &gc, l, strlen(l));
        }
        free(data);
        data = (unsigned char *)g;
        len = gl;
    }
    if (len < 0) {
        notify.property = None;
        XSendEvent(xdpy, req->requestor, False, 0, (XEvent *)&notify);
        XFlush(xdpy);
        LOG("QQ paste %s: not available", name ? name : "?");
    } else {
        int ok = send_data(req, prop, &notify, data, len);
        LOG("QQ paste %s <- %s: %ld bytes%s", name, mime, len, ok ? "" : " (INCR incomplete)");
    }
    free(data);
    if (name)
        XFree(name);
}

static void handle_x_events(void)
{
    while (n_pending > 0) {
        XSelectionRequestEvent req = pending[--n_pending];
        serve_request(&req);
    }
    while (XPending(xdpy)) {
        XEvent ev;
        XNextEvent(xdpy, &ev);
        if (ev.type == SelectionRequest)
            serve_request(&ev.xselectionrequest);
        else if (fixes_event_base >= 0 && ev.type == fixes_event_base)
            note_x11_owner(((FixesSelectionNotify *)&ev)->owner);
        while (n_pending > 0) {
            XSelectionRequestEvent req = pending[--n_pending];
            serve_request(&req);
        }
    }
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = dev_data_offer,
    .selection = dev_selection,
    .finished = dev_finished,
    .primary_selection = dev_primary,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)d;
    if (!strcmp(iface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, ext_data_control_manager_v1_interface.name))
        dcm = wl_registry_bind(r, name, &ext_data_control_manager_v1_interface, 1);
    else if (!strcmp(iface, zwlr_data_control_manager_v1_interface.name))
        wlr_dcm = wl_registry_bind(r, name, &zwlr_data_control_manager_v1_interface, 2 <= version ? 2 : 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* QQ 刚复制完：取 TARGETS，在 Wayland 上重新提供。 */
static void on_qq_copy(void)
{
    unsigned char *data = NULL;
    Atom type;
    long n = -1;
    /* QQ 刚声明拥有剪贴板时，它的 X 事件循环可能还没来得及应答，失败就稍等重试。 */
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt)
            usleep(200 * 1000);
        free(data);
        n = fetch(A_TARGETS, &data, &type);
        if (n > 0)
            break;
    }
    if (n <= 0) {
        LOG("QQ copied but TARGETS unavailable");
        free(data);
        return;
    }
    last_owner = XGetSelectionOwner(xdpy, A_CLIPBOARD);
    build_offers((Atom *)data, n / sizeof(Atom));
    free(data);

    /*
     * QQ 复制图片时给的文件路径指向它的缓存，原图没下载时文件并不存在。
     * 这时不提供文件格式（否则对方会拿到一个死路径），只提供 QQ 给的图片数据。
     */
    if (has_files && !files_exist()) {
        int had_png = 0;
        for (int i = 0; i < n_offered; ++i)
            if (!strcmp(offered[i], "image/png"))
                had_png = 1;
        if (had_png) {
            offers_clear();
            offer_add("image/png");
            LOG("QQ copied files that do not exist locally; offering image data only");
        }
    }
    if (n_offered)
        offer_add(MARKER_MIME);
    if (!n_offered) {
        LOG("QQ copied but no transferable format");
        return;
    }

    if (source)
        ext_data_control_source_v1_destroy(source);
    source = dcm ? ext_data_control_manager_v1_create_data_source(dcm)
                 : (struct ext_data_control_source_v1 *)zwlr_data_control_manager_v1_create_data_source(wlr_dcm);
    ext_data_control_source_v1_add_listener(source, &source_listener, NULL);
    char list[1024] = "";
    for (int i = 0; i < n_offered; ++i) {
        ext_data_control_source_v1_offer(source, offered[i]);
        if (strlen(list) + strlen(offered[i]) + 2 < sizeof(list)) {
            strcat(list, offered[i]);
            strcat(list, " ");
        }
    }
    ext_data_control_device_v1_set_selection(device, source);
    wl_display_flush(wdpy);
    LOG("QQ copied -> Wayland: %s", list);
}

static void *worker(void *arg)
{
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    xdpy = XOpenDisplay(NULL);
    wdpy = wl_display_connect(NULL);
    if (!xdpy || !wdpy) {
        LOG("cannot connect: X=%p wayland=%p", (void *)xdpy, (void *)wdpy);
        return NULL;
    }
    prev_x_error_handler = XSetErrorHandler(clip_x_error);
    xwin = XCreateSimpleWindow(xdpy, DefaultRootWindow(xdpy), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(xdpy, xwin, PropertyChangeMask);
    A_CLIPBOARD = XInternAtom(xdpy, "CLIPBOARD", False);
    A_TARGETS = XInternAtom(xdpy, "TARGETS", False);
    A_INCR = XInternAtom(xdpy, "INCR", False);
    A_PROP = XInternAtom(xdpy, "QQ_CLIPBRIDGE", False);
    A_UTF8 = XInternAtom(xdpy, "UTF8_STRING", False);
    A_GNOME_FILES = XInternAtom(xdpy, "x-special/gnome-copied-files", False);

    /* libXfixes 运行时 dlopen，不增加链接依赖；没有时退回较粗的判断 */
    void *fx = dlopen("libXfixes.so.3", RTLD_LAZY | RTLD_LOCAL);
    Bool (*fx_query)(Display *, int *, int *) = fx ? dlsym(fx, "XFixesQueryExtension") : NULL;
    void (*fx_select)(Display *, Window, Atom, unsigned long) = fx ? dlsym(fx, "XFixesSelectSelectionInput") : NULL;
    int fx_event, fx_error;
    if (fx_query && fx_select && fx_query(xdpy, &fx_event, &fx_error)) {
        fixes_event_base = fx_event;   /* XFixesSelectionNotify = 事件基数 + 0 */
        fx_select(xdpy, DefaultRootWindow(xdpy), A_CLIPBOARD, 1 /* XFixesSetSelectionOwnerNotifyMask */);
    } else {
        LOG("XFixes unavailable, X11-copy detection is less precise");
    }

    struct wl_registry *reg = wl_display_get_registry(wdpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(wdpy);
    if (!seat || (!dcm && !wlr_dcm)) {
        LOG("compositor supports neither ext-data-control nor wlr-data-control "
            "(e.g. GNOME); clipboard bridge disabled");
        return NULL;
    }
    device = dcm ? ext_data_control_manager_v1_get_data_device(dcm, seat)
                 : (struct ext_data_control_device_v1 *)zwlr_data_control_manager_v1_get_data_device(wlr_dcm, seat);
    ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
    wl_display_roundtrip(wdpy);
    LOG("ready (pid %d, %s)", (int)getpid(), dcm ? "ext-data-control" : "wlr-data-control");

    uint32_t seen = 0;   /* 从 0 开始：线程就绪前 QQ 已经复制过的也要处理 */
    for (;;) {
        while (wl_display_prepare_read(wdpy) != 0)
            wl_display_dispatch_pending(wdpy);
        wl_display_flush(wdpy);

        struct pollfd p[3] = {
            { wl_display_get_fd(wdpy), POLLIN, 0 },
            { wake_pipe[0], POLLIN, 0 },
            { ConnectionNumber(xdpy), POLLIN, 0 },
        };
        int r = poll(p, 3, -1);
        if (r > 0 && (p[0].revents & POLLIN))
            wl_display_read_events(wdpy);
        else
            wl_display_cancel_read(wdpy);
        wl_display_dispatch_pending(wdpy);

        if (p[1].revents & POLLIN) {
            char buf[64];
            (void)!read(wake_pipe[0], buf, sizeof(buf));
        }
        handle_x_events();
        if (copy_generation != seen) {
            seen = copy_generation;
            on_qq_copy();
        }
    }
    return NULL;
}

/* ---------------- Xlib 拦截 ---------------- */

int XSetSelectionOwner(Display *dpy, Atom selection, Window owner, Time t)
{
    static int (*real)(Display *, Atom, Window, Time);
    if (!real)
        real = (int (*)(Display *, Atom, Window, Time))dlsym(RTLD_NEXT, "XSetSelectionOwner");
    int r = real(dpy, selection, owner, t);

    if (enabled && r && dpy != xdpy && owner != None) {
        /* 每个 Display 的 CLIPBOARD atom 只查一次 */
        static Display *cached_dpy;
        static Atom cached_clipboard;
        if (cached_dpy != dpy) {
            cached_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
            cached_dpy = dpy;
        }
        if (selection == cached_clipboard) {
            /* 当场记下「QQ 刚复制」：合成器把它同步回 Wayland 的事件可能比 XFixes 通知先到 */
            qq_owner = owner;
            x11_copy_owner = owner;
            clock_gettime(CLOCK_MONOTONIC, &x11_copy_at);
            __atomic_add_fetch(&copy_generation, 1, __ATOMIC_SEQ_CST);
            wake();
        }
    }
    return r;
}

__attribute__((constructor))
static void init(void)
{
    /* QQ_CLIPBOARD_FIX_DISABLE=1：不启用（排查问题用）；QQ_CLIPBOARD_FIX_FORCE=1：任意进程都启用（仅供测试） */
    const char *off = getenv("QQ_CLIPBOARD_FIX_DISABLE");
    if ((off && *off && strcmp(off, "0")) || !getenv("WAYLAND_DISPLAY") || !getenv("DISPLAY"))
        return;
    if (!getenv("QQ_CLIPBOARD_FIX_FORCE") && !is_qq_main_on_wayland())
        return;
    /* 后台线程会开自己的 X 连接，与 QQ 的 Xlib 调用并发；必须在任何 Xlib 调用之前初始化线程支持。 */
    XInitThreads();
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0) {
        pthread_detach(t);
        enabled = 1;
    }
}
