#include "xmlmini.h"

#include <stdlib.h>
#include <string.h>

void xml_init(xml_reader *x, const char *data, size_t len) {
    x->p = data;
    x->end = data + len;
    x->name[0] = '\0';
    x->attrs = NULL;
    x->attrs_len = 0;
    x->text = NULL;
    x->text_len = 0;
}

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Advance past "needle", return 1 if found. */
static int skip_past(xml_reader *x, const char *needle) {
    size_t n = strlen(needle);
    while (x->p + n <= x->end) {
        if (memcmp(x->p, needle, n) == 0) {
            x->p += n;
            return 1;
        }
        x->p++;
    }
    x->p = x->end;
    return 0;
}

static void read_name(xml_reader *x) {
    size_t i = 0;
    while (x->p < x->end && !is_ws(*x->p) && *x->p != '>' && *x->p != '/') {
        if (i < sizeof(x->name) - 1)
            x->name[i++] = *x->p;
        x->p++;
    }
    x->name[i] = '\0';
}

xml_event xml_next(xml_reader *x) {
    for (;;) {
        if (x->p >= x->end)
            return XML_EOF;

        if (*x->p != '<') { /* text run */
            x->text = x->p;
            while (x->p < x->end && *x->p != '<')
                x->p++;
            x->text_len = (size_t)(x->p - x->text);
            return XML_TEXT;
        }

        /* *x->p == '<' */
        if (x->p + 1 >= x->end)
            return XML_EOF;

        if (x->p[1] == '?') { /* <?xml ... ?> */
            skip_past(x, "?>");
            continue;
        }
        if (x->p + 3 < x->end && memcmp(x->p, "<!--", 4) == 0) {
            skip_past(x, "-->");
            continue;
        }
        if (x->p + 8 < x->end && memcmp(x->p, "<![CDATA[", 9) == 0) {
            x->p += 9;
            x->text = x->p;
            const char *start = x->p;
            if (!skip_past(x, "]]>")) {
                x->text_len = (size_t)(x->end - start);
                return XML_TEXT;
            }
            x->text_len = (size_t)(x->p - start - 3);
            return XML_TEXT;
        }
        if (x->p[1] == '!') { /* <!DOCTYPE ...> — not nested in OOXML */
            skip_past(x, ">");
            continue;
        }
        if (x->p[1] == '/') { /* end tag */
            x->p += 2;
            read_name(x);
            skip_past(x, ">");
            return XML_ELEM_END;
        }

        /* start or empty tag */
        x->p += 1;
        read_name(x);
        x->attrs = x->p;
        int quote = 0;
        while (x->p < x->end) {
            char c = *x->p;
            if (quote) {
                if (c == quote)
                    quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
            x->p++;
        }
        const char *gt = x->p;
        if (x->p < x->end)
            x->p++; /* consume '>' */
        int empty = gt > x->attrs && gt[-1] == '/';
        x->attrs_len = (size_t)(gt - x->attrs) - (empty ? 1 : 0);
        return empty ? XML_ELEM_EMPTY : XML_ELEM_START;
    }
}

void xml_decode_into(sb *out, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (s[i] != '&') {
            sb_append_n(out, s + i, 1);
            i++;
            continue;
        }
        /* entity */
        size_t semi = i + 1;
        while (semi < n && semi < i + 12 && s[semi] != ';')
            semi++;
        if (semi >= n || s[semi] != ';') {
            sb_append_n(out, s + i, 1);
            i++;
            continue;
        }
        size_t elen = semi - i - 1;
        const char *e = s + i + 1;
        if (elen == 3 && memcmp(e, "amp", 3) == 0)
            sb_append(out, "&");
        else if (elen == 2 && memcmp(e, "lt", 2) == 0)
            sb_append(out, "<");
        else if (elen == 2 && memcmp(e, "gt", 2) == 0)
            sb_append(out, ">");
        else if (elen == 4 && memcmp(e, "quot", 4) == 0)
            sb_append(out, "\"");
        else if (elen == 4 && memcmp(e, "apos", 4) == 0)
            sb_append(out, "'");
        else if (elen > 1 && e[0] == '#') {
            long cp = e[1] == 'x' || e[1] == 'X'
                          ? strtol(e + 2, NULL, 16)
                          : strtol(e + 1, NULL, 10);
            /* encode code point as UTF-8 */
            char b[4];
            if (cp < 0x80) {
                b[0] = (char)cp;
                sb_append_n(out, b, 1);
            } else if (cp < 0x800) {
                b[0] = (char)(0xC0 | (cp >> 6));
                b[1] = (char)(0x80 | (cp & 0x3F));
                sb_append_n(out, b, 2);
            } else if (cp < 0x10000) {
                b[0] = (char)(0xE0 | (cp >> 12));
                b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                b[2] = (char)(0x80 | (cp & 0x3F));
                sb_append_n(out, b, 3);
            } else {
                b[0] = (char)(0xF0 | (cp >> 18));
                b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                b[3] = (char)(0x80 | (cp & 0x3F));
                sb_append_n(out, b, 4);
            }
        } else {
            sb_append_n(out, s + i, semi - i + 1); /* unknown: keep raw */
        }
        i = semi + 1;
    }
}

char *xml_attr(const xml_reader *x, const char *name) {
    if (!x->attrs)
        return NULL;
    size_t nlen = strlen(name);
    const char *p = x->attrs;
    const char *end = x->attrs + x->attrs_len;
    while (p < end) {
        while (p < end && is_ws(*p))
            p++;
        /* attribute name */
        const char *astart = p;
        while (p < end && *p != '=' && !is_ws(*p))
            p++;
        size_t alen = (size_t)(p - astart);
        while (p < end && is_ws(*p))
            p++;
        if (p >= end || *p != '=')
            break; /* malformed */
        p++;
        while (p < end && is_ws(*p))
            p++;
        if (p >= end || (*p != '"' && *p != '\''))
            break;
        char q = *p++;
        const char *vstart = p;
        while (p < end && *p != q)
            p++;
        size_t vlen = (size_t)(p - vstart);
        if (p < end)
            p++; /* closing quote */

        /* match name exactly, or after a namespace prefix */
        int match = alen == nlen && memcmp(astart, name, nlen) == 0;
        if (!match && alen > nlen && astart[alen - nlen - 1] == ':' &&
            memcmp(astart + alen - nlen, name, nlen) == 0)
            match = 1;
        if (match) {
            sb v;
            sb_init(&v);
            xml_decode_into(&v, vstart, vlen);
            return sb_take(&v);
        }
    }
    return NULL;
}
