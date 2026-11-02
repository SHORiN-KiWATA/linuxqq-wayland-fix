/*
 * qq-screenshot-helper：替 QQ 拿屏幕画面（两种模式），原始像素经 stdout 回传。
 *
 * 用法：qq-screenshot-helper [kde|portal] > 输出
 *   输出 = struct shot_header + stride*height 字节原始像素。
 *   像素格式由 header 里的 QImage::Format 表示（kde 模式用 KWin 返回的，
 *   portal 模式固定为 RGBA8888=17）。失败时退出码非 0，原因打到 stderr。
 *
 *   kde 模式（默认）：调 KWin 的 org.kde.KWin.ScreenShot2.CaptureWorkspace。
 *     KWin 会检查调用方可执行文件（/proc/<pid>/exe）对应的 .desktop 里有没有
 *       X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
 *     所以本 helper 配了 qq-screenshot-helper.desktop 声明受限接口；QQ 自己
 *     的 qq.desktop 没有这一条，直接调会被拒绝。机制与 Spectacle 相同。
 *
 *   portal 模式：调 org.freedesktop.portal.Screenshot（interactive=false），
 *     等 Response 拿 PNG 文件 URI，自己解码成 RGBA 后回传、删掉文件。
 *     GNOME 上这是唯一官方截图路（会弹 GNOME 的截图对话框，用户确认后出图）；
 *     KDE 的 portal 后端是静默的（无对话框）。QQ 进程里因此不需要跑 GLib
 *     主循环，库侧只做裁剪。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define KDE_SERVICE "org.kde.KWin.ScreenShot2"
#define KDE_OBJECT  "/org/kde/KWin/ScreenShot2"
#define KDE_IFACE   "org.kde.KWin.ScreenShot2"
#define KDE_METHOD  "CaptureWorkspace"

#define PORTAL_SERVICE "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT  "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE   "org.freedesktop.portal.Screenshot"

struct shot_header {
    char magic[4]; /* "QQKS" */
    uint32_t width, height, stride, format; /* QImage::Format */
    double scale;
};

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_image(const struct shot_header *hdr, const void *pix)
{
    if (write_all(STDOUT_FILENO, hdr, sizeof *hdr) != 0)
        return -1;
    return write_all(STDOUT_FILENO, pix, (size_t)hdr->stride * hdr->height);
}

/* ---------------- kde 模式：KWin ScreenShot2 ---------------- */

