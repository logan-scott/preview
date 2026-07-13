#include "conv_docx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "miniz/miniz.h"

#include "page.h"
#include "str.h"
#include "xmlmini.h"
#include "ziputil.h"

/* ---- relationships (word/_rels/document.xml.rels) ---------------------- */

typedef struct {
    char *id;
    char *target;
} rel;

typedef struct {
    rel *v;
    size_t n, cap;
} rel_list;

static void rels_parse(rel_list *rl, const char *xml, size_t len) {
    rl->v = NULL;
    rl->n = rl->cap = 0;
    if (!xml)
        return;
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;
    while ((ev = xml_next(&x)) != XML_EOF) {
        if ((ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) &&
            strstr(x.name, "Relationship") &&
            strcmp(x.name, "Relationships") != 0) {
            char *id = xml_attr(&x, "Id");
            char *target = xml_attr(&x, "Target");
            if (id && target) {
                if (rl->n == rl->cap) {
                    rl->cap = rl->cap ? rl->cap * 2 : 16;
                    rl->v = realloc(rl->v, rl->cap * sizeof(rel));
                }
                rl->v[rl->n].id = id;
                rl->v[rl->n].target = target;
                rl->n++;
            } else {
                free(id);
                free(target);
            }
        }
    }
}

static const char *rels_find(const rel_list *rl, const char *id) {
    for (size_t i = 0; i < rl->n; i++)
        if (strcmp(rl->v[i].id, id) == 0)
            return rl->v[i].target;
    return NULL;
}

static void rels_free(rel_list *rl) {
    for (size_t i = 0; i < rl->n; i++) {
        free(rl->v[i].id);
        free(rl->v[i].target);
    }
    free(rl->v);
}

/* ---- numbering (word/numbering.xml) ------------------------------------ */

#define MAX_NUMS 256
#define MAX_LVLS 9

typedef struct {
    int num_id[MAX_NUMS];
    int num_abs[MAX_NUMS];
    int nnum;
    int abs_id[MAX_NUMS];
    unsigned char abs_bullet[MAX_NUMS][MAX_LVLS]; /* 1 = bullet, 0 = numbered */
    int nabs;
} numbering;

static void numbering_parse(numbering *nb, const char *xml, size_t len) {
    memset(nb, 0, sizeof(*nb));
    if (!xml)
        return;
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;
    int cur_abs = -1, cur_lvl = -1, cur_num = -1;
    while ((ev = xml_next(&x)) != XML_EOF) {
        if (ev != XML_ELEM_START && ev != XML_ELEM_EMPTY)
            continue;
        if (strcmp(x.name, "w:abstractNum") == 0) {
            char *v = xml_attr(&x, "abstractNumId");
            cur_abs = -1;
            if (v && nb->nabs < MAX_NUMS) {
                cur_abs = nb->nabs++;
                nb->abs_id[cur_abs] = atoi(v);
            }
            free(v);
        } else if (strcmp(x.name, "w:lvl") == 0 && cur_abs >= 0) {
            char *v = xml_attr(&x, "ilvl");
            cur_lvl = v ? atoi(v) : -1;
            free(v);
        } else if (strcmp(x.name, "w:numFmt") == 0 && cur_abs >= 0 &&
                   cur_lvl >= 0 && cur_lvl < MAX_LVLS) {
            char *v = xml_attr(&x, "val");
            if (v)
                nb->abs_bullet[cur_abs][cur_lvl] =
                    strcmp(v, "bullet") == 0 ? 1 : 0;
            free(v);
        } else if (strcmp(x.name, "w:num") == 0) {
            char *v = xml_attr(&x, "numId");
            cur_num = -1;
            if (v && nb->nnum < MAX_NUMS) {
                cur_num = nb->nnum++;
                nb->num_id[cur_num] = atoi(v);
                nb->num_abs[cur_num] = -1;
            }
            free(v);
        } else if (strcmp(x.name, "w:abstractNumId") == 0 && cur_num >= 0) {
            char *v = xml_attr(&x, "val");
            if (v)
                nb->num_abs[cur_num] = atoi(v);
            free(v);
        }
    }
}

