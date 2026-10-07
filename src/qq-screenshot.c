/*
 * qq-screenshot：按截图键（Ctrl+Alt+A）时 QQ 不再闪退，并截到真实画面。
 *
 * QQ 截图前先执行 `echo $XDG_SESSION_TYPE`：是 wayland 就走 GNOME 专用的方式，
 * 否则用主程序里的 X11 代码对根窗口 XGetImage / XShmGetImage 截全屏。
 * 启动器为了让屏幕共享可用，给 QQ 的是 XDG_SESSION_TYPE=x11，于是走 X11 这条；
 * 而 Wayland 桌面下的 XWayland 是 rootless 的，根窗口没有内容，GetImage 必然 BadMatch。
 * Xlib 打印错误后返回 NULL，QQ 不检查就去读像素，段错误。
 *
 * 这里只处理对根窗口的截取（别的窗口照常交给 Xlib）：
 *   1. 在 Wayland 会话里（有 WAYLAND_DISPLAY），通过 wlr-screencopy 逐个截取 Wayland 输出，
 *      按 X 的显示器布局（XRandR monitors，名字与 wl_output 名字对应）拼成根窗口坐标系下的画面。
 *      XShmGetImage 在 rootless XWayland 上不报错、只给全黑，所以不能「X 失败了再截」；
 *   2. KDE（KWin）没有 wlr-screencopy：fork/exec qq-screenshot-helper（kde 模式），
 *      由它调 org.kde.KWin.ScreenShot2.CaptureWorkspace（helper 自己的 .desktop
 *      声明了受限接口，能通过 KWin 的权限检查），原始像素经管道传回后按显示器裁剪；
 *   3. GNOME 没有 wlr-screencopy 也没有 KWin：走 portal（helper 的 portal 模式调
 *      org.freedesktop.portal.Screenshot，interactive=false），helper 解码 PNG 后
 *      回传原始像素；GNOME 会弹它自己的截图对话框等用户确认；
 *   4. 其它情况照常调用 Xlib，但临时接管 X 错误（默认处理会直接退出进程）；
 *      仍然失败就给一张黑图，至少不闪退。真正的 X11 会话里截取会成功，行为不变。
 *
 * 另外：共享时批注 / 激光笔绘制窗口用的显示器矩形换算成逻辑尺寸（见文件末尾 XRRGetMonitors）。
 *
 * QQ_SCREENSHOT_FIX_DISABLE=1 关掉整个截图修复（含上面的换算）；
 * QQ_SCREENSHOT_KDE=0 只关 KDE 路径，=1 强制走 KDE（测试用）；
 * QQ_SCREENSHOT_PORTAL=1/0 强制开/关 portal 路径（默认仅 GNOME 会话启用）；
 * QQ_SCREENSHOT_HELPER 覆盖 helper 路径（测试用）。
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#define LOG(...) do { fprintf(stderr, "[qq-screenshot] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* 截图 helper 的默认安装路径（make 按 LIBEXECDIR 覆盖）。 */
#ifndef QQ_SCREENSHOT_HELPER
#define QQ_SCREENSHOT_HELPER "/usr/lib/linuxqq-wayland-fix/qq-screenshot-helper"
#endif

/* ---------------- 截取 Wayland 输出 ---------------- */

struct output {
    struct wl_output *wl;
    char name[64];
    uint32_t *pix; /* 截到的画面，0x00RRGGBB */
    int w, h;
    int mode_w, mode_h, scale; /* wl_output 的原生模式尺寸 / scale */
    int log_w, log_h;          /* xdg-output 的逻辑尺寸（分数缩放后） */
    struct zxdg_output_v1 *xdg;
    struct output *next;
};

struct capture {
    struct wl_display *dpy;
    struct wl_shm *shm;
    struct zwlr_screencopy_manager_v1 *mgr;
    struct zxdg_output_manager_v1 *xdg_mgr;
    struct output *outputs;
};

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char *make, const char *model, int32_t tr)
{
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{
    struct output *out = d;
    (void)o; (void)f; (void)r;
    out->mode_w = w;
    out->mode_h = h;
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t s)
{
    struct output *out = d;
    (void)o;
    out->scale = s > 0 ? s : 1;
}
static void out_name(void *d, struct wl_output *o, const char *name)
{
    struct output *out = d;
    (void)o;
    snprintf(out->name, sizeof out->name, "%s", name);
}
static void out_desc(void *d, struct wl_output *o, const char *s) { (void)d; (void)o; (void)s; }

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done,
    .scale = out_scale, .name = out_name, .description = out_desc,
};

static void reg_global(void *data, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
    struct capture *c = data;

    if (!strcmp(iface, wl_shm_interface.name)) {
        c->shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    } else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
        c->mgr = wl_registry_bind(r, id, &zwlr_screencopy_manager_v1_interface, ver < 3 ? ver : 3);
    } else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) {
        c->xdg_mgr = wl_registry_bind(r, id, &zxdg_output_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct output *o = calloc(1, sizeof *o);
        if (!o)
            return;
        o->wl = wl_registry_bind(r, id, &wl_output_interface, ver < 4 ? ver : 4);
        wl_output_add_listener(o->wl, &output_listener, o);
        o->next = c->outputs;
        c->outputs = o;
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }

static void xdg_out_pos(void *d, struct zxdg_output_v1 *x, int32_t px, int32_t py)
{ (void)d; (void)x; (void)px; (void)py; }
static void xdg_out_size(void *d, struct zxdg_output_v1 *x, int32_t w, int32_t h)
{
    struct output *out = d;
    (void)x;
    out->log_w = w;
    out->log_h = h;
}
static void xdg_out_done(void *d, struct zxdg_output_v1 *x) { (void)d; (void)x; }
static void xdg_out_str(void *d, struct zxdg_output_v1 *x, const char *s) { (void)d; (void)x; (void)s; }
static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_out_pos, .logical_size = xdg_out_size, .done = xdg_out_done,
    .name = xdg_out_str, .description = xdg_out_str,
};
static const struct wl_registry_listener registry_listener = { reg_global, reg_remove };