static int kde_capture(void)
{
    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) {
        fprintf(stderr, "[qq-shot-helper] session bus: %s\n", err->message);
        return 1;
    }

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        perror("[qq-shot-helper] pipe2");
        return 1;
    }

    GUnixFDList *fdlist = g_unix_fd_list_new();
    int idx = g_unix_fd_list_append(fdlist, fds[1], &err);
    close(fds[1]); /* KWin 收到后会 dup，自己的写端可以关了 */
    if (idx < 0) {
        fprintf(stderr, "[qq-shot-helper] fd list: %s\n", err->message);
        return 1;
    }

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    /* native-resolution=false 时 KWin 按逻辑尺寸渲染整张工作区（4K @1.5 只给
     * 2560x1440），库侧再放大到 QQ 请求的原生尺寸就是糊的。取回设备像素。 */
    g_variant_builder_add(&opts, "{sv}", "native-resolution", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&opts, "{sv}", "include-cursor", g_variant_new_boolean(FALSE));

    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(bus, KDE_SERVICE, KDE_OBJECT,
                                                                    KDE_IFACE, KDE_METHOD,
                                                                    g_variant_new("(a{sv}h)", &opts, idx),
                                                                    G_VARIANT_TYPE("(a{sv})"),
                                                                    G_DBUS_CALL_FLAGS_NONE, 10000,
                                                                    fdlist, NULL, NULL, &err);
    g_object_unref(fdlist);
    if (!reply) {
        fprintf(stderr, "[qq-shot-helper] %s failed: %s\n", KDE_METHOD, err->message);
        return 1;
    }

    GVariant *dict = NULL;
    g_variant_get(reply, "(@a{sv})", &dict);
    char *type = NULL;
    uint32_t width = 0, height = 0, stride = 0, format = 0;
    double scale = 1.0;
    int ok = g_variant_lookup(dict, "type", "s", &type) &&
             g_variant_lookup(dict, "width", "u", &width) &&
             g_variant_lookup(dict, "height", "u", &height) &&
             g_variant_lookup(dict, "stride", "u", &stride) &&
             g_variant_lookup(dict, "format", "u", &format);
    g_variant_lookup(dict, "scale", "d", &scale);
    g_variant_unref(dict);
    g_variant_unref(reply);

    if (!ok || !type || strcmp(type, "raw") || !width || !height || !stride) {
        fprintf(stderr, "[qq-shot-helper] unexpected reply (type=%s %ux%u stride=%u format=%u)\n",
                type ? type : "?", width, height, stride, format);
        g_free(type);
        return 1;
    }
    g_free(type);

    struct shot_header hdr;
    memcpy(hdr.magic, "QQKS", 4);
    hdr.width = width;
    hdr.height = height;
    hdr.stride = stride;
    hdr.format = format;
    hdr.scale = scale;

    if (write_all(STDOUT_FILENO, &hdr, sizeof hdr) != 0) {
        fprintf(stderr, "[qq-shot-helper] write header: %s\n", strerror(errno));
        return 1;
    }

    size_t total = (size_t)stride * height, done = 0;
    char buf[65536];
    while (done < total) {
        size_t want = total - done;
        if (want > sizeof buf)
            want = sizeof buf;
        ssize_t n = read(fds[0], buf, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("[qq-shot-helper] read image");
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "[qq-shot-helper] short image (%zu/%zu)\n", done, total);
            return 1;
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
            fprintf(stderr, "[qq-shot-helper] write image: %s\n", strerror(errno));
            return 1;
        }
        done += (size_t)n;
    }
    close(fds[0]);
    return 0;
}

/* ---------------- portal 模式：org.freedesktop.portal.Screenshot ---------------- */

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* 最小 PNG 解码：8 位、非隔行、RGB(2)/RGBA(6)，输出 RGBA8888 字节序。 */
static uint8_t *png_decode(const uint8_t *d, size_t n, int *ow, int *oh)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n < 8 || memcmp(d, sig, 8) != 0)
        return NULL;

    size_t off = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    uint8_t *idat = NULL;
    size_t idat_len = 0;

    while (off + 12 <= n) {
        uint32_t clen = rd32be(d + off);
        const uint8_t *type = d + off + 4;
        if (off + 12 + clen > n)
            break;
        const uint8_t *data = d + off + 8;
        if (!memcmp(type, "IHDR", 4) && clen >= 13) {
            w = rd32be(data);
            h = rd32be(data + 4);
            depth = data[8];
            color = data[9];
            interlace = data[12];
        } else if (!memcmp(type, "IDAT", 4)) {
            uint8_t *p = realloc(idat, idat_len + clen);
            if (!p) {
                free(idat);
                return NULL;
            }
            idat = p;
            memcpy(idat + idat_len, data, clen);
            idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        off += 12 + clen;
    }

    if (!w || !h || depth != 8 || interlace != 0 || (color != 6 && color != 2)) {
        free(idat);
        return NULL;
    }
    int bpp = color == 6 ? 4 : 3;
    size_t stride = (size_t)w * bpp;
    size_t rawlen = (stride + 1) * h;
    uint8_t *raw = malloc(rawlen);
    uLongf outlen = (uLongf)rawlen;
    if (!raw || uncompress(raw, &outlen, idat, (uLong)idat_len) != Z_OK || outlen != rawlen) {
        fprintf(stderr, "[qq-shot-helper] PNG inflate failed\n");
        free(raw);
        free(idat);
        return NULL;
    }
    free(idat);

    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) {
        free(raw);
        return NULL;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *cur = raw + (size_t)y * (stride + 1);
        uint8_t filter = *cur++;
        uint8_t *prev = y ? raw + (size_t)(y - 1) * (stride + 1) + 1 : NULL;
        for (size_t x = 0; x < stride; x++) {
            uint8_t a = x >= (size_t)bpp ? cur[x - bpp] : 0;
            uint8_t b = prev ? prev[x] : 0;
            uint8_t c = (prev && x >= (size_t)bpp) ? prev[x - bpp] : 0;
            switch (filter) {
            case 0: break;
            case 1: cur[x] = (uint8_t)(cur[x] + a); break;
            case 2: cur[x] = (uint8_t)(cur[x] + b); break;
            case 3: cur[x] = (uint8_t)(cur[x] + (a + b) / 2); break;
            case 4: {
                int p = (int)a + (int)b - (int)c;
                int pa = abs(p - (int)a), pb = abs(p - (int)b), pc = abs(p - (int)c);
                cur[x] = (uint8_t)(cur[x] + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c)));
                break;
            }
            default:
                free(raw);
                free(rgba);
                return NULL;
            }
        }
        uint8_t *dst = rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            if (bpp == 4) {
                dst[x * 4] = cur[x * 4];
                dst[x * 4 + 1] = cur[x * 4 + 1];
                dst[x * 4 + 2] = cur[x * 4 + 2];
                dst[x * 4 + 3] = cur[x * 4 + 3];
            } else {
                dst[x * 4] = cur[x * 3];
                dst[x * 4 + 1] = cur[x * 3 + 1];
                dst[x * 4 + 2] = cur[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
    }
    free(raw);
    *ow = (int)w;
    *oh = (int)h;
    return rgba;
}

