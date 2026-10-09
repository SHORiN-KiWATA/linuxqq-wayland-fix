/* libqq-clipbridge.so 各文件共用的声明。总体说明见 main.c。 */
#ifndef QQ_CLIPBRIDGE_H
#define QQ_CLIPBRIDGE_H

#include <X11/Xlib.h>
#include <stdint.h>
#include <stdio.h>

#define LOG(...) do { fprintf(stderr, "[qq-clipbridge] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* QQ_CLIPBOARD_DEBUG=1 时多记一些（每次向 QQ 取数据的结果） */
extern int cb_debug;
#define DEBUG(...) do { if (cb_debug) LOG(__VA_ARGS__); } while (0)

/* 库用 -fvisibility=hidden 编译，只有拦截 QQ 调用的函数导出 */
#define EXPORT __attribute__((visibility("default")))

#define MAX_MIMES 64

/* 旧版剪贴板桥给自己的内容加的标记格式，不是内容，不转给 QQ */
#define CB_MARKER_PREFIX "application/x-qq-clipbridge"

/* ---------------- main.c：QQ 的线程与后台线程之间 ---------------- */

/* QQ 的一次粘贴请求，即它调 XConvertSelection 的参数 */
struct cb_request {
    Window requestor;
    Atom target, property;
    Time time;
};

int cb_pop_request(struct cb_request *out);
uint32_t cb_copy_generation(void);
Window cb_qq_owner(void);
Window cb_qq_lost(uint32_t generation);
void cb_go_native(Window proxy, Atom clipboard);
void cb_go_x11(void);
void cb_set_foreign(int present);
void cb_wake(void);
int cb_wake_fd(void);

/* ---------------- bridge.c：后台线程 ---------------- */

void *cb_worker(void *arg);

/* ---------------- x11.c：后台线程自己的 X 连接 ---------------- */

extern Display *cb_xdpy;
extern Window cb_xwin;
extern Atom cb_A_CLIPBOARD, cb_A_TARGETS, cb_A_UTF8, cb_A_GNOME_FILES, cb_A_URI_LIST;

int cb_x11_open(void);
long cb_x11_fetch(Window owner, Atom target, unsigned char **out);
void cb_x11_forward(Window owner, const struct cb_request *r);
void cb_x11_reply(const struct cb_request *r, Atom type, int format, const void *data, long nitems);
void cb_x11_refuse(const struct cb_request *r);
void cb_x11_clear(Window owner);
void cb_x11_drain(void);
char *cb_x11_name(Atom a);
Atom cb_x11_atom(const char *name);

/* ---------------- formats.c：X11 target 与 Wayland MIME 的对应 ---------------- */

struct cb_mimes {
    char *v[MAX_MIMES];
    int n;
};

void cb_mimes_add(struct cb_mimes *m, const char *mime);
void cb_mimes_clear(struct cb_mimes *m);
int cb_mimes_has(const struct cb_mimes *m, const char *mime);
void cb_mimes_join(const struct cb_mimes *m, char *buf, size_t cap);
int cb_is_text_name(const char *n);
int cb_is_content_name(const char *n);
void cb_offers_from_qq(const struct cb_mimes *targets, int files_ok, struct cb_mimes *out);
long cb_targets_for_qq(const struct cb_mimes *offer, Atom *out, long cap);
const char *cb_mime_for_target(const struct cb_mimes *offer, const char *target, int *to_gnome);
char *cb_paths_to_uris(const unsigned char *in, long n, const char *sep, long *outlen);
char *cb_uris_to_gnome(const unsigned char *in, long n, long *outlen);

/* ---------------- wayland.c：ext-data-control（没有时用 wlr-data-control） ---------------- */

struct ext_data_control_offer_v1;
struct wl_display;

/* Wayland 剪贴板上的一份内容 */
struct cb_offer {
    struct ext_data_control_offer_v1 *obj;
    struct cb_mimes mimes;
};

extern struct wl_display *cb_wdpy;

int cb_wl_open(void);
const char *cb_wl_protocol(void);
int cb_wl_set_source(const struct cb_mimes *mimes);
int cb_wl_receive(struct cb_offer *offer, const char *mime);
void cb_offer_free(struct cb_offer *offer);

/* wayland.c 收到事件时回调（实现在 bridge.c） */
void cb_on_selection(struct cb_offer *offer);
void cb_on_send(const char *mime, int fd);
void cb_on_cancelled(void);

#endif