struct frame {
    uint32_t format, w, h, stride;
    int have_buffer, buffer_done, ready, failed, y_invert;
};

static int supported_format(uint32_t f)
{
    return f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_ARGB8888 ||
           f == WL_SHM_FORMAT_XBGR8888 || f == WL_SHM_FORMAT_ABGR8888;
}

static void fr_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
                      uint32_t w, uint32_t h, uint32_t stride)
{
    struct frame *fr = d;
    (void)f;
    /* 可能给出多个格式，取第一个支持的 */
    if (!fr->have_buffer && supported_format(format)) {
        fr->format = format; fr->w = w; fr->h = h; fr->stride = stride;
        fr->have_buffer = 1;
    }
}
static void fr_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
    struct frame *fr = d;
    (void)f;
    fr->y_invert = !!(flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
}
static void fr_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c)
{
    (void)f; (void)a; (void)b; (void)c;
    ((struct frame *)d)->ready = 1;
}
static void fr_failed(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)f; ((struct frame *)d)->failed = 1; }
static void fr_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void fr_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h)
{
    (void)d; (void)f; (void)fmt; (void)w; (void)h;
}
static void fr_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)f; ((struct frame *)d)->buffer_done = 1; }

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = fr_buffer, .flags = fr_flags, .ready = fr_ready, .failed = fr_failed,
    .damage = fr_damage, .linux_dmabuf = fr_dmabuf, .buffer_done = fr_buffer_done,
};

/* 带超时地分发事件，直到 *flag 或 *fail 置位。 */
static int dispatch_until(struct wl_display *dpy, int *flag, int *fail, int timeout_ms)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!*flag && !*fail) {
        while (wl_display_prepare_read(dpy) != 0)
            if (wl_display_dispatch_pending(dpy) < 0)
                return -1;
        if (*flag || *fail) {
            wl_display_cancel_read(dpy);
            break;
        }
        wl_display_flush(dpy);
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = timeout_ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        struct pollfd p = { wl_display_get_fd(dpy), POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, left) <= 0) {
            wl_display_cancel_read(dpy);
            return -1;
        }
        if (wl_display_read_events(dpy) < 0 || wl_display_dispatch_pending(dpy) < 0)
            return -1;
    }
    return *flag ? 0 : -1;
}

static int capture_output(struct capture *c, struct output *o)
{
    struct frame fr = { 0 };
    struct zwlr_screencopy_frame_v1 *f = zwlr_screencopy_manager_v1_capture_output(c->mgr, 0, o->wl);
    int ret = -1;

    zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, &fr);
    /* v3 用 buffer_done 表示格式列完了；更早的版本一次往返后 buffer 事件就到了 */
    if (zwlr_screencopy_manager_v1_get_version(c->mgr) >= 3)
        dispatch_until(c->dpy, &fr.buffer_done, &fr.failed, 2000);
    else
        wl_display_roundtrip(c->dpy);
    if (!fr.have_buffer || fr.failed) {
        LOG("output %s: no usable shm format", o->name);
        goto out;
    }

    size_t size = (size_t)fr.stride * fr.h;
    int fd = memfd_create("qq-screenshot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        if (fd >= 0)
            close(fd);
        goto out;
    }
    uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        goto out;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, fr.w, fr.h, fr.stride, fr.format);
    wl_shm_pool_destroy(pool);
    close(fd);

    zwlr_screencopy_frame_v1_copy(f, buf);
    if (dispatch_until(c->dpy, &fr.ready, &fr.failed, 3000) == 0 &&
        (o->pix = malloc((size_t)fr.w * fr.h * 4))) {
        int swap = fr.format == WL_SHM_FORMAT_XBGR8888 || fr.format == WL_SHM_FORMAT_ABGR8888;
        for (uint32_t y = 0; y < fr.h; y++) {
            const uint32_t *src = (const uint32_t *)(map + (size_t)(fr.y_invert ? fr.h - 1 - y : y) * fr.stride);
            uint32_t *dst = o->pix + (size_t)y * fr.w;
            for (uint32_t x = 0; x < fr.w; x++) {
                uint32_t p = src[x];
                if (swap)
                    p = (p & 0x0000ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff);
                dst[x] = p & 0x00ffffff;
            }
        }
        o->w = fr.w;
        o->h = fr.h;
        ret = 0;
    } else {
        LOG("output %s: capture failed", o->name);
    }
    wl_buffer_destroy(buf);
    munmap(map, size);
out:
    zwlr_screencopy_frame_v1_destroy(f);
    return ret;
}