struct portal_state {
    GMainLoop *loop;
    guint32 response;
    char *uri;
};

static void portal_response(GDBusConnection *conn, const char *sender, const char *path,
                            const char *iface, const char *signal, GVariant *params,
                            gpointer user_data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal;
    struct portal_state *st = user_data;
    guint32 response = 2;
    GVariant *results = NULL;
    g_variant_get(params, "(u@a{sv})", &response, &results);
    st->response = response;
    if (response == 0 && results) {
        const char *uri = NULL;
        if (g_variant_lookup(results, "uri", "&s", &uri))
            st->uri = g_strdup(uri);
    }
    if (results)
        g_variant_unref(results);
    g_main_loop_quit(st->loop);
}

static gboolean portal_timeout(gpointer data)
{
    struct portal_state *st = data;
    fprintf(stderr, "[qq-shot-helper] portal screenshot timed out\n");
    g_main_loop_quit(st->loop);
    return G_SOURCE_REMOVE;
}

static int portal_capture(void)
{
    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) {
        fprintf(stderr, "[qq-shot-helper] session bus: %s\n", err->message);
        return 1;
    }

    static int counter;
    char token[64];
    snprintf(token, sizeof token, "qqscreenshot%d_%d", (int)getpid(), ++counter);

    /* 请求对象路径：/org/freedesktop/portal/desktop/request/<sender>/<token>，
     * sender 是唯一名去掉开头的 ':'、'.' 换成 '_'。 */
    const char *unique = g_dbus_connection_get_unique_name(bus);
    if (*unique == ':')
        unique++;
    char sender[128];
    size_t j = 0;
    for (const char *p = unique; *p && j < sizeof sender - 1; p++)
        sender[j++] = (*p == '.') ? '_' : *p;
    sender[j] = '\0';

    char reqpath[256];
    snprintf(reqpath, sizeof reqpath,
             "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);

    struct portal_state st = { 0 };
    st.loop = g_main_loop_new(NULL, FALSE);
    guint sub = g_dbus_connection_signal_subscribe(bus, PORTAL_SERVICE,
                                                   "org.freedesktop.portal.Request", "Response",
                                                   reqpath, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                   portal_response, &st, NULL);

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&opts, "{sv}", "interactive", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(FALSE));

    GVariant *reply = g_dbus_connection_call_sync(bus, PORTAL_SERVICE, PORTAL_OBJECT,
                                                  PORTAL_IFACE, "Screenshot",
                                                  g_variant_new("(sa{sv})", "", &opts),
                                                  G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE,
                                                  10000, NULL, &err);
    if (!reply) {
        fprintf(stderr, "[qq-shot-helper] portal Screenshot call failed: %s\n", err->message);
        g_dbus_connection_signal_unsubscribe(bus, sub);
        return 1;
    }

    /* 规范里 request 路径可以按 sender+token 推算，但实际以返回的 handle 为准；
     * 两个路径都订阅，避免推算不一致时漏掉 Response。 */
    const char *handle = NULL;
    g_variant_get(reply, "(&o)", &handle);
    fprintf(stderr, "[qq-shot-helper] portal handle=%s (expected=%s)\n",
            handle ? handle : "?", reqpath);
    guint sub2 = 0;
    if (handle && strcmp(handle, reqpath) != 0)
        sub2 = g_dbus_connection_signal_subscribe(bus, PORTAL_SERVICE,
                                                  "org.freedesktop.portal.Request", "Response",
                                                  handle, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
                                                  portal_response, &st, NULL);
    g_variant_unref(reply);

    /* GNOME 会弹截图对话框等用户确认，超时默认 60s，可用
     * QQ_SCREENSHOT_PORTAL_TIMEOUT（秒）放宽 */
    guint timeout = 60;
    const char *tv = getenv("QQ_SCREENSHOT_PORTAL_TIMEOUT");
    if (tv && *tv)
        timeout = (guint)atoi(tv);
    if (!timeout)
        timeout = 60;
    g_timeout_add_seconds(timeout, portal_timeout, &st);
    g_main_loop_run(st.loop);
    g_dbus_connection_signal_unsubscribe(bus, sub);
    if (sub2)
        g_dbus_connection_signal_unsubscribe(bus, sub2);
    g_main_loop_unref(st.loop);

    if (st.response != 0 || !st.uri) {
        fprintf(stderr, "[qq-shot-helper] portal screenshot not taken (response=%u)\n", st.response);
        g_free(st.uri);
        return 1;
    }

    char *path = g_filename_from_uri(st.uri, NULL, &err);
    g_free(st.uri);
    if (!path) {
        fprintf(stderr, "[qq-shot-helper] bad uri: %s\n", err->message);
        return 1;
    }

    char *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, &err)) {
        fprintf(stderr, "[qq-shot-helper] read %s: %s\n", path, err->message);
        g_free(path);
        return 1;
    }
    unlink(path); /* 抓完就删：QQ 每次截图不该在 ~/Pictures 里留文件 */
    fprintf(stderr, "[qq-shot-helper] portal screenshot file %s (%zu bytes)\n", path, (size_t)len);
    g_free(path);

    int iw = 0, ih = 0;
    uint8_t *rgba = png_decode((const uint8_t *)data, (size_t)len, &iw, &ih);
    g_free(data);
    if (!rgba) {
        fprintf(stderr, "[qq-shot-helper] unsupported PNG\n");
        return 1;
    }

    struct shot_header hdr;
    memcpy(hdr.magic, "QQKS", 4);
    hdr.width = (uint32_t)iw;
    hdr.height = (uint32_t)ih;
    hdr.stride = (uint32_t)iw * 4;
    hdr.format = 17; /* QImage::Format_RGBA8888 */
    hdr.scale = 1.0;
    if (write_image(&hdr, rgba) != 0) {
        fprintf(stderr, "[qq-shot-helper] write image: %s\n", strerror(errno));
        free(rgba);
        return 1;
    }
    free(rgba);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "portal") == 0) {
        int timeout = 60;
        const char *tv = getenv("QQ_SCREENSHOT_PORTAL_TIMEOUT");
        if (tv && *tv)
            timeout = atoi(tv);
        if (timeout <= 0)
            timeout = 60;
        alarm((unsigned)(timeout + 30)); /* GNOME 的对话框可能要等用户点确认 */
        return portal_capture();
    }
    alarm(30);
    return kde_capture();
}
