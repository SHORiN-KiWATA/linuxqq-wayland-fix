/* QQ 的 X11 target 与 Wayland MIME 怎么对应。 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "clipbridge.h"

void cb_mimes_add(struct cb_mimes *m, const char *mime)
{
    if (cb_mimes_has(m, mime) || m->n >= MAX_MIMES)
        return;
    m->v[m->n++] = strdup(mime);
}

void cb_mimes_clear(struct cb_mimes *m)
{
    for (int i = 0; i < m->n; ++i)
        free(m->v[i]);
    m->n = 0;
}

int cb_mimes_has(const struct cb_mimes *m, const char *mime)
{
    for (int i = 0; m && i < m->n; ++i)
        if (!strcmp(m->v[i], mime))
            return 1;
    return 0;
}

void cb_mimes_join(const struct cb_mimes *m, char *buf, size_t cap)
{
    buf[0] = 0;
    for (int i = 0; i < m->n && strlen(buf) + strlen(m->v[i]) + 2 < cap; ++i) {
        strcat(buf, m->v[i]);
        strcat(buf, " ");
    }
}

int cb_is_text_name(const char *n)
{
    return !strcmp(n, "UTF8_STRING") || !strcmp(n, "STRING") || !strcmp(n, "TEXT") ||
           !strncmp(n, "text/plain", 10);
}

/*
 * 是不是内容格式。X11 选区协议自己的名字（xwayland-satellite 等会把它们原样转到 Wayland）、
 * 我们自己或旧版桥的标记都不是。名字不带 / 的也算内容：QQ 的私有格式就是这样
 * （QQ_Unicode_RichEdit_Format：表情、图文混排；QQ_MultiMsg_RichEdit_Format：合并转发）。
 */
int cb_is_content_name(const char *n)
{
    static const char *const protocol[] = {
        "TARGETS", "MULTIPLE", "TIMESTAMP", "SAVE_TARGETS", "DELETE", "INCR", "INSERT_SELECTION", "INSERT_PROPERTY",
    };
    for (size_t i = 0; i < sizeof(protocol) / sizeof(*protocol); ++i)
        if (!strcmp(n, protocol[i]))
            return 0;
    return *n && strncmp(n, CB_MARKER_PREFIX, strlen(CB_MARKER_PREFIX));
}

/*
 * QQ 复制时给的 TARGETS → 在 Wayland 上提供的格式：
 *   - 文本（UTF8_STRING 等）同时以 text/plain;charset=utf-8、text/plain 和 X11 的名字提供；
 *   - 其余格式原样提供，包括 QQ 的私有格式（别的 QQ 进程粘贴表情、合并转发时要用）；
 *   - 有文件时 text/uri-list 放最前面，并去掉 text/html：QQ 的 HTML 只是 <img src=缓存路径>，
 *     很多程序会优先选它，结果拿不到图；
 *   - 文件不存在（复制了原图还没下载的图片）时只提供图片数据和 QQ 的私有格式。
 */
void cb_offers_from_qq(const struct cb_mimes *targets, int files_ok, struct cb_mimes *out)
{
    cb_mimes_clear(out);
    int files = cb_mimes_has(targets, "x-special/gnome-copied-files") || cb_mimes_has(targets, "text/uri-list");
    int image_only = files && !files_ok && cb_mimes_has(targets, "image/png");
    if (files && !image_only) {
        cb_mimes_add(out, "text/uri-list");
        cb_mimes_add(out, "x-special/gnome-copied-files");
    }
    for (int i = 0; i < targets->n; ++i) {
        const char *t = targets->v[i];
        if (!cb_is_content_name(t) || !strcmp(t, "text/uri-list") || !strcmp(t, "x-special/gnome-copied-files"))
            continue;
        if (image_only && strcmp(t, "image/png") && strncmp(t, "QQ_", 3))
            continue;
        if (cb_is_text_name(t)) {
            cb_mimes_add(out, "text/plain;charset=utf-8");
            cb_mimes_add(out, "text/plain");
            cb_mimes_add(out, "UTF8_STRING");
            cb_mimes_add(out, "STRING");
            cb_mimes_add(out, "TEXT");
        } else if (!(files && !strcmp(t, "text/html"))) {
            cb_mimes_add(out, t);
        }
    }
}

/* Wayland 上别人的内容 → 给 QQ 的 TARGETS（format 32 的 Atom 数组），返回个数。 */
long cb_targets_for_qq(const struct cb_mimes *offer, Atom *out, long cap)
{
    long n = 0;
    out[n++] = cb_A_TARGETS;
    int text = 0;
    for (int i = 0; i < offer->n && n < cap - 6; ++i) {
        const char *m = offer->v[i];
        if (cb_is_text_name(m))
            text = 1;
        else if (cb_is_content_name(m))
            out[n++] = cb_x11_atom(m);
    }
    if (text) {
        out[n++] = cb_A_UTF8;
        out[n++] = cb_x11_atom("STRING");
        out[n++] = cb_x11_atom("TEXT");
        out[n++] = cb_x11_atom("text/plain;charset=utf-8");
        out[n++] = cb_x11_atom("text/plain");
    }
    if (cb_mimes_has(offer, "text/uri-list") && !cb_mimes_has(offer, "x-special/gnome-copied-files"))
        out[n++] = cb_A_GNOME_FILES;
    return n;
}

/* QQ 要某个 target 时从 Wayland 的哪个格式取；*to_gnome 表示要把 uri-list 转成 gnome-copied-files。 */
const char *cb_mime_for_target(const struct cb_mimes *offer, const char *target, int *to_gnome)
{
    *to_gnome = 0;
    if (cb_is_text_name(target)) {
        static const char *const pref[] = { "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT" };
        for (size_t i = 0; i < sizeof(pref) / sizeof(*pref); ++i)
            if (cb_mimes_has(offer, pref[i]))
                return pref[i];
        return NULL;
    }
    if (!strcmp(target, "x-special/gnome-copied-files") && !cb_mimes_has(offer, target) &&
        cb_mimes_has(offer, "text/uri-list")) {
        *to_gnome = 1;
        return "text/uri-list";
    }
    for (int i = 0; i < offer->n; ++i)
        if (!strcmp(offer->v[i], target))
            return offer->v[i];
    return NULL;
}

/* ---- 文件列表 ---- */

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

/* 「每行一个路径或 URI」→ URI 列表（行分隔符为 sep）。QQ 给的常是裸路径；跳过 copy/cut 行。 */
char *cb_paths_to_uris(const unsigned char *in, long n, const char *sep, long *outlen)
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
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        strchr("/-_.~!$&'()*+,;=:@", ch)) {
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

/* text/uri-list（CRLF 分隔，# 开头是注释）→ x-special/gnome-copied-files（"copy" + 每行一个 URI） */
char *cb_uris_to_gnome(const unsigned char *in, long n, long *outlen)
{
    char *out = NULL;
    size_t len = 0, cap = 0;
    append(&out, &len, &cap, "copy", 4);
    const char *p = (const char *)in, *end = p + n;
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\r' && *e != '\n')
            ++e;
        if (e > p && *p != '#') {
            append(&out, &len, &cap, "\n", 1);
            append(&out, &len, &cap, p, e - p);
        }
        p = e + 1;
    }
    *outlen = len;
    return out;
}
