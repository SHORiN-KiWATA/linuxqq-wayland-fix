/*
 * libqq-clipbridge.so：让 QQ 自己的剪贴板代码直接用 Wayland 剪贴板。
 *
 * QQ 的剪贴板（wrapper.node 里的 ClipBoardHelper）只用 Xlib，而 QQ 的窗口是原生 Wayland 窗口，
 * 合成器不会替它同步 X11 剪贴板。实测它的用法：
 *   - 复制：在一条常驻的 X 连接上 XSetSelectionOwner(CLIPBOARD)，紧接着 XGetSelectionOwner 核对，
 *     由一个专门的线程回应 SelectionRequest；
 *   - 粘贴：在主线程的另一条常驻连接上 XConvertSelection(TARGETS)，列表里有 QQ 的私有格式
 *     （QQ_Unicode_RichEdit_Format：表情等）时再要私有格式，然后 XNextEvent 一直等 SelectionNotify，
 *     没有超时。文字、图片、文件由 QQ 里的 Chromium 直接从 Wayland 读。
 *
 * 这里把 QQ 对 CLIPBOARD 的这三个调用拦下来，不发给 X 服务器，真正的 X11 CLIPBOARD 从不触碰：
 *   - XSetSelectionOwner：记下 QQ 的窗口（它成了剪贴板的「主人」），后台线程直接向这个窗口要数据，
 *     通过 data-control 放到 Wayland 剪贴板上，所有格式原样提供，包括 QQ 的私有格式；
 *   - XGetSelectionOwner：按上面的记录回答；
 *   - XConvertSelection：后台线程把 Wayland 剪贴板上的内容写到 QQ 的窗口上再发 SelectionNotify；
 *     剪贴板上是 QQ 自己复制的内容时，照 X 服务器的做法把请求转给 QQ 自己回应。
 * X11 程序与 QQ 之间由合成器同步，和其它原生 Wayland 程序一样。每个 QQ 进程（多账号、频道）
 * 只和 Wayland 打交道，谁后复制谁生效，不会互相争抢。
 *
 * 合成器两种 data-control 都没有（如 GNOME）时不拦截，QQ 照原样用 X11。
 *
 * 另外：QQ 的 Chromium Wayland 剪贴板数据源向粘贴方的管道 write() 时没有屏蔽 SIGPIPE；
 * 读端提前关闭时（读端可能是任何程序），Bugly 会把 SIGPIPE 当致命错误直接杀掉整个 QQ。
 * 这里把 SIGPIPE 强制为忽略：写失败只返回 EPIPE，Chromium 自己会记录并清理。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clipbridge.h"

/* ---------------- 进程判断 ---------------- */

/*
 * 返回 1：QQ 的主进程（包括频道这类 --loadapp 进程）且以原生 Wayland 运行。
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
static atomic_int native;                  /* 后台线程就绪，拦截生效 */
static atomic_ulong clipboard_atom;
static atomic_ulong proxy_window;          /* Wayland 上是别人的内容时，XGetSelectionOwner 回答这个窗口 */
static atomic_int foreign;

static pthread_mutex_t own_mu = PTHREAD_MUTEX_INITIALIZER;
static Window qq_owner;                    /* QQ 复制用的窗口；它的复制被别人替换后清空 */
static uint32_t copy_generation;           /* QQ 每复制一次 +1 */

#define MAX_REQUESTS 16
static pthread_mutex_t req_mu = PTHREAD_MUTEX_INITIALIZER;
static struct cb_request requests[MAX_REQUESTS];
static int req_head, req_count;

void cb_wake(void)
{
    char c = 1;
    if (wake_pipe[1] >= 0)
        (void)!write(wake_pipe[1], &c, 1);
}

int cb_wake_fd(void)
{
    return wake_pipe[0];
}

static int push_request(const struct cb_request *r)
{
    pthread_mutex_lock(&req_mu);
    int ok = req_count < MAX_REQUESTS;
    if (ok)
        requests[(req_head + req_count++) % MAX_REQUESTS] = *r;
    pthread_mutex_unlock(&req_mu);
    return ok;
}

int cb_pop_request(struct cb_request *out)
{
    pthread_mutex_lock(&req_mu);
    int ok = req_count > 0;
    if (ok) {
        *out = requests[req_head];
        req_head = (req_head + 1) % MAX_REQUESTS;
        --req_count;
    }
    pthread_mutex_unlock(&req_mu);
    return ok;
}

uint32_t cb_copy_generation(void)
{
    pthread_mutex_lock(&own_mu);
    uint32_t g = copy_generation;
    pthread_mutex_unlock(&own_mu);
    return g;
}

Window cb_qq_owner(void)
{
    pthread_mutex_lock(&own_mu);
    Window w = qq_owner;
    pthread_mutex_unlock(&own_mu);
    return w;
}

/* QQ 第 generation 次复制的内容在 Wayland 上被别人替换了。期间 QQ 又复制过就不算；返回被撤下的窗口。 */
Window cb_qq_lost(uint32_t generation)
{
    pthread_mutex_lock(&own_mu);
    Window w = None;
    if (copy_generation == generation) {
        w = qq_owner;
        qq_owner = None;
    }
    pthread_mutex_unlock(&own_mu);
    return w;
}

void cb_go_native(Window proxy, Atom clipboard)
{
    atomic_store(&proxy_window, proxy);
    atomic_store(&clipboard_atom, clipboard);
    atomic_store(&native, 1);
}