/* 1 = bullet list, 0 = numbered */
static int numbering_is_bullet(const numbering *nb, int num_id, int ilvl) {
    if (ilvl < 0 || ilvl >= MAX_LVLS)
        ilvl = 0;
    for (int i = 0; i < nb->nnum; i++) {
        if (nb->num_id[i] == num_id) {
            int abs = nb->num_abs[i];
            for (int j = 0; j < nb->nabs; j++)
                if (nb->abs_id[j] == abs)
                    return nb->abs_bullet[j][ilvl];
            return 1;
        }
    }
    return 1; /* unknown: bullets look less wrong than numbers */
}

/* ---- document walker ----------------------------------------------------- */

typedef struct {
    /* run formatting */
    int bold, italic, underline, strike, vert; /* vert: 1 sup, -1 sub */
    /* paragraph properties */
    int heading;    /* 0 = none, 1-6 */
    int list_numid; /* -1 = not a list item */
    int list_ilvl;
    char jc[16]; /* justification */
    /* list nesting stack: type of each open list level */
    unsigned char list_is_ul[16];
    int list_depth;
    /* current image */
    long img_cx, img_cy;
    char img_rid[64];
    int in_drawing;
    /* element context flags */
    int in_ppr, in_rpr, in_tcpr, in_del, in_wt, in_instr;
} walk_state;

typedef struct {
    zcap *zip;
    const rel_list *rels;
    const numbering *nums;
    sb *out;  /* final document */
    sb *para; /* current paragraph body */
    walk_state st;
} docx_ctx;

static const char *docx_image_mime(const uint8_t *d, size_t n,
                                   const char *target) {
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (n >= 3 && memcmp(d, "\xff\xd8\xff", 3) == 0)     return "image/jpeg";
    if (n >= 6 && memcmp(d, "GIF8", 4) == 0)             return "image/gif";
    if (n >= 2 && memcmp(d, "BM", 2) == 0)               return "image/bmp";
    const char *dot = strrchr(target, '.');
    if (dot && strcmp(dot, ".svg") == 0)                 return "image/svg+xml";
    return NULL; /* emf/wmf/tiff/...: browsers can't reliably show these */
}

static void emit_image(docx_ctx *c) {
    if (!c->st.img_rid[0])
        return;
    const char *target = rels_find(c->rels, c->st.img_rid);
    if (!target)
        return;
    char zpath[512];
    if (target[0] == '/')
        snprintf(zpath, sizeof(zpath), "%s", target + 1);
    else
        snprintf(zpath, sizeof(zpath), "word/%s", target);

    size_t imglen = 0;
    void *img = zcap_extract(c->zip, zpath, &imglen);
    if (!img) {
        sb_append(c->para, "<span class=\"missing-img\">[image]</span>");
        return;
    }
    const char *mime = docx_image_mime(img, imglen, target);
    if (!mime) {
        sb_append(c->para, "<span class=\"missing-img\">[unsupported "
                           "image format]</span>");
        mz_free(img);
        return;
    }
    sb_append(c->para, "<img src=\"data:");
    sb_append(c->para, mime);
    sb_append(c->para, ";base64,");
    sb_append_base64(c->para, img, imglen);
    mz_free(img);
    sb_append(c->para, "\" alt=\"\"");
    if (c->st.img_cx > 0) {
        /* EMU → CSS px (9525 EMU per px) */
        long wpx = c->st.img_cx / 9525;
        if (wpx > 0 && wpx < 4000)
            sb_appendf(c->para, " style=\"width:%ldpx\"", wpx);
    }
    sb_append(c->para, ">");
}

static void close_lists_to(docx_ctx *c, int depth) {
    while (c->st.list_depth > depth) {
        c->st.list_depth--;
        sb_append(c->out,
                  c->st.list_is_ul[c->st.list_depth] ? "</ul>" : "</ol>");
    }
}