static void capture_free(struct capture *c)
{
    for (struct output *o = c->outputs, *n; o; o = n) {
        n = o->next;
        if (o->xdg)
            zxdg_output_v1_destroy(o->xdg);
        if (o->wl)
            wl_output_destroy(o->wl);
        free(o->pix);
        free(o);
    }
    if (c->mgr)
        zwlr_screencopy_manager_v1_destroy(c->mgr);
    if (c->xdg_mgr)
        zxdg_output_manager_v1_destroy(c->xdg_mgr);
    if (c->shm)
        wl_shm_destroy(c->shm);
    if (c->dpy)
        wl_display_disconnect(c->dpy);
    memset(c, 0, sizeof *c);
}

/* 截取所有输出。至少截到一个时返回 0。 */
static int capture_all(struct capture *c)
{
    memset(c, 0, sizeof *c);
    c->dpy = wl_display_connect(NULL);
    if (!c->dpy)
        return -1;
    struct wl_registry *reg = wl_display_get_registry(c->dpy);
    wl_registry_add_listener(reg, &registry_listener, c);
    wl_display_roundtrip(c->dpy);
    wl_display_roundtrip(c->dpy); /* wl_output.name */
    wl_registry_destroy(reg);
    if (!c->mgr || !c->shm) {
        LOG("compositor has no wlr-screencopy");
        return -1;
    }
    int got = 0;
    for (struct output *o = c->outputs; o; o = o->next)
        got += capture_output(c, o) == 0;
    return got ? 0 : -1;
}

/* ---------------- 按 X 的布局拼成根窗口画面 ---------------- */

/*
 * 轻量查询指定 Wayland 输出的原生模式尺寸、scale 和逻辑尺寸（不截帧）。
 * 合成器没有 xdg-output 时逻辑尺寸为 0。
 * XGetWindowAttributes / XRRGetMonitors 可能被频繁调用，结果按输出名缓存 5 秒。
 */
struct output_info {
    int mode_w, mode_h, scale, log_w, log_h;
};

static int query_output(const char *want, struct output_info *info)
{
    static char c_name[64];
    static struct output_info c_info;
    static struct timespec c_at;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (c_name[0] && !strcmp(c_name, want) &&
        (now.tv_sec - c_at.tv_sec) * 1000 + (now.tv_nsec - c_at.tv_nsec) / 1000000 < 5000) {
        *info = c_info;
        return 1;
    }

    struct capture c = { 0 };
    c.dpy = wl_display_connect(NULL);
    if (!c.dpy)
        return 0;
    struct wl_registry *reg = wl_display_get_registry(c.dpy);
    wl_registry_add_listener(reg, &registry_listener, &c);
    wl_display_roundtrip(c.dpy);
    wl_display_roundtrip(c.dpy); /* wl_output.name/mode/scale */
    wl_registry_destroy(reg);
    if (c.xdg_mgr) {
        for (struct output *o = c.outputs; o; o = o->next) {
            o->xdg = zxdg_output_manager_v1_get_xdg_output(c.xdg_mgr, o->wl);
            zxdg_output_v1_add_listener(o->xdg, &xdg_output_listener, o);
        }
        wl_display_roundtrip(c.dpy); /* zxdg_output_v1.logical_size */
    }

    int found = 0;
    for (struct output *o = c.outputs; o; o = o->next)
        if (!strcmp(o->name, want) && o->mode_w > 0 && o->mode_h > 0) {
            info->mode_w = o->mode_w;
            info->mode_h = o->mode_h;
            info->scale = o->scale > 0 ? o->scale : 1;
            info->log_w = o->log_w;
            info->log_h = o->log_h;
            found = 1;
            break;
        }
    capture_free(&c);
    if (found) {
        snprintf(c_name, sizeof c_name, "%s", want);
        c_info = *info;
        c_at = now;
    }
    return found;
}

typedef struct {
    Atom name;
    Bool primary, automatic;
    int noutput, x, y, width, height, mwidth, mheight;
    void *outputs;
} MonitorInfo; /* 与 XRRMonitorInfo 布局一致；运行时 dlopen libXrandr，不增加链接依赖 */

typedef MonitorInfo *(*get_monitors_fn)(Display *, Window, Bool, int *);
typedef void (*free_monitors_fn)(MonitorInfo *);

/* 根窗口的整张画面。缓存一小会儿：QQ 可能对每块屏各取一次 */
static struct {
    Display *dpy;
    uint32_t *pix;
    int w, h;
    struct timespec at;
} root_cache;

static void blit(uint32_t *dst, int dw, int dh, int mx, int my, int mw, int mh, const struct output *o)
{
    for (int y = 0; y < mh; y++) {
        int ry = my + y;
        if (ry < 0 || ry >= dh)
            continue;
        const uint32_t *src = o->pix + (size_t)((long)y * o->h / mh) * o->w;
        uint32_t *row = dst + (size_t)ry * dw;
        for (int x = 0; x < mw; x++) {
            int rx = mx + x;
            if (rx >= 0 && rx < dw)
                row[rx] = src[(long)x * o->w / mw];
        }
    }
}

/* ---------------- 目标显示器：niri 聚焦输出，退回指针位置 ---------------- */

