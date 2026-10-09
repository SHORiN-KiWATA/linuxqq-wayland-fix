/*
 * Wayland 一侧：data-control 设备、剪贴板上的内容（offer）和我们放上去的数据源（source）。
 *
 * 优先使用 ext-data-control-v1，没有时退回 wlr-data-control-unstable-v1。
 * 两者的请求和事件完全一致，只有「创建对象」的两个请求需要区分协议；
 * 其余调用都用 ext 的函数，libwayland 按对象自身的接口编码消息，对 wlr 对象同样正确。
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "clipbridge.h"
#include "ext-data-control-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"

struct wl_display *cb_wdpy;
static struct wl_seat *seat;
static struct ext_data_control_manager_v1 *dcm;     /* ext 管理器 */
static struct zwlr_data_control_manager_v1 *wlr_dcm; /* wlr 管理器（没有 ext 时用） */
static struct ext_data_control_device_v1 *device;
static struct ext_data_control_source_v1 *source;
const char *cb_wl_protocol(void)
{
    return dcm ? "ext-data-control" : "wlr-data-control";
}

/* ---------------- 剪贴板上的内容 ---------------- */

static void offer_mime(void *data, struct ext_data_control_offer_v1 *obj, const char *mime)
{
    (void)obj;
    cb_mimes_add(&((struct cb_offer *)data)->mimes, mime);
}

static const struct ext_data_control_offer_v1_listener offer_listener = { .offer = offer_mime };

void cb_offer_free(struct cb_offer *offer)
{
    if (!offer)
        return;
    ext_data_control_offer_v1_destroy(offer->obj);
    cb_mimes_clear(&offer->mimes);
    free(offer);
}

/* 读取 offer 的 mime 格式：返回管道读端，数据由对方写进来 */
int cb_wl_receive(struct cb_offer *offer, const char *mime)
{
    int fds[2];
    if (!offer || pipe2(fds, O_CLOEXEC) < 0)
        return -1;
    ext_data_control_offer_v1_receive(offer->obj, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(cb_wdpy);
    return fds[0];
}

/* ---------------- 设备 ---------------- */

/* 合成器先发 data_offer（随后若干 offer 事件列出格式），再发 selection */
static void dev_data_offer(void *d, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *obj)
{
    (void)d; (void)dev;
    struct cb_offer *offer = calloc(1, sizeof(*offer));
    offer->obj = obj;
    ext_data_control_offer_v1_add_listener(obj, &offer_listener, offer);
}

static void dev_selection(void *d, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *obj)
{
    (void)d; (void)dev;
    cb_on_selection(obj ? ext_data_control_offer_v1_get_user_data(obj) : NULL);
}

static void dev_finished(void *d, struct ext_data_control_device_v1 *dev)
{
    (void)d;
    ext_data_control_device_v1_destroy(dev);
    device = NULL;
    LOG("data-control device finished by the compositor");
}

static void dev_primary(void *d, struct ext_data_control_device_v1 *dev, struct ext_data_control_offer_v1 *obj)
{
    (void)d; (void)dev;
    if (obj)
        cb_offer_free(ext_data_control_offer_v1_get_user_data(obj));
}

static const struct ext_data_control_device_v1_listener device_listener = {
    .data_offer = dev_data_offer,
    .selection = dev_selection,
    .finished = dev_finished,
    .primary_selection = dev_primary,
};

/* ---------------- 我们的数据源 ---------------- */

static void source_send(void *data, struct ext_data_control_source_v1 *src, const char *mime, int32_t fd)
{
    (void)data; (void)src;
    cb_on_send(mime, fd);
}

static void source_cancelled(void *data, struct ext_data_control_source_v1 *src)
{
    (void)data;
    ext_data_control_source_v1_destroy(src);
    if (src == source) {
        source = NULL;
        cb_on_cancelled();
    }
}

static const struct ext_data_control_source_v1_listener source_listener = {
    .send = source_send,
    .cancelled = source_cancelled,
};

/* 把 mimes 作为新的剪贴板内容；替换掉我们之前放的 */
int cb_wl_set_source(const struct cb_mimes *mimes)
{
    if (!device)
        return -1;
    if (source)
        ext_data_control_source_v1_destroy(source);
    source = dcm ? ext_data_control_manager_v1_create_data_source(dcm)
                 : (struct ext_data_control_source_v1 *)zwlr_data_control_manager_v1_create_data_source(wlr_dcm);
    ext_data_control_source_v1_add_listener(source, &source_listener, NULL);
    for (int i = 0; i < mimes->n; ++i)
        ext_data_control_source_v1_offer(source, mimes->v[i]);
    ext_data_control_device_v1_set_selection(device, source);
    wl_display_flush(cb_wdpy);
    return 0;
}

/* ---------------- 连接 ---------------- */

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version)
{
    (void)d;
    if (!strcmp(iface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, ext_data_control_manager_v1_interface.name))
        dcm = wl_registry_bind(r, name, &ext_data_control_manager_v1_interface, 1);
    else if (!strcmp(iface, zwlr_data_control_manager_v1_interface.name))
        wlr_dcm = wl_registry_bind(r, name, &zwlr_data_control_manager_v1_interface, version >= 2 ? 2 : 1);
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d; (void)r; (void)name;
}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* 成功返回 Wayland 连接的 fd；合成器两种 data-control 都没有时返回 -1 */
int cb_wl_open(void)
{
    cb_wdpy = wl_display_connect(NULL);
    if (!cb_wdpy)
        return -1;
    struct wl_registry *reg = wl_display_get_registry(cb_wdpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(cb_wdpy);
    if (!seat || (!dcm && !wlr_dcm)) {
        LOG("compositor supports neither ext-data-control nor wlr-data-control (e.g. GNOME); "
            "QQ keeps using the X11 clipboard");
        wl_display_disconnect(cb_wdpy);
        cb_wdpy = NULL;
        return -1;
    }
    device = dcm ? ext_data_control_manager_v1_get_data_device(dcm, seat)
                 : (struct ext_data_control_device_v1 *)zwlr_data_control_manager_v1_get_data_device(wlr_dcm, seat);
    ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
    wl_display_roundtrip(cb_wdpy);
    return wl_display_get_fd(cb_wdpy);
}