static void flush_paragraph(docx_ctx *c) {
    walk_state *st = &c->st;
    if (st->list_numid >= 0) {
        int want = st->list_ilvl + 1;
        if (want > 16)
            want = 16;
        int is_ul = numbering_is_bullet(c->nums, st->list_numid,
                                        st->list_ilvl);
        close_lists_to(c, want); /* pop deeper levels first */
        /* same depth but different list type: close and reopen */
        if (st->list_depth == want &&
            st->list_is_ul[want - 1] != (unsigned char)is_ul)
            close_lists_to(c, want - 1);
        while (st->list_depth < want) {
            st->list_is_ul[st->list_depth] = (unsigned char)is_ul;
            st->list_depth++;
            sb_append(c->out, is_ul ? "<ul>" : "<ol>");
        }
        sb_append(c->out, "<li>");
        sb_append(c->out, c->para->buf);
        sb_append(c->out, "</li>");
        return;
    }

    close_lists_to(c, 0);
    const char *tag = "p";
    char tagbuf[4];
    if (st->heading > 0) {
        snprintf(tagbuf, sizeof(tagbuf), "h%d",
                 st->heading > 6 ? 6 : st->heading);
        tag = tagbuf;
    }
    sb_appendf(c->out, "<%s", tag);
    if (st->jc[0] && (strcmp(st->jc, "center") == 0 ||
                      strcmp(st->jc, "right") == 0 ||
                      strcmp(st->jc, "both") == 0))
        sb_appendf(c->out, " style=\"text-align:%s\"",
                   strcmp(st->jc, "both") == 0 ? "justify" : st->jc);
    sb_append(c->out, ">");
    sb_append(c->out, c->para->buf);
    sb_appendf(c->out, "</%s>", tag);
}

/* w:val on toggle properties: absent means on */
static int prop_is_on(xml_reader *x) {
    char *v = xml_attr(x, "val");
    int on = 1;
    if (v && (strcmp(v, "false") == 0 || strcmp(v, "0") == 0 ||
              strcmp(v, "none") == 0))
        on = 0;
    free(v);
    return on;
}