/* 通过 niri IPC 取聚焦输出名；带 1 秒缓存（一次截图里会查询多次）。 */
static int niri_focused_output(char *name, size_t name_size)
{
    static char cached[64];
    static struct timespec cached_at;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (cached[0] &&
        (now.tv_sec - cached_at.tv_sec) * 1000 +
            (now.tv_nsec - cached_at.tv_nsec) / 1000000 < 1000) {
        snprintf(name, name_size, "%s", cached);
        return 1;
    }

    const char *path = getenv("NIRI_SOCKET");
    if (!path || !*path)
        return 0;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    struct sockaddr_un addr = { 0 };
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return 0;
    }
    static const char req[] = "\"FocusedOutput\"\n";
    if (write(fd, req, sizeof req - 1) != (ssize_t)(sizeof req - 1)) {
        close(fd);
        return 0;
    }
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    const char *p = strstr(buf, "\"name\":\"");
    if (!p)
        return 0;
    p += 8;
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= name_size)
        return 0;
    snprintf(name, name_size, "%.*s", (int)(e - p), p);

    snprintf(cached, sizeof cached, "%s", name);
    cached_at = now;
    return 1;
}

/*
 * pick_monitor：want 非空按名字选（niri 的聚焦输出），否则选指针所在的
 * XRandR 显示器。返回名字和 X 逻辑尺寸。
 */
static int pick_monitor(Display *dpy, const char *want, char *name, size_t name_size,
                        int *mx, int *my, int *mw, int *mh)
{
    Window root = DefaultRootWindow(dpy);
    int rx = 0, ry = 0;
    if (!want) {
        Window rr, cr;
        int wx, wy;
        unsigned mask;
        if (!XQueryPointer(dpy, root, &rr, &cr, &rx, &ry, &wx, &wy, &mask))
            return 0;
    }

    void *xrandr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!xrandr)
        return 0;
    get_monitors_fn get = (get_monitors_fn)dlsym(xrandr, "XRRGetMonitors");
    free_monitors_fn freem = (free_monitors_fn)dlsym(xrandr, "XRRFreeMonitors");
    int n = 0, found = 0;
    MonitorInfo *m = get ? get(dpy, root, True, &n) : NULL;
    for (int i = 0; m && i < n; i++) {
        char *nm = XGetAtomName(dpy, m[i].name);
        if (!nm)
            continue;
        int hit = want ? !strcmp(nm, want)
                       : (rx >= m[i].x && rx < m[i].x + m[i].width &&
                          ry >= m[i].y && ry < m[i].y + m[i].height);
        if (hit) {
            snprintf(name, name_size, "%s", nm);
            *mx = m[i].x;
            *my = m[i].y;
            *mw = m[i].width;
            *mh = m[i].height;
            found = 1;
        }
        XFree(nm);
        if (found)
            break;
    }
    if (m && freem)
        freem(m);
    /* 不 dlclose：libXrandr 可能在 Xlib 里注册过扩展钩子，卸载后
     * XCloseDisplay 会踩到悬空函数指针。 */
    return found;
}

/* 优先 niri 聚焦输出；不是 niri 或查询失败时按指针位置。 */
static int target_monitor(Display *dpy, char *name, size_t name_size,
                          int *mx, int *my, int *mw, int *mh)
{
    char niri[64];
    if (niri_focused_output(niri, sizeof niri) &&
        pick_monitor(dpy, niri, name, name_size, mx, my, mw, mh))
        return 1;
    return pick_monitor(dpy, NULL, name, name_size, mx, my, mw, mh);
}

/* ---------------- KDE（KWin ScreenShot2）---------------- */

struct shot_header {
    char magic[4]; /* "QQKS" */
    uint32_t width, height, stride, format; /* QImage::Format */
    double scale;
};

/* 0=自动（KDE 会话才用），1=强制（测试用），-1=禁用 */
static int kde_mode(void)
{
    static int mode = -2;
    if (mode != -2)
        return mode;
    const char *v = getenv("QQ_SCREENSHOT_KDE");
    if (v && *v) {
        mode = !strcmp(v, "0") ? -1 : 1;
        return mode;
    }
    const char *de = getenv("XDG_CURRENT_DESKTOP");
    const char *kde = getenv("KDE_FULL_SESSION");
    mode = ((de && strstr(de, "KDE")) || (kde && *kde)) ? 1 : 0;
    return mode;
}

static const char *screenshot_helper_path(void)
{
    const char *p = getenv("QQ_SCREENSHOT_HELPER");
    return (p && *p) ? p : QQ_SCREENSHOT_HELPER;
}

static int kde_usable(void)
{
    return kde_mode() == 1 && access(screenshot_helper_path(), X_OK) == 0;
}

/* portal 截图（org.freedesktop.portal.Screenshot）：GNOME 默认启用（KDE 有
 * ScreenShot2、wlroots 有 wlr-screencopy，都不走这条）；QQ_SCREENSHOT_PORTAL=1/0
 * 可强制开关（测试用）。 */
static int portal_enabled(void)
{
    static int state = -1;
    if (state >= 0)
        return state;
    const char *v = getenv("QQ_SCREENSHOT_PORTAL");
    if (v && *v) {
        state = !strcmp(v, "0") ? 0 : 1;
        return state;
    }
    const char *de = getenv("XDG_CURRENT_DESKTOP");
    state = (de && strstr(de, "GNOME")) ? 1 : 0;
    return state;
}

/* 去掉子进程环境里的注入变量，避免 helper 里再加载一遍我们的库。 */
static void strip_env(const char *name)
{
    extern char **environ;
    size_t len = strlen(name);
    for (char **p = environ; *p;) {
        if (!strncmp(*p, name, len) && (*p)[len] == '=') {
            for (char **q = p; *q; q++)
                *q = q[1];
        } else {
            p++;
        }
    }
}

