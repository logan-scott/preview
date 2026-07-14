#include "conv_rtf.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "page.h"
#include "str.h"

#define RTF_MAX_DEPTH 256

typedef struct {
    unsigned bold : 1, italic : 1, underline : 1;
} rtf_fmt;

typedef struct {
    sb *out;
    rtf_fmt fmt;                 /* desired formatting */
    unsigned ob : 1, oi : 1, ou : 1; /* currently open <b>/<i>/<u> */
    int in_para;                 /* a <p> is open */
    rtf_fmt stack[RTF_MAX_DEPTH]; /* saved by { , restored by } */
    int depth;
    int skip_to;                 /* skip text until depth < skip_to (-1 = off) */
    int uc;                      /* \ucN: fallback chars to skip after \u */
} rtf_state;

static void append_utf8(sb *out, long cp) {
    char b[4];
    if (cp < 0)
        return;
    if (cp < 0x80) {
        b[0] = (char)cp;
        sb_append_html(out, b, 1);
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
}

/* Windows-1252 high range (0x80-0x9F) → Unicode, for \'hh bytes. */
static const unsigned short cp1252_hi[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

static void put_byte(rtf_state *s, unsigned char b) {
    if (b >= 0x80 && b <= 0x9F)
        append_utf8(s->out, cp1252_hi[b - 0x80]);
    else
        append_utf8(s->out, b); /* latin-1 maps 1:1 to Unicode */
}

/* Bring the open <b>/<i>/<u> tags in line with the desired formatting. */
static void sync(rtf_state *s) {
    if (!s->in_para) {
        sb_append(s->out, "<p>");
        s->in_para = 1;
    }
    if (s->ou && !s->fmt.underline) { sb_append(s->out, "</u>"); s->ou = 0; }
    if (s->oi && !s->fmt.italic)    { sb_append(s->out, "</i>"); s->oi = 0; }
    if (s->ob && !s->fmt.bold)      { sb_append(s->out, "</b>"); s->ob = 0; }
    if (s->fmt.bold && !s->ob)      { sb_append(s->out, "<b>"); s->ob = 1; }
    if (s->fmt.italic && !s->oi)    { sb_append(s->out, "<i>"); s->oi = 1; }
    if (s->fmt.underline && !s->ou) { sb_append(s->out, "<u>"); s->ou = 1; }
}

static void close_tags(rtf_state *s) {
    if (s->ou) { sb_append(s->out, "</u>"); s->ou = 0; }
    if (s->oi) { sb_append(s->out, "</i>"); s->oi = 0; }
    if (s->ob) { sb_append(s->out, "</b>"); s->ob = 0; }
}

static void end_para(rtf_state *s) {
    if (s->in_para) {
        close_tags(s);
        sb_append(s->out, "</p>");
        s->in_para = 0;
    }
}

static void text_char(rtf_state *s, char c) {
    if (s->skip_to >= 0)
        return;
    sync(s);
    sb_append_html(s->out, &c, 1);
}

/* Control words that begin an ignorable destination group. */
static int is_skip_dest(const char *w) {
    static const char *dests[] = {"fonttbl", "colortbl", "stylesheet",
                                  "info",    "pict",     "header",
                                  "footer",  "footnote", "field",
                                  "object",  "themedata", "datastore",
                                  "generator", "listtable", "revtbl"};
    for (size_t i = 0; i < sizeof(dests) / sizeof(dests[0]); i++)
        if (strcmp(w, dests[i]) == 0)
            return 1;
    return 0;
}

static void control(rtf_state *s, const char *w, int has_param, long param) {
    if (s->skip_to >= 0)
        return; /* inside a skipped destination */

    if (strcmp(w, "par") == 0 || strcmp(w, "pard") == 0) {
        if (strcmp(w, "par") == 0) {
            end_para(s);
        }
        return;
    }
    if (strcmp(w, "line") == 0) { sync(s); sb_append(s->out, "<br>"); return; }
    if (strcmp(w, "tab") == 0) {
        sync(s);
        sb_append(s->out, "<span style=\"display:inline-block;width:2em\">"
                          "</span>");
        return;
    }
    if (strcmp(w, "b") == 0)  { s->fmt.bold = !has_param || param != 0; return; }
    if (strcmp(w, "i") == 0)  { s->fmt.italic = !has_param || param != 0; return; }
    if (strcmp(w, "ul") == 0) { s->fmt.underline = !has_param || param != 0; return; }
    if (strcmp(w, "ulnone") == 0) { s->fmt.underline = 0; return; }
    if (strcmp(w, "uc") == 0) { s->uc = has_param ? (int)param : 1; return; }
    if (strcmp(w, "plain") == 0) {
        s->fmt.bold = s->fmt.italic = s->fmt.underline = 0;
        return;
    }
    if (is_skip_dest(w))
        s->skip_to = s->depth; /* skip until this group closes */
    /* all other control words are ignored */
}

char *convert_rtf(const source_file *src) {
    const char *base = path_basename(src->path);
    const char *p = (const char *)src->data;
    const char *end = p + src->len;

    sb out;
    sb_init(&out);
    page_begin(&out, base, 0);
    sb_append(&out, "<div class=\"content\">");

    rtf_state s;
    memset(&s, 0, sizeof(s));
    s.out = &out;
    s.skip_to = -1;
    s.uc = 1;

    while (p < end) {
        char c = *p++;
        if (c == '{') {
            if (s.depth < RTF_MAX_DEPTH)
                s.stack[s.depth] = s.fmt;
            s.depth++;
        } else if (c == '}') {
            s.depth--;
            if (s.depth < 0)
                s.depth = 0;
            if (s.depth < RTF_MAX_DEPTH)
                s.fmt = s.stack[s.depth];
            if (s.skip_to >= 0 && s.depth < s.skip_to)
                s.skip_to = -1;
        } else if (c == '\\') {
            if (p >= end)
                break;
            char n = *p;
            if (isalpha((unsigned char)n)) {
                /* control word */
                char word[40];
                int wl = 0;
                while (p < end && isalpha((unsigned char)*p)) {
                    if (wl < (int)sizeof(word) - 1)
                        word[wl++] = *p;
                    p++;
                }
                word[wl] = '\0';
                int has_param = 0;
                long param = 0, sign = 1;
                if (p < end && (*p == '-' || isdigit((unsigned char)*p))) {
                    has_param = 1;
                    if (*p == '-') { sign = -1; p++; }
                    while (p < end && isdigit((unsigned char)*p))
                        param = param * 10 + (*p++ - '0');
                    param *= sign;
                }
                if (p < end && *p == ' ')
                    p++; /* delimiter space is consumed */
                if (strcmp(word, "u") == 0 && has_param) {
                    if (s.skip_to < 0) {
                        long cp = param < 0 ? param + 65536 : param;
                        sync(&s);
                        append_utf8(&out, cp);
                    }
                    /* skip \uc fallback characters */
                    for (int k = 0; k < s.uc && p < end; k++) {
                        if (*p == '\\' || *p == '{' || *p == '}')
                            break;
                        p++;
                    }
                } else {
                    control(&s, word, has_param, param);
                }
            } else if (n == '\'') {
                p++; /* the quote */
                int hi = 0, lo = 0;
                if (p < end) hi = *p++;
                if (p < end) lo = *p++;
                int v = 0;
                v = (isdigit(hi) ? hi - '0' : (tolower(hi) - 'a' + 10)) * 16 +
                    (isdigit(lo) ? lo - '0' : (tolower(lo) - 'a' + 10));
                if (s.skip_to < 0) { sync(&s); put_byte(&s, (unsigned char)v); }
            } else if (n == '\\' || n == '{' || n == '}') {
                p++;
                text_char(&s, n);
            } else if (n == '~') {
                p++;
                if (s.skip_to < 0) { sync(&s); sb_append(&out, "&nbsp;"); }
            } else if (n == '\n' || n == '\r') {
                p++;
                if (s.skip_to < 0) end_para(&s);
            } else {
                p++; /* other control symbol: ignore */
            }
        } else if (c == '\r' || c == '\n') {
            /* raw line breaks in RTF are not significant */
        } else {
            text_char(&s, c);
        }
    }
    end_para(&s);

    sb_append(&out, "</div>");
    page_end(&out, 0);
    return sb_take(&out);
}