static void docx_walk(docx_ctx *c, const char *xml, size_t len) {
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;
    walk_state *st = &c->st;

    while ((ev = xml_next(&x)) != XML_EOF) {
        const char *n = x.name;
        if (ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) {
            int empty = ev == XML_ELEM_EMPTY;

            /* a "<td" is pending its '>' until we know whether a tcPr
             * carries a colspan; any other element means no tcPr came */
            if (st->in_tcpr == 2 && strcmp(n, "w:tcPr") != 0) {
                sb_append(c->out, ">");
                st->in_tcpr = 0;
            }

            if (strcmp(n, "w:p") == 0) {
                c->para->len = 0;
                c->para->buf[0] = '\0';
                st->heading = 0;
                st->list_numid = -1;
                st->list_ilvl = 0;
                st->jc[0] = '\0';
            } else if (strcmp(n, "w:pPr") == 0 && !empty) {
                st->in_ppr = 1;
            } else if (strcmp(n, "w:rPr") == 0 && !empty) {
                st->in_rpr = 1;
            } else if (strcmp(n, "w:del") == 0 && !empty) {
                st->in_del = 1; /* tracked deletion: skip content */
            } else if (st->in_ppr && strcmp(n, "w:pStyle") == 0) {
                char *v = xml_attr(&x, "val");
                if (v) {
                    if (strncasecmp(v, "Heading", 7) == 0 && v[7])
                        st->heading = atoi(v + 7);
                    else if (strcasecmp(v, "Title") == 0)
                        st->heading = 1;
                    free(v);
                }
            } else if (st->in_ppr && strcmp(n, "w:jc") == 0) {
                char *v = xml_attr(&x, "val");
                if (v) {
                    snprintf(st->jc, sizeof(st->jc), "%s", v);
                    free(v);
                }
            } else if (st->in_ppr && strcmp(n, "w:numId") == 0) {
                char *v = xml_attr(&x, "val");
                if (v) {
                    st->list_numid = atoi(v);
                    free(v);
                }
            } else if (st->in_ppr && strcmp(n, "w:ilvl") == 0) {
                char *v = xml_attr(&x, "val");
                if (v) {
                    st->list_ilvl = atoi(v);
                    free(v);
                }
            } else if (st->in_rpr) {
                if (strcmp(n, "w:b") == 0)
                    st->bold = prop_is_on(&x);
                else if (strcmp(n, "w:i") == 0)
                    st->italic = prop_is_on(&x);
                else if (strcmp(n, "w:u") == 0)
                    st->underline = prop_is_on(&x);
                else if (strcmp(n, "w:strike") == 0)
                    st->strike = prop_is_on(&x);
                else if (strcmp(n, "w:vertAlign") == 0) {
                    char *v = xml_attr(&x, "val");
                    st->vert = 0;
                    if (v) {
                        if (strcmp(v, "superscript") == 0)
                            st->vert = 1;
                        else if (strcmp(v, "subscript") == 0)
                            st->vert = -1;
                        free(v);
                    }
                }
            } else if (strcmp(n, "w:r") == 0 && !empty) {
                st->bold = st->italic = st->underline = st->strike = 0;
                st->vert = 0;
            } else if (strcmp(n, "w:t") == 0) {
                st->in_wt = !empty;
            } else if (strcmp(n, "w:instrText") == 0 && !empty) {
                st->in_instr = 1; /* field codes: not display text */
            } else if (strcmp(n, "w:br") == 0 ||
                       strcmp(n, "w:cr") == 0) {
                if (!st->in_del)
                    sb_append(c->para, "<br>");
            } else if (strcmp(n, "w:tab") == 0 && !st->in_ppr &&
                       !st->in_rpr) {
                if (!st->in_del)
                    sb_append(c->para,
                              "<span style=\"display:inline-block;"
                              "width:2em\"></span>");
            } else if (strcmp(n, "w:hyperlink") == 0 && !empty) {
                char *id = xml_attr(&x, "id");
                char *anchor = xml_attr(&x, "anchor");
                const char *href = id ? rels_find(c->rels, id) : NULL;
                sb_append(c->para, "<a href=\"");
                if (href && safe_link_scheme(href)) {
                    sb_append_html(c->para, href, strlen(href));
                } else if (!href && anchor) {
                    sb_append(c->para, "#");
                    sb_append_html(c->para, anchor, strlen(anchor));
                } else {
                    /* dangerous or missing target: keep the link text,
                     * neuter the destination */
                    sb_append(c->para, "#blocked");
                }
                sb_append(c->para, "\">");
                free(id);
                free(anchor);
            } else if (strcmp(n, "w:drawing") == 0 && !empty) {
                st->in_drawing = 1;
                st->img_rid[0] = '\0';
                st->img_cx = st->img_cy = 0;
            } else if (st->in_drawing && strcmp(n, "wp:extent") == 0) {
                char *cx = xml_attr(&x, "cx");
                char *cy = xml_attr(&x, "cy");
                if (cx)
                    st->img_cx = atol(cx);
                if (cy)
                    st->img_cy = atol(cy);
                free(cx);
                free(cy);
            } else if (st->in_drawing && strcmp(n, "a:blip") == 0) {
                char *rid = xml_attr(&x, "embed");
                if (rid) {
                    snprintf(st->img_rid, sizeof(st->img_rid), "%s", rid);
                    free(rid);
                }
            } else if (strcmp(n, "w:tbl") == 0 && !empty) {
                close_lists_to(c, 0);
                sb_append(c->out, "<table class=\"docxtbl\">");
            } else if (strcmp(n, "w:tr") == 0 && !empty) {
                sb_append(c->out, "<tr>");
            } else if (strcmp(n, "w:tc") == 0 && !empty) {
                /* colspan discovered inside tcPr; use a placeholder we
                 * can't easily backpatch, so scan ahead is avoided by
                 * emitting the <td> lazily at first content. Simpler:
                 * emit now, add colspan via tcPr as attribute on a
                 * marker — instead we buffer: tcPr comes first, before
                 * any paragraph, so remember and emit at tcPr end. */
                sb_append(c->out, "<td");
                st->in_tcpr = 2; /* waiting for tcPr (or content) */
            } else if (strcmp(n, "w:tcPr") == 0 && !empty) {
                st->in_tcpr = 1;
            } else if (st->in_tcpr == 1 && strcmp(n, "w:gridSpan") == 0) {
                char *v = xml_attr(&x, "val");
                if (v) {
                    int span = atoi(v);
                    if (span > 1)
                        sb_appendf(c->out, " colspan=\"%d\"", span);
                    free(v);
                }
            }
        } else if (ev == XML_ELEM_END) {
            if (strcmp(n, "w:p") == 0) {
                flush_paragraph(c);
            } else if (strcmp(n, "w:t") == 0) {
                st->in_wt = 0;
            } else if (strcmp(n, "w:instrText") == 0) {
                st->in_instr = 0;
            } else if (strcmp(n, "w:pPr") == 0) {
                st->in_ppr = 0;
            } else if (strcmp(n, "w:rPr") == 0) {
                st->in_rpr = 0;
            } else if (strcmp(n, "w:del") == 0) {
                st->in_del = 0;
            } else if (strcmp(n, "w:hyperlink") == 0) {
                sb_append(c->para, "</a>");
            } else if (strcmp(n, "w:drawing") == 0) {
                st->in_drawing = 0;
                if (!st->in_del)
                    emit_image(c);
            } else if (strcmp(n, "w:tcPr") == 0) {
                if (st->in_tcpr == 1) {
                    sb_append(c->out, ">");
                    st->in_tcpr = 0;
                }
            } else if (strcmp(n, "w:tc") == 0) {
                if (st->in_tcpr == 2) { /* completely empty cell */
                    sb_append(c->out, ">");
                    st->in_tcpr = 0;
                }
                close_lists_to(c, 0);
                sb_append(c->out, "</td>");
            } else if (strcmp(n, "w:tr") == 0) {
                sb_append(c->out, "</tr>");
            } else if (strcmp(n, "w:tbl") == 0) {
                sb_append(c->out, "</table>");
            } else if (strcmp(n, "w:body") == 0) {
                close_lists_to(c, 0);
            }
        } else if (ev == XML_TEXT) {
            /* Only text inside w:t is document content; everything else
             * is pretty-printing whitespace or field machinery. */
            if (!st->in_wt || st->in_del || st->in_instr)
                continue;

            if (st->bold) sb_append(c->para, "<b>");
            if (st->italic) sb_append(c->para, "<i>");
            if (st->underline) sb_append(c->para, "<u>");
            if (st->strike) sb_append(c->para, "<s>");
            if (st->vert == 1) sb_append(c->para, "<sup>");
            if (st->vert == -1) sb_append(c->para, "<sub>");

            /* decode XML entities, then escape for HTML */
            sb dec;
            sb_init(&dec);
            xml_decode_into(&dec, x.text, x.text_len);
            sb_append_html(c->para, dec.buf, dec.len);
            sb_free(&dec);

            if (st->vert == -1) sb_append(c->para, "</sub>");
            if (st->vert == 1) sb_append(c->para, "</sup>");
            if (st->strike) sb_append(c->para, "</s>");
            if (st->underline) sb_append(c->para, "</u>");
            if (st->italic) sb_append(c->para, "</i>");
            if (st->bold) sb_append(c->para, "</b>");
        }
    }
    close_lists_to(c, 0);
}

