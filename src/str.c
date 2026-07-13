#include "str.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void sb_grow(sb *s, size_t need) {
    if (s->len + need + 1 <= s->cap)
        return;
    size_t cap = s->cap ? s->cap : 256;
    while (cap < s->len + need + 1)
        cap *= 2;
    char *nb = realloc(s->buf, cap);
    if (!nb) {
        fprintf(stderr, "preview: out of memory\n");
        exit(1);
    }
    s->buf = nb;
    s->cap = cap;
}

void sb_init(sb *s) {
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
    sb_grow(s, 0);
    s->buf[0] = '\0';
}

void sb_free(sb *s) {
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
}

void sb_append_n(sb *s, const char *data, size_t n) {
    sb_grow(s, n);
    memcpy(s->buf + s->len, data, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

void sb_append(sb *s, const char *cstr) { sb_append_n(s, cstr, strlen(cstr)); }

void sb_appendf(sb *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    sb_grow(s, (size_t)n);
    vsnprintf(s->buf + s->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    s->len += (size_t)n;
}

void sb_append_html(sb *s, const char *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        switch (c) {
        case '&': sb_append(s, "&amp;"); break;
        case '<': sb_append(s, "&lt;"); break;
        case '>': sb_append(s, "&gt;"); break;
        case '"': sb_append(s, "&quot;"); break;
        case '\'': sb_append(s, "&#39;"); break;
        default: sb_append_n(s, &c, 1);
        }
    }
}

void sb_append_base64(sb *s, const uint8_t *data, size_t n) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    sb_grow(s, ((n + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 |
                     data[i + 2];
        char out[4] = {tbl[v >> 18], tbl[(v >> 12) & 63], tbl[(v >> 6) & 63],
                       tbl[v & 63]};
        sb_append_n(s, out, 4);
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)data[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2)
            v |= (uint32_t)data[i + 1] << 8;
        char out[4] = {tbl[v >> 18], tbl[(v >> 12) & 63],
                       rem == 2 ? tbl[(v >> 6) & 63] : '=', '='};
        sb_append_n(s, out, 4);
    }
}

char *sb_take(sb *s) {
    char *b = s->buf;
    s->buf = NULL;
    s->len = s->cap = 0;
    return b;
}

uint8_t *read_entire_file(const char *path, size_t *out_len,
                          const char **err) {
    *err = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err = strerror(errno);
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        *err = strerror(errno);
        fclose(f);
        return NULL;
    }
    if (S_ISDIR(st.st_mode)) {
        *err = "is a directory";
        fclose(f);
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        *err = "out of memory";
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        *err = "short read";
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}
