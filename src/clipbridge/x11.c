/*
 * 后台线程自己的 X 连接：直接向 QQ 的复制窗口要数据，把粘贴结果写给 QQ 的窗口。
 * 所有往来都是我们与 QQ 之间点对点的事件和窗口属性，X11 的 CLIPBOARD 本身从不改动。
 */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clipbridge.h"

#define FETCH_TIMEOUT_MS 1000   /* QQ 回应一次取数据最多等这么久（正常不到 1 ms） */
#define NUDGE_MS 25             /* 这么久没回应就叫醒 QQ 一次，见 nudge() */

int cb_debug;
Display *cb_xdpy;
Window cb_xwin;
Atom cb_A_CLIPBOARD, cb_A_TARGETS, cb_A_UTF8, cb_A_GNOME_FILES, cb_A_URI_LIST;
static Atom A_PROP, A_WAKE;
static Window wake_win;   /* 叫醒 QQ 用的请求的应答落在这里，与正式请求分开 */

/*
 * QQ 没有装自己的 X 错误处理，用的是 libX11 默认的：出错就 exit，整个 QQ 跟着退出。
 * 本连接上的错误（比如要写的窗口刚好被销毁）只记一笔；其它连接的照旧交给原来的处理。
 */
static XErrorHandler prev_error;

static int on_x_error(Display *dpy, XErrorEvent *e)
{
    if (dpy == cb_xdpy) {
        LOG("X error %d (request %d) on the bridge's own connection, ignored", e->error_code, e->request_code);
        return 0;
    }
    return prev_error ? prev_error(dpy, e) : 0;
}

int cb_x11_open(void)
{
    const char *dbg = getenv("QQ_CLIPBOARD_DEBUG");
    cb_debug = dbg && *dbg && strcmp(dbg, "0");
    cb_xdpy = XOpenDisplay(NULL);
    if (!cb_xdpy)
        return -1;
    prev_error = XSetErrorHandler(on_x_error);
    cb_xwin = XCreateSimpleWindow(cb_xdpy, DefaultRootWindow(cb_xdpy), 0, 0, 1, 1, 0, 0, 0);
    wake_win = XCreateSimpleWindow(cb_xdpy, DefaultRootWindow(cb_xdpy), 0, 0, 1, 1, 0, 0, 0);
    cb_A_CLIPBOARD = XInternAtom(cb_xdpy, "CLIPBOARD", False);
    cb_A_TARGETS = XInternAtom(cb_xdpy, "TARGETS", False);
    cb_A_UTF8 = XInternAtom(cb_xdpy, "UTF8_STRING", False);
    cb_A_GNOME_FILES = XInternAtom(cb_xdpy, "x-special/gnome-copied-files", False);
    cb_A_URI_LIST = XInternAtom(cb_xdpy, "text/uri-list", False);
    A_PROP = XInternAtom(cb_xdpy, "QQ_CLIPBRIDGE", False);
    A_WAKE = XInternAtom(cb_xdpy, "QQ_CLIPBRIDGE_WAKE", False);
    XFlush(cb_xdpy);
    return ConnectionNumber(cb_xdpy);
}

static int ms_left(const struct timespec *end)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int)((end->tv_sec - now.tv_sec) * 1000 + (end->tv_nsec - now.tv_nsec) / 1000000);
}

/*
 * QQ 只在连接上来了新数据时才处理事件。QQ 启动后第一次复制时，它设完主人还要做几次 X 往返
 * （登记格式名）；我们的请求若在这期间到达，会被一并读进 Xlib 的队列，之后连接上没有新数据，
 * 请求就一直没人处理（实测：一直等到下一次粘贴才回应）。这时再发一个请求，QQ 醒来会把
 * 队列里的都处理掉。只能发 SelectionRequest：QQ 回应完一次复制后收到别的事件（实测
 * ClientMessage）会退出回应循环，此后一律拒绝，直到下一次复制。
 */
static void nudge(Window owner)
{
    XSelectionRequestEvent rq = {
        .type = SelectionRequest, .send_event = True, .display = cb_xdpy, .owner = owner,
        .requestor = wake_win, .selection = cb_A_CLIPBOARD, .target = cb_A_TARGETS, .property = A_WAKE,
        .time = CurrentTime,
    };
    XSendEvent(cb_xdpy, owner, False, NoEventMask, (XEvent *)&rq);
    XFlush(cb_xdpy);
}

/* 等 QQ 对 target 的应答；之前超时的应答迟到了就丢掉 */
static int nudges;

static int wait_notify(Window owner, Atom target, XEvent *ev)
{
    nudges = 0;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += FETCH_TIMEOUT_MS / 1000;
    end.tv_nsec += (FETCH_TIMEOUT_MS % 1000) * 1000000L;
    for (;;) {
        while (XCheckTypedWindowEvent(cb_xdpy, cb_xwin, SelectionNotify, ev))
            if (ev->xselection.target == target)
                return 1;
        int left = ms_left(&end);
        if (left <= 0)
            return 0;
        struct pollfd p = { ConnectionNumber(cb_xdpy), POLLIN, 0 };
        if (poll(&p, 1, left < NUDGE_MS ? left : NUDGE_MS) == 0) {
            nudge(owner);
            ++nudges;
        }
    }
}