/* ---- entry point ----------------------------------------------------------- */

char *convert_docx(const source_file *src) {
    const char *base = path_basename(src->path);

    mz_zip_archive za;
    memset(&za, 0, sizeof(za));
    if (!mz_zip_reader_init_mem(&za, src->data, src->len, 0))
        return page_error(base, "Not a valid DOCX file",
                          "could not open ZIP container");
    zcap zc;
    zcap_init(&zc, &za);

    size_t doclen = 0, relslen = 0, numslen = 0;
    char *docxml = zcap_extract(&zc, "word/document.xml", &doclen);
    if (!docxml) {
        mz_zip_reader_end(&za);
        return page_error(base, "Not a valid DOCX file",
                          "word/document.xml missing or oversized");
    }
    char *relsxml =
        zcap_extract(&zc, "word/_rels/document.xml.rels", &relslen);
    char *numsxml = zcap_extract(&zc, "word/numbering.xml", &numslen);

    rel_list rels;
    rels_parse(&rels, relsxml, relslen);
    numbering nums;
    numbering_parse(&nums, numsxml, numslen);

    sb out, para;
    sb_init(&out);
    sb_init(&para);
    page_begin(&out, base, 0);
    sb_append(&out,
              "<style>.docxtbl{display:table}"
              ".missing-img{color:var(--muted);border:1px dashed "
              "var(--border);padding:2px 8px;border-radius:4px}"
              "</style><div class=\"content\">");

    docx_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.zip = &zc;
    ctx.rels = &rels;
    ctx.nums = &nums;
    ctx.out = &out;
    ctx.para = &para;
    ctx.st.list_numid = -1;

    docx_walk(&ctx, docxml, doclen);

    sb_append(&out, "</div>");
    page_end(&out, 0);

    sb_free(&para);
    rels_free(&rels);
    mz_free(docxml);
    if (relsxml)
        mz_free(relsxml);
    if (numsxml)
        mz_free(numsxml);
    mz_zip_reader_end(&za);
    return sb_take(&out);
}