static int read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * 调 helper 截整张工作区（mode = "kde" 走 KWin ScreenShot2，"portal" 走
 * org.freedesktop.portal.Screenshot）。成功返回 0x00RRGGBB 的缓冲，失败返回 NULL。
 */
static uint32_t *helper_workspace_image(const char *mode, int *w, int *h)
{
    const char *helper = screenshot_helper_path();
    int fds[2];
    if (pipe(fds) != 0)
        return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        if (fds[1] != STDOUT_FILENO)
            close(fds[1]);
        strip_env("LD_PRELOAD");
        strip_env("LD_LIBRARY_PATH");
        execl(helper, helper, mode, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    struct shot_header hdr;
    uint8_t *raw = NULL;
    uint32_t *pix = NULL;
    if (read_all(fds[0], &hdr, sizeof hdr) != 0 || memcmp(hdr.magic, "QQKS", 4) != 0 ||
        !hdr.width || !hdr.height || !hdr.stride) {
        LOG("%s: screenshot helper failed (KDE 权限或 portal 不可用？)", mode);
        goto out;
    }

    size_t total = (size_t)hdr.stride * hdr.height;
    raw = malloc(total);
    if (!raw || read_all(fds[0], raw, total) != 0) {
        LOG("%s: short image from helper", mode);
        goto out;
    }

    pix = malloc((size_t)hdr.width * hdr.height * 4);
    if (!pix)
        goto out;
    switch (hdr.format) {
    case 4: case 5: case 6: /* RGB32 / ARGB32 / ARGB32_Premultiplied：内存里就是 B,G,R,A */
        for (uint32_t y = 0; y < hdr.height; y++) {
            const uint32_t *s = (const uint32_t *)(raw + (size_t)y * hdr.stride);
            uint32_t *d = pix + (size_t)y * hdr.width;
            for (uint32_t x = 0; x < hdr.width; x++)
                d[x] = s[x] & 0x00ffffffu;
        }
        break;
    case 16: case 17: case 18: /* RGBX8888 / RGBA8888[_Premultiplied]：字节序 R,G,B,A */
        for (uint32_t y = 0; y < hdr.height; y++) {
            const uint8_t *s = raw + (size_t)y * hdr.stride;
            uint32_t *d = pix + (size_t)y * hdr.width;
            for (uint32_t x = 0; x < hdr.width; x++, s += 4)
                d[x] = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
        }
        break;
    default:
        LOG("%s: unsupported QImage format %u", mode, hdr.format);
        free(pix);
        pix = NULL;
        goto out;
    }
    *w = (int)hdr.width;
    *h = (int)hdr.height;
    LOG("%s: captured workspace %ux%u (QImage format %u, scale %.2f)",
        mode, hdr.width, hdr.height, hdr.format, hdr.scale);

out:
    free(raw);
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return pix;
}

/* 把 src 里 (sx,sy,sw,sh) 缩放着铺进 dst（最近邻，和 blit 一致）。 */
static void blit_rect(uint32_t *dst, int dw, int dh,
                      const uint32_t *src, int src_w, int src_h,
                      int sx, int sy, int sw, int sh)
{
    for (int y = 0; y < dh; y++) {
        int syy = sy + (int)((long)y * sh / dh);
        if (syy < 0 || syy >= src_h)
            continue;
        const uint32_t *srow = src + (size_t)syy * src_w;
        uint32_t *drow = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int sxx = sx + (int)((long)x * sw / dw);
            if (sxx >= 0 && sxx < src_w)
                drow[x] = srow[sxx];
        }
    }
}

/*
 * 单屏：工作区整图（helper 给的原始像素）按 X 根窗口/显示器矩形的比例裁剪并
 * 缩放到请求尺寸。X11 根窗口原点与工作区包围盒原点一致，按尺寸比例映射即可。
 */
static uint32_t *helper_monitor_image(const char *mode, Display *dpy,
                                      int mx, int my, int mw, int mh, int w, int h)
{
    int kw = 0, kh = 0;
    uint32_t *kpix = helper_workspace_image(mode, &kw, &kh);
    if (!kpix)
        return NULL;

    Window r;
    int rx, ry;
    unsigned rw = 0, rh = 0, bw, depth;
    if (!XGetGeometry(dpy, DefaultRootWindow(dpy), &r, &rx, &ry, &rw, &rh, &bw, &depth) || !rw || !rh) {
        free(kpix);
        return NULL;
    }

    uint32_t *pix = calloc((size_t)w * h, 4);
    if (pix) {
        int ix = (int)((long)mx * kw / (long)rw);
        int iy = (int)((long)my * kh / (long)rh);
        int iw = (int)((long)mw * kw / (long)rw);
        int ih = (int)((long)mh * kh / (long)rh);
        LOG("%s: crop monitor (%d,%d %dx%d) -> image (%d,%d %dx%d) -> %dx%d",
            mode, mx, my, mw, mh, ix, iy, iw, ih, w, h);
        blit_rect(pix, w, h, kpix, kw, kh, ix, iy, iw, ih);
    }
    free(kpix);
    return pix;
}

/* 单屏模式：只截指针所在显示器，按请求的尺寸缩放。 */
static struct {
    Display *dpy;
    char name[64];
    int w, h;
    uint32_t *pix;
    struct timespec at;
} mon_cache;