/* Wayland 连接断了：之后的复制粘贴交还给 X11 */
void cb_go_x11(void)
{
    atomic_store(&native, 0);
}

void cb_set_foreign(int present)
{
    atomic_store(&foreign, present);
}

static int intercepts(Display *dpy, Atom selection)
{
    return atomic_load(&native) && dpy != cb_xdpy && selection == (Atom)atomic_load(&clipboard_atom);
}

/* ---------------- Xlib 拦截 ---------------- */

EXPORT int XSetSelectionOwner(Display *dpy, Atom selection, Window owner, Time t)
{
    static int (*real)(Display *, Atom, Window, Time);
    if (!real)
        real = (int (*)(Display *, Atom, Window, Time))dlsym(RTLD_NEXT, "XSetSelectionOwner");
    if (!intercepts(dpy, selection))
        return real(dpy, selection, owner, t);

    pthread_mutex_lock(&own_mu);
    qq_owner = owner;
    if (owner != None)
        ++copy_generation;
    pthread_mutex_unlock(&own_mu);
    if (owner != None)
        cb_wake();
    return 1;
}

EXPORT Window XGetSelectionOwner(Display *dpy, Atom selection)
{
    static Window (*real)(Display *, Atom);
    if (!real)
        real = (Window (*)(Display *, Atom))dlsym(RTLD_NEXT, "XGetSelectionOwner");
    if (!intercepts(dpy, selection))
        return real(dpy, selection);

    Window w = cb_qq_owner();
    if (w == None && atomic_load(&foreign))
        w = (Window)atomic_load(&proxy_window);
    return w;
}

EXPORT int XConvertSelection(Display *dpy, Atom selection, Atom target, Atom property, Window requestor, Time t)
{
    static int (*real)(Display *, Atom, Atom, Atom, Window, Time);
    if (!real)
        real = (int (*)(Display *, Atom, Atom, Atom, Window, Time))dlsym(RTLD_NEXT, "XConvertSelection");
    if (intercepts(dpy, selection)) {
        struct cb_request r = { requestor, target, property != None ? property : target, t };
        if (push_request(&r)) {
            cb_wake();
            return 1;
        }
        LOG("too many pending pastes, passing one to X11");
    }
    return real(dpy, selection, target, property, requestor, t);
}

/* ---------------- SIGPIPE 防护 ----------------
 *
 * QQ 的 Chromium Wayland 剪贴板数据源向粘贴方的管道 write() 时没有屏蔽 SIGPIPE；
 * 读端提前关闭时（读端可能是任何程序），Bugly 的处理器会把 SIGPIPE 当致命错误
 * 直接杀掉整个 QQ（日志：fatalHandler signo: 13，栈在 __write）。
 * 这里在信号处置层面把 SIGPIPE 强制为忽略：写失败只返回 EPIPE，由写端自己处理。
 */
static sighandler_t (*real_signal_fn)(int, sighandler_t);
static int (*real_sigaction_fn)(int, const struct sigaction *, struct sigaction *);

EXPORT sighandler_t signal(int signum, sighandler_t handler)
{
    if (!real_signal_fn)
        real_signal_fn = dlsym(RTLD_NEXT, "signal");
    if (enabled && signum == SIGPIPE && handler != SIG_IGN)
        handler = SIG_IGN;
    return real_signal_fn(signum, handler);
}

EXPORT int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    if (!real_sigaction_fn)
        real_sigaction_fn = dlsym(RTLD_NEXT, "sigaction");
    if (enabled && signum == SIGPIPE && act && act->sa_handler != SIG_IGN) {
        struct sigaction ign = *act;
        ign.sa_handler = SIG_IGN;
        ign.sa_flags &= ~(SA_RESETHAND | SA_NODEFER);
        return real_sigaction_fn(signum, &ign, oldact);
    }
    return real_sigaction_fn(signum, act, oldact);
}

__attribute__((constructor))
static void init(void)
{
    /* QQ_CLIPBOARD_FIX_DISABLE=1：不启用（排查问题用）；
     * QQ_CLIPBOARD_FIX_FORCE=1：强制启用（任意进程 / KDE 上绕过自动禁用，仅供测试）。 */
    const char *off = getenv("QQ_CLIPBOARD_FIX_DISABLE");
    if ((off && *off && strcmp(off, "0")) || !getenv("WAYLAND_DISPLAY") || !getenv("DISPLAY"))
        return;
    if (!getenv("QQ_CLIPBOARD_FIX_FORCE")) {
        /* KDE Plasma 自己会把 XWayland 的剪贴板与 Wayland 双向同步（KWin/Klipper），
         * QQ 的 X11 剪贴板本来就能互通，KDE 默认不启用。 */
        const char *de = getenv("XDG_CURRENT_DESKTOP");
        const char *kde = getenv("KDE_FULL_SESSION");
        if ((de && strstr(de, "KDE")) || (kde && *kde))
            return;
        if (!is_qq_main_on_wayland())
            return;
    }
    /* QQ 的剪贴板代码与后台线程并发使用 Xlib；必须在任何 Xlib 调用之前初始化线程支持。 */
    XInitThreads();
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    pthread_t t;
    if (pthread_create(&t, NULL, cb_worker, NULL) == 0) {
        pthread_detach(t);
        enabled = 1;
        signal(SIGPIPE, SIG_IGN);
    }
}
