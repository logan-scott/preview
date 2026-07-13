#ifndef PREVIEW_STR_H
#define PREVIEW_STR_H

#include <stddef.h>
#include <stdint.h>

/* Growable string buffer. All appenders keep buf NUL-terminated. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sb;

void sb_init(sb *s);
void sb_free(sb *s);
void sb_append_n(sb *s, const char *data, size_t n);
void sb_append(sb *s, const char *cstr);
void sb_appendf(sb *s, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Append with &<>"' escaped for HTML. */
void sb_append_html(sb *s, const char *data, size_t n);
/* Append base64-encoded binary. */
void sb_append_base64(sb *s, const uint8_t *data, size_t n);
/* Detach the buffer from the builder (caller frees). */
char *sb_take(sb *s);

/* Read an entire file. Returns malloc'd buffer (NUL-terminated one past
 * *out_len for convenience) or NULL with *err set to a static message. */
uint8_t *read_entire_file(const char *path, size_t *out_len,
                          const char **err);

#endif