static uint32_t *monitor_image(Display *dpy, const char *name,
                               int mx, int my, int mw, int mh, int w, int h)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (mon_cache.pix && mon_cache.dpy == dpy && mon_cache.w == w && mon_cache.h == h &&
        !strcmp(mon_cache.name, name) &&
        (now.tv_sec - mon_cache.at.tv_sec) * 1000 +
            (now.tv_nsec - mon_cache.at.tv_nsec) / 1000000 < 1500)
        return mon_cache.pix;

    free(mon_cache.pix);
    mon_cache.pix = NULL;

    uint32_t *pix = NULL;
    int kde_first = kde_usable(); /* KDE 会话（或测试强制）时优先 ScreenShot2 */

    if (kde_first)
        pix = helper_monitor_image("kde", dpy, mx, my, mw, mh, w, h);

    if (!pix) {
        struct capture c;
        int cap = capture_all(&c);
        if (cap == 0) {
            pix = calloc((size_t)w * h, 4);
            if (pix) {
                int found = 0;
                for (struct output *o = c.outputs; o; o = o->next)
                    if (o->pix && !strcmp(o->name, name)) {
                        LOG("single-monitor: %s %dx%d -> requested %dx%d", name, o->w, o->h, w, h);
                        blit(pix, w, h, 0, 0, w, h, o);
                        found = 1;
                        break;
                    }
                if (!found) {
                    free(pix);
                    pix = NULL;
                }
            }
        }
        capture_free(&c);
    }

    if (!pix && !kde_first && kde_usable())
        pix = helper_monitor_image("kde", dpy, mx, my, mw, mh, w, h);

    if (!pix && portal_enabled())
        pix = helper_monitor_image("portal", dpy, mx, my, mw, mh, w, h);

    if (!pix)
        pix = calloc((size_t)w * h, 4); /* 截不到：给全黑，行为与以前一致 */

    mon_cache.dpy = dpy;
    snprintf(mon_cache.name, sizeof mon_cache.name, "%s", name);
    mon_cache.w = w;
    mon_cache.h = h;
    mon_cache.at = now;
    mon_cache.pix = pix;
    return pix;
}

static uint32_t *root_image(Display *dpy, Window root, int *w, int *h)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (root_cache.pix && root_cache.dpy == dpy &&
        (now.tv_sec - root_cache.at.tv_sec) * 1000 + (now.tv_nsec - root_cache.at.tv_nsec) / 1000000 < 1500) {
        *w = root_cache.w;
        *h = root_cache.h;
        return root_cache.pix;
    }
    free(root_cache.pix);
    root_cache.pix = NULL;

    Window r;
    int rx, ry;
    unsigned rw, rh, bw, depth;
    if (!XGetGeometry(dpy, root, &r, &rx, &ry, &rw, &rh, &bw, &depth))
        return NULL;

    uint32_t *pix = NULL;
    struct capture c;
    int cap = capture_all(&c);
    if (cap == 0) {
        pix = calloc((size_t)rw * rh, 4);
        if (pix) {
            void *xrandr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
            get_monitors_fn get = xrandr ? (get_monitors_fn)dlsym(xrandr, "XRRGetMonitors") : NULL;
            free_monitors_fn freem = xrandr ? (free_monitors_fn)dlsym(xrandr, "XRRFreeMonitors") : NULL;
            int n = 0, placed = 0;
            MonitorInfo *m = get ? get(dpy, root, True, &n) : NULL;
            for (int i = 0; i < n; i++) {
                char *name = XGetAtomName(dpy, m[i].name);
                for (struct output *o = c.outputs; o && name; o = o->next)
                    if (o->pix && !strcmp(o->name, name)) {
                        blit(pix, rw, rh, m[i].x, m[i].y, m[i].width, m[i].height, o);
                        placed++;
                        break;
                    }
                XFree(name);
            }
            if (m && freem)
                freem(m);
            /* 不 dlclose，原因见 pick_monitor。 */
            if (!placed) /* 对不上名字时：铺满第一块屏 */
                for (struct output *o = c.outputs; o; o = o->next)
                    if (o->pix) {
                        blit(pix, rw, rh, 0, 0, rw, rh, o);
                        break;
                    }
            LOG("captured %ux%u root from Wayland (%d monitor(s) matched)", rw, rh, placed);
        }
    }
    capture_free(&c);

    if (!pix && kde_usable()) {
        int kw = 0, kh = 0;
        uint32_t *kpix = helper_workspace_image("kde", &kw, &kh);
        if (kpix) {
            pix = calloc((size_t)rw * rh, 4);
            if (pix)
                blit_rect(pix, (int)rw, (int)rh, kpix, kw, kh, 0, 0, kw, kh);
            free(kpix);
            if (pix)
                LOG("captured %ux%u root via KDE ScreenShot2", rw, rh);
        }
    }

    if (!pix && portal_enabled()) {
        int kw = 0, kh = 0;
        uint32_t *kpix = helper_workspace_image("portal", &kw, &kh);
        if (kpix) {
            pix = calloc((size_t)rw * rh, 4);
            if (pix)
                blit_rect(pix, (int)rw, (int)rh, kpix, kw, kh, 0, 0, kw, kh);
            free(kpix);
            if (pix)
                LOG("captured %ux%u root via portal Screenshot", rw, rh);
        }
    }

    if (!pix)
        return NULL;

    root_cache.dpy = dpy;
    root_cache.pix = pix;
    root_cache.w = rw;
    root_cache.h = rh;
    root_cache.at = now;
    *w = rw;
    *h = rh;
    return pix;
}

