#include "conv_odf.h"

#include <stdlib.h>
#include <string.h>

#include "miniz/miniz.h"

#include "page.h"
#include "str.h"
#include "xmlmini.h"
#include "ziputil.h"

/* ---- text styles (from <office:automatic-styles>) ------------------------ */

typedef struct {
    char *name;
    unsigned bold : 1, italic : 1, underline : 1, strike : 1;
    char color[8]; /* "RRGGBB" or empty */
} odf_style;

typedef struct {
    odf_style *v;
    size_t n, cap;
} style_tab;

static const odf_style *style_find(const style_tab *t, const char *name) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, name) == 0)
            return &t->v[i];
    return NULL;
}

/* Parse text-property styles keyed by style:name. Only the run-level
 * properties we render are captured. */
static void styles_parse(style_tab *t, const char *xml, size_t len) {
    memset(t, 0, sizeof(*t));
    if (!xml)
        return;
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;
    odf_style *cur = NULL;
    while ((ev = xml_next(&x)) != XML_EOF) {
        if (ev != XML_ELEM_START && ev != XML_ELEM_EMPTY)
            continue;
        if (strcmp(x.name, "style:style") == 0) {
            char *name = xml_attr(&x, "name");
            cur = NULL;
            if (name) {
                if (t->n == t->cap) {
                    t->cap = t->cap ? t->cap * 2 : 32;
                    t->v = realloc(t->v, t->cap * sizeof(odf_style));
                }
                cur = &t->v[t->n++];
                memset(cur, 0, sizeof(*cur));
                cur->name = name;
            }
        } else if (cur && strcmp(x.name, "style:text-properties") == 0) {
            char *w = xml_attr(&x, "font-weight");
            if (w && strcmp(w, "bold") == 0)
                cur->bold = 1;
            free(w);
            char *st = xml_attr(&x, "font-style");
            if (st && strcmp(st, "italic") == 0)
                cur->italic = 1;
            free(st);
            char *u = xml_attr(&x, "text-underline-style");
            if (u && strcmp(u, "none") != 0)
                cur->underline = 1;
            free(u);
            char *ln = xml_attr(&x, "text-line-through-style");
            if (ln && strcmp(ln, "none") != 0)
                cur->strike = 1;
            free(ln);
            char *c = xml_attr(&x, "color");
            if (c && c[0] == '#' && strlen(c) == 7) {
                int ok = 1;
                for (int k = 1; k <= 6; k++) {
                    char d = c[k];
                    if (!((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f') ||
                          (d >= 'A' && d <= 'F')))
                        ok = 0;
                }
                if (ok)
                    snprintf(cur->color, sizeof(cur->color), "%s", c + 1);
            }
            free(c);
        }
    }
}

static void styles_free(style_tab *t) {
    for (size_t i = 0; i < t->n; i++)
        free(t->v[i].name);
    free(t->v);
}

/* ---- document walker ----------------------------------------------------- */

typedef struct {
    zcap *zip;
    const style_tab *styles;
    sb *out;
    int list_depth;
    unsigned char list_is_ol[16];
    int in_span;      /* nesting of styled spans, for closing tags */
    int skip_depth;   /* inside elements whose text we ignore */
    int heading;      /* current heading level (headings do not nest) */
} odf_ctx;

static void open_span(odf_ctx *c, const odf_style *s) {
    if (!s) {
        sb_append(c->out, "<span>");
        return;
    }
    sb_append(c->out, "<span style=\"");
    if (s->bold) sb_append(c->out, "font-weight:bold;");
    if (s->italic) sb_append(c->out, "font-style:italic;");
    if (s->underline && s->strike)
        sb_append(c->out, "text-decoration:underline line-through;");
    else if (s->underline)
        sb_append(c->out, "text-decoration:underline;");
    else if (s->strike)
        sb_append(c->out, "text-decoration:line-through;");
    if (s->color[0])
        sb_appendf(c->out, "color:#%s;", s->color);
    sb_append(c->out, "\">");
}

static void emit_image(odf_ctx *c, const char *href) {
    if (!href)
        return;
    char zpath[512];
    if (strncmp(href, "./", 2) == 0)
        href += 2;
    snprintf(zpath, sizeof(zpath), "%s", href);
    size_t len = 0;
    void *img = zcap_extract(c->zip, zpath, &len);
    if (!img)
        return;
    const char *mime = "application/octet-stream";
    const uint8_t *d = img;
    if (len >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) mime = "image/png";
    else if (len >= 3 && memcmp(d, "\xff\xd8\xff", 3) == 0) mime = "image/jpeg";
    else if (len >= 6 && memcmp(d, "GIF8", 4) == 0) mime = "image/gif";
    else {
        const char *dot = strrchr(href, '.');
        if (dot && (strcmp(dot, ".svg") == 0)) mime = "image/svg+xml";
    }
    sb_appendf(c->out, "<img src=\"data:%s;base64,", mime);
    sb_append_base64(c->out, img, len);
    sb_append(c->out, "\" alt=\"\">");
    mz_free(img);
}

static void odf_walk(odf_ctx *c, const char *xml, size_t len) {
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;
    while ((ev = xml_next(&x)) != XML_EOF) {
        const char *n = x.name;
        if (ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) {
            int empty = ev == XML_ELEM_EMPTY;
            /* skip note bodies, tracked-change metadata, etc. */
            if (c->skip_depth) {
                if (!empty)
                    c->skip_depth++;
                continue;
            }
            if (strcmp(n, "text:h") == 0) {
                char *lvl = xml_attr(&x, "outline-level");
                int l = lvl ? atoi(lvl) : 1;
                free(lvl);
                if (l < 1) l = 1;
                if (l > 6) l = 6;
                c->heading = l;
                sb_appendf(c->out, "<h%d>", l);
            } else if (strcmp(n, "text:p") == 0) {
                sb_append(c->out, "<p>");
            } else if (strcmp(n, "text:span") == 0 && !empty) {
                char *sn = xml_attr(&x, "style-name");
                open_span(c, style_find(c->styles, sn));
                free(sn);
                c->in_span++;
            } else if (strcmp(n, "text:line-break") == 0) {
                sb_append(c->out, "<br>");
            } else if (strcmp(n, "text:tab") == 0) {
                sb_append(c->out, "<span style=\"display:inline-block;"
                                  "width:2em\"></span>");
            } else if (strcmp(n, "text:list") == 0) {
                if (c->list_depth < 16) {
                    c->list_is_ol[c->list_depth] = 0; /* bullets by default */
                    c->list_depth++;
                }
                sb_append(c->out, "<ul>");
            } else if (strcmp(n, "text:list-item") == 0) {
                sb_append(c->out, "<li>");
            } else if (strcmp(n, "table:table") == 0) {
                sb_append(c->out, "<table>");
            } else if (strcmp(n, "table:table-row") == 0) {
                sb_append(c->out, "<tr>");
            } else if (strcmp(n, "table:table-cell") == 0) {
                char *span = xml_attr(&x, "number-columns-spanned");
                sb_append(c->out, "<td");
                if (span && atoi(span) > 1)
                    sb_appendf(c->out, " colspan=\"%d\"", atoi(span));
                free(span);
                sb_append(c->out, ">");
            } else if (strcmp(n, "draw:image") == 0) {
                char *href = xml_attr(&x, "href");
                emit_image(c, href);
                free(href);
            } else if (strcmp(n, "text:note-body") == 0 ||
                       strcmp(n, "office:annotation") == 0) {
                if (!empty)
                    c->skip_depth = 1;
            }
        } else if (ev == XML_ELEM_END) {
            if (c->skip_depth) {
                c->skip_depth--;
                continue;
            }
            if (strcmp(n, "text:h") == 0) {
                sb_appendf(c->out, "</h%d>", c->heading ? c->heading : 1);
                c->heading = 0;
            } else if (strcmp(n, "text:p") == 0) {
                sb_append(c->out, "</p>");
            } else if (strcmp(n, "text:span") == 0) {
                if (c->in_span > 0) {
                    sb_append(c->out, "</span>");
                    c->in_span--;
                }
            } else if (strcmp(n, "text:list") == 0) {
                if (c->list_depth > 0)
                    c->list_depth--;
                sb_append(c->out, "</ul>");
            } else if (strcmp(n, "text:list-item") == 0) {
                sb_append(c->out, "</li>");
            } else if (strcmp(n, "table:table") == 0) {
                sb_append(c->out, "</table>");
            } else if (strcmp(n, "table:table-row") == 0) {
                sb_append(c->out, "</tr>");
            } else if (strcmp(n, "table:table-cell") == 0) {
                sb_append(c->out, "</td>");
            }
        } else if (ev == XML_TEXT && !c->skip_depth) {
            sb dec;
            sb_init(&dec);
            xml_decode_into(&dec, x.text, x.text_len);
            sb_append_html(c->out, dec.buf, dec.len);
            sb_free(&dec);
        }
    }
}

char *convert_odf(const source_file *src) {
    const char *base = path_basename(src->path);
    mz_zip_archive za;
    memset(&za, 0, sizeof(za));
    if (!mz_zip_reader_init_mem(&za, src->data, src->len, 0))
        return page_error(base, "Not a valid OpenDocument file",
                          "could not open ZIP container");
    zcap zc;
    zcap_init(&zc, &za);

    size_t clen = 0;
    char *content = zcap_extract(&zc, "content.xml", &clen);
    if (!content) {
        mz_zip_reader_end(&za);
        return page_error(base, "Not a valid OpenDocument file",
                          "content.xml missing");
    }

    /* Styles live both in content.xml's automatic-styles and styles.xml. */
    style_tab styles;
    styles_parse(&styles, content, clen);
    size_t slen = 0;
    char *stylesxml = zcap_extract(&zc, "styles.xml", &slen);
    if (stylesxml) {
        style_tab extra;
        styles_parse(&extra, stylesxml, slen);
        /* append extra into styles */
        for (size_t i = 0; i < extra.n; i++) {
            if (styles.n == styles.cap) {
                styles.cap = styles.cap ? styles.cap * 2 : 32;
                styles.v = realloc(styles.v, styles.cap * sizeof(odf_style));
            }
            styles.v[styles.n++] = extra.v[i]; /* moves name ownership */
        }
        free(extra.v);
    }

    sb out;
    sb_init(&out);
    page_begin(&out, base, 0);
    sb_append(&out, "<div class=\"content\">");

    odf_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.zip = &zc;
    ctx.styles = &styles;
    ctx.out = &out;
    odf_walk(&ctx, content, clen);

    sb_append(&out, "</div>");
    page_end(&out, 0);

    styles_free(&styles);
    mz_free(content);
    if (stylesxml)
        mz_free(stylesxml);
    mz_zip_reader_end(&za);
    return sb_take(&out);
}