/*
 * 向 QQ 的复制窗口要 target 格式的数据：不经过 X11 的 CLIPBOARD，直接给这个窗口发 SelectionRequest，
 * QQ 的剪贴板线程照常回应。结果放在 *out（malloc，末尾补 0），返回字节数；失败返回 -1。
 * QQ 不做 INCR 分块（它从不监听属性变化），数据总是一次写完。
 */
long cb_x11_fetch(Window owner, Atom target, unsigned char **out)
{
    *out = NULL;
    XDeleteProperty(cb_xdpy, cb_xwin, A_PROP);
    XSelectionRequestEvent rq = {
        .type = SelectionRequest, .send_event = True, .display = cb_xdpy, .owner = owner,
        .requestor = cb_xwin, .selection = cb_A_CLIPBOARD, .target = target, .property = A_PROP,
        .time = CurrentTime,
    };
    XSendEvent(cb_xdpy, owner, False, NoEventMask, (XEvent *)&rq);
    XFlush(cb_xdpy);

    XEvent ev;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int answered = wait_notify(owner, target, &ev);
    if (cb_debug) {
        char *name = cb_x11_name(target);
        DEBUG("fetch %s from 0x%lx: %s after %d ms, %d nudge(s)", name ? name : "?", owner,
              !answered ? "no answer" : ev.xselection.property == None ? "refused" : "ok", -ms_left(&t0), nudges);
        free(name);
    }
    if (!answered || ev.xselection.property == None)
        return -1;
    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(cb_xdpy, cb_xwin, A_PROP, 0, 0x7fffffff, True, AnyPropertyType, &type, &format,
                           &nitems, &after, &data) != Success || !data)
        return -1;
    long len = nitems * (format == 32 ? (long)sizeof(long) : format / 8);
    *out = malloc(len + 1);
    memcpy(*out, data, len);
    (*out)[len] = 0;
    XFree(data);
    return len;
}

/* QQ 粘贴自己复制的内容：照 X 服务器的做法，把请求转给 QQ 的复制窗口，由 QQ 自己回应。 */
void cb_x11_forward(Window owner, const struct cb_request *r)
{
    XSelectionRequestEvent rq = {
        .type = SelectionRequest, .send_event = True, .display = cb_xdpy, .owner = owner,
        .requestor = r->requestor, .selection = cb_A_CLIPBOARD, .target = r->target, .property = r->property,
        .time = r->time,
    };
    XSendEvent(cb_xdpy, owner, False, NoEventMask, (XEvent *)&rq);
    XFlush(cb_xdpy);
}

static void notify(const struct cb_request *r, Atom property)
{
    XSelectionEvent ev = {
        .type = SelectionNotify, .send_event = True, .display = cb_xdpy, .requestor = r->requestor,
        .selection = cb_A_CLIPBOARD, .target = r->target, .property = property, .time = r->time,
    };
    XSendEvent(cb_xdpy, r->requestor, False, NoEventMask, (XEvent *)&ev);
    XFlush(cb_xdpy);
}

/* 把数据写到 QQ 的窗口上再通知它。QQ 收不了 INCR，放不进一个属性的数据只能拒绝。 */
void cb_x11_reply(const struct cb_request *r, Atom type, int format, const void *data, long nitems)
{
    long max = XExtendedMaxRequestSize(cb_xdpy);
    if (!max)
        max = XMaxRequestSize(cb_xdpy);
    if (nitems * (format / 8) > max * 4 - 1024) {
        LOG("QQ paste: %ld bytes do not fit in one X property, refused", nitems * (format / 8));
        notify(r, None);
        return;
    }
    XChangeProperty(cb_xdpy, r->requestor, r->property, type, format, PropModeReplace, data, (int)nitems);
    notify(r, r->property);
}

void cb_x11_refuse(const struct cb_request *r)
{
    notify(r, None);
}

/* 告诉 QQ 它复制的内容已经不是剪贴板内容了（X 服务器在换主人时也是这样通知旧主人的） */
void cb_x11_clear(Window owner)
{
    XSelectionClearEvent ev = {
        .type = SelectionClear, .send_event = True, .display = cb_xdpy, .window = owner,
        .selection = cb_A_CLIPBOARD, .time = CurrentTime,
    };
    XSendEvent(cb_xdpy, owner, False, NoEventMask, (XEvent *)&ev);
    XFlush(cb_xdpy);
}

/* 本连接上只会有迟到的应答，丢掉 */
void cb_x11_drain(void)
{
    XEvent ev;
    while (XPending(cb_xdpy))
        XNextEvent(cb_xdpy, &ev);
}

char *cb_x11_name(Atom a)
{
    char *n = a != None ? XGetAtomName(cb_xdpy, a) : NULL;
    char *r = n ? strdup(n) : NULL;
    if (n)
        XFree(n);
    return r;
}

Atom cb_x11_atom(const char *name)
{
    return XInternAtom(cb_xdpy, name, False);
}