/* 把根窗口画面里 (x, y, w, h) 这一块写进 32 位 ZPixmap 图像。截不到时返回 -1，图像不动。 */
static int fill_from_wayland(Display *dpy, Window root, XImage *img, int x, int y)
{
    int rw = 0, rh = 0;
    uint32_t *pix = NULL;
    char name[64];
    int mw = 0, mh = 0;

    int mx = 0, my = 0;
    if (getenv("WAYLAND_DISPLAY") && img->format == ZPixmap && img->bits_per_pixel == 32) {
        if (target_monitor(dpy, name, sizeof name, &mx, &my, &mw, &mh)) {
            /* 单屏模式：QQ 看到的屏幕就是目标显示器（优先 niri 聚焦输出） */
            pix = monitor_image(dpy, name, mx, my, mw, mh, img->width, img->height);
            rw = img->width;
            rh = img->height;
        } else {
            pix = root_image(dpy, root, &rw, &rh);
        }
    }
    if (!pix)
        return -1;
    for (int j = 0; j < img->height; j++) {
        uint32_t *row = (uint32_t *)(img->data + (size_t)j * img->bytes_per_line);
        for (int i = 0; i < img->width; i++) {
            int sx = x + i, sy = y + j;
            row[i] = (sx >= 0 && sy >= 0 && sx < rw && sy < rh) ? pix[(size_t)sy * rw + sx] : 0;
        }
    }
    return 0;
}

/* ---------------- 拦截 ---------------- */

static int enabled(void)
{
    static int state = -1;
    if (state < 0) {
        const char *v = getenv("QQ_SCREENSHOT_FIX_DISABLE");
        state = !(v && *v && strcmp(v, "0"));
    }
    return state;
}

static int is_root(Display *dpy, Drawable d)
{
    for (int i = 0; i < ScreenCount(dpy); i++)
        if (d == RootWindow(dpy, i))
            return 1;
    return 0;
}

/* 截取期间临时接管 X 错误（默认处理会直接退出进程）。QQ 只在主线程截图。 */
static int x_failed;
static int trap(Display *dpy, XErrorEvent *e)
{
    (void)dpy; (void)e;
    x_failed = 1;
    return 0;
}

typedef XImage *(*get_image_fn)(Display *, Drawable, int, int, unsigned, unsigned, unsigned long, int);
typedef Bool (*shm_get_image_fn)(Display *, Drawable, XImage *, int, int, unsigned long);

XImage *XGetImage(Display *dpy, Drawable d, int x, int y, unsigned w, unsigned h,
                  unsigned long planes, int format)
{
    static get_image_fn real;
    if (!real)
        real = (get_image_fn)dlsym(RTLD_NEXT, "XGetImage");

    if (!enabled() || !is_root(dpy, d) || !w || !h)
        return real(dpy, d, x, y, w, h, planes, format);

    int scr = DefaultScreen(dpy);
    int depth = DefaultDepth(dpy, scr);
    XImage *out = XCreateImage(dpy, DefaultVisual(dpy, scr), depth, format, 0, NULL, w, h, 32, 0);
    if (!out)
        return NULL;
    out->data = calloc((size_t)out->bytes_per_line * h * (format == ZPixmap ? 1 : depth), 1);
    if (!out->data) {
        XDestroyImage(out);
        return NULL;
    }
    if (fill_from_wayland(dpy, d, out, x, y) == 0)
        return out;

    XSync(dpy, False);
    x_failed = 0;
    XErrorHandler old = XSetErrorHandler(trap);
    XImage *img = real(dpy, d, x, y, w, h, planes, format);
    XSync(dpy, False);
    XSetErrorHandler(old);
    if (img && !x_failed) {
        XDestroyImage(out);
        return img;
    }
    if (img)
        XDestroyImage(img);
    LOG("XGetImage on root %ux%u+%d+%d failed, returning a black image", w, h, x, y);
    return out;
}

Bool XShmGetImage(Display *dpy, Drawable d, XImage *img, int x, int y, unsigned long planes)
{
    static shm_get_image_fn real;
    if (!real)
        real = (shm_get_image_fn)dlsym(RTLD_NEXT, "XShmGetImage");
    if (!real)
        return False;

    if (!enabled() || !img || !is_root(dpy, d))
        return real(dpy, d, img, x, y, planes);

    if (fill_from_wayland(dpy, d, img, x, y) == 0)
        return True;

    XSync(dpy, False);
    x_failed = 0;
    XErrorHandler old = XSetErrorHandler(trap);
    Bool ok = real(dpy, d, img, x, y, planes);
    XSync(dpy, False);
    XSetErrorHandler(old);
    if (ok && !x_failed)
        return ok;
    memset(img->data, 0, (size_t)img->bytes_per_line * img->height);
    LOG("XShmGetImage on root %dx%d+%d+%d failed, returning a black image", img->width, img->height, x, y);
    return True;
}

/*
 * 单屏模式：QQ 用 XGetWindowAttributes(root) 取「屏幕尺寸」来决定截取范围
 * 和覆盖层大小。这里把根窗口的宽高报成指针所在显示器的尺寸，配合
 * fill_from_wayland 只截那块输出；找不到显示器时（真 X11 会话等）保持原样。
 */
typedef int (*get_window_attrs_fn)(Display *, Window, XWindowAttributes *);

int XGetWindowAttributes(Display *dpy, Window w, XWindowAttributes *attr)
{
    static get_window_attrs_fn real;
    if (!real)
        real = (get_window_attrs_fn)dlsym(RTLD_NEXT, "XGetWindowAttributes");

    int r = real(dpy, w, attr);
    if (r && enabled() && getenv("WAYLAND_DISPLAY") && is_root(dpy, w)) {
        char name[64];
        int mx, my, mw, mh;
        if (target_monitor(dpy, name, sizeof name, &mx, &my, &mw, &mh)) {
            struct output_info info;
            int mode_w = 0, mode_h = 0, scale = 1;
            if (query_output(name, &info)) {
                mode_w = info.mode_w;
                mode_h = info.mode_h;
                scale = info.scale;
            }
            if (mode_w > 0 && (mode_w != mw || mode_h != mh)) {
                /* QQ 的截图覆盖层按设备像素工作：KWin/GNOME 的 XWayland 直接报原生
                 * 尺寸，Hyprland 这类报逻辑尺寸；上报逻辑尺寸会让原生截图 1:1 画进
                 * scale 倍缓冲，只填左上角。统一上报 Wayland 输出的原生模式尺寸。 */
                LOG("single-monitor: %s scale %d, report native %dx%d (x11 %dx%d)",
                    name, scale, mode_w, mode_h, mw, mh);
                mw = mode_w;
                mh = mode_h;
            }
            LOG("single-monitor: report screen as %s %dx%d", name, mw, mh);
            attr->width = mw;
            attr->height = mh;
        }
    }
    return r;
}

/*
 * 共享时的批注 / 激光笔绘制窗口（issue #32、#34）。
 *
 * libAVSDKPlugin.so 的 QRTCServiceInterfaceWrapper::SizeUpdateProc 定时用
 * XRRGetMonitors 取被共享显示器在 X11 里的矩形，原样交给主进程摆放绘制窗口；
 * 主进程把它当逻辑尺寸（DIP）用（Linux 版 GetDpiForDisplay 恒为 1.0，不做换算）。
 * KDE（旧版应用自行缩放）、xwayland-satellite 的 XWayland 报的是物理像素，
 * 分数缩放下绘制窗口就大了 scale 倍：工具条跑到「逻辑中心」、笔迹落点错位。
 *
 * 这里只对 libAVSDKPlugin.so 的调用：X11 报的显示器尺寸等于该输出的物理模式尺寸、
 * 而 xdg-output 的逻辑尺寸更小时，把矩形换算成逻辑坐标。X11 本来就报逻辑尺寸
 * （Hyprland、KDE「由系统缩放」）或缩放为 1 时不动。同库的 DisplayIdToMonitor /
 * DisplayIndexToMonitor 只按名字找序号，不受影响。
 *
 * QQ_DRAWING_RECT_FIX_DISABLE=1 可以关掉。
 */
typedef MonitorInfo *(*xrr_get_monitors_fn)(Display *, Window, Bool, int *);

static int called_from(const void *caller, const char *lib)
{
    Dl_info info;
    if (!caller || !dladdr(caller, &info) || !info.dli_fname)
        return 0;
    const char *base = strrchr(info.dli_fname, '/');
    return !strcmp(base ? base + 1 : info.dli_fname, lib);
}

static int drawing_rect_fix_enabled(void)
{
    static int state = -1;
    if (state < 0) {
        const char *v = getenv("QQ_DRAWING_RECT_FIX_DISABLE");
        state = enabled() && !(v && *v && strcmp(v, "0")) && getenv("WAYLAND_DISPLAY");
    }
    return state;
}

/* X11 显示器矩形是物理像素时换算成逻辑坐标；返回是否改了。 */
static int monitor_to_logical(Display *dpy, MonitorInfo *m)
{
    char *nm = XGetAtomName(dpy, m->name);
    if (!nm)
        return 0;
    struct output_info info;
    int found = query_output(nm, &info);
    int changed = 0;
    if (found && info.log_w > 0 && info.log_h > 0 && m->width > 0 && m->height > 0) {
        /* 输出旋转时模式尺寸是转之前的，X11 和 xdg-output 给的都是转之后的 */
        int physical = (m->width == info.mode_w && m->height == info.mode_h) ||
                       (m->width == info.mode_h && m->height == info.mode_w);
        if (physical && (m->width != info.log_w || m->height != info.log_h)) {
            static int logged_w, logged_h;
            if (logged_w != m->width || logged_h != m->height) {
                logged_w = m->width;
                logged_h = m->height;
                LOG("drawing rect: %s X11 %dx%d+%d+%d -> logical %dx%d", nm, m->width, m->height,
                    m->x, m->y, info.log_w, info.log_h);
            }
            m->x = (int)((long)m->x * info.log_w / m->width);
            m->y = (int)((long)m->y * info.log_h / m->height);
            m->width = info.log_w;
            m->height = info.log_h;
            changed = 1;
        }
    }
    XFree(nm);
    return changed;
}

MonitorInfo *XRRGetMonitors(Display *dpy, Window w, Bool active, int *n)
{
    static xrr_get_monitors_fn real;
    if (!real)
        real = (xrr_get_monitors_fn)dlsym(RTLD_NEXT, "XRRGetMonitors");
    if (!real) {
        if (n)
            *n = 0;
        return NULL;
    }

    MonitorInfo *m = real(dpy, w, active, n);
    if (m && n && *n > 0 && drawing_rect_fix_enabled() &&
        called_from(__builtin_return_address(0), "libAVSDKPlugin.so"))
        for (int i = 0; i < *n; i++)
            monitor_to_logical(dpy, &m[i]);
    return m;
}
