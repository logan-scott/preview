#include "conv_pptx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz/miniz.h"

#include "page.h"
#include "str.h"
#include "xmlmini.h"

#define PPTX_MAX_SLIDES 200

/* Slide rels: rId → media target (for images) */
typedef struct {
    char *id;
    char *target;
} srel;

typedef struct {
    srel *v;
    size_t n, cap;
} srel_list;

static void srels_parse(srel_list *rl, const char *xml, size_t len) {
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
            char *tg = xml_attr(&x, "Target");
            if (id && tg) {
                if (rl->n == rl->cap) {
                    rl->cap = rl->cap ? rl->cap * 2 : 8;
                    rl->v = realloc(rl->v, rl->cap * sizeof(srel));
                }
                rl->v[rl->n].id = id;
                rl->v[rl->n].target = tg;
                rl->n++;
            } else {
                free(id);
                free(tg);
            }
        }
    }
}

static const char *srels_find(const srel_list *rl, const char *id) {
    for (size_t i = 0; i < rl->n; i++)
        if (strcmp(rl->v[i].id, id) == 0)
            return rl->v[i].target;
    return NULL;
}

static void srels_free(srel_list *rl) {
    for (size_t i = 0; i < rl->n; i++) {
        free(rl->v[i].id);
        free(rl->v[i].target);
    }
    free(rl->v);
}

static const char *pptx_mime(const uint8_t *d, size_t n) {
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (n >= 3 && memcmp(d, "\xff\xd8\xff", 3) == 0)     return "image/jpeg";
    if (n >= 6 && memcmp(d, "GIF8", 4) == 0)             return "image/gif";
    if (n >= 2 && memcmp(d, "BM", 2) == 0)               return "image/bmp";
    return NULL;
}

/* Walk one slide: pull paragraph text out of a:p / a:t and images out of
 * a:blip. The title placeholder becomes the slide heading. */
static void slide_to_html(sb *out, mz_zip_archive *za, const char *xml,
                          size_t len, const srel_list *rels) {
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;

    int in_sp = 0, is_title_sp = 0, in_para = 0, in_at = 0;
    sb para;
    sb_init(&para);
    sb title;
    sb_init(&title);
    sb body;
    sb_init(&body);

    while ((ev = xml_next(&x)) != XML_EOF) {
        if (ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) {
            if (strcmp(x.name, "p:sp") == 0) {
                in_sp = 1;
                is_title_sp = 0;
            } else if (in_sp && strcmp(x.name, "p:ph") == 0) {
                char *t = xml_attr(&x, "type");
                if (t && (strcmp(t, "title") == 0 ||
                          strcmp(t, "ctrTitle") == 0))
                    is_title_sp = 1;
                free(t);
            } else if (strcmp(x.name, "a:p") == 0 &&
                       ev == XML_ELEM_START) {
                in_para = 1;
                para.len = 0;
                para.buf[0] = '\0';
            } else if (strcmp(x.name, "a:t") == 0 &&
                       ev == XML_ELEM_START) {
                in_at = 1;
            } else if (strcmp(x.name, "a:br") == 0) {
                if (in_para)
                    sb_append(&para, " ");
            } else if (strcmp(x.name, "a:blip") == 0) {
                char *rid = xml_attr(&x, "embed");
                const char *tg = rid ? srels_find(rels, rid) : NULL;
                if (tg) {
                    char zpath[512];
                    if (tg[0] == '/')
                        snprintf(zpath, sizeof(zpath), "%s", tg + 1);
                    else if (strncmp(tg, "../", 3) == 0)
                        snprintf(zpath, sizeof(zpath), "ppt/%s", tg + 3);
                    else
                        snprintf(zpath, sizeof(zpath), "ppt/slides/%s", tg);
                    size_t ilen = 0;
                    void *img = mz_zip_reader_extract_file_to_heap(
                        za, zpath, &ilen, 0);
                    if (img) {
                        const char *mime = pptx_mime(img, ilen);
                        if (mime) {
                            sb_append(&body, "<img src=\"data:");
                            sb_append(&body, mime);
                            sb_append(&body, ";base64,");
                            sb_append_base64(&body, img, ilen);
                            sb_append(&body, "\" alt=\"\">");
                        }
                        mz_free(img);
                    }
                }
                free(rid);
            }
        } else if (ev == XML_ELEM_END) {
            if (strcmp(x.name, "p:sp") == 0) {
                in_sp = 0;
                is_title_sp = 0;
            } else if (strcmp(x.name, "a:p") == 0) {
                if (para.len) {
                    if (is_title_sp && !title.len) {
                        sb_append_n(&title, para.buf, para.len);
                    } else {
                        sb_append(&body, "<p>");
                        sb_append(&body, para.buf);
                        sb_append(&body, "</p>");
                    }
                }
                in_para = 0;
            } else if (strcmp(x.name, "a:t") == 0) {
                in_at = 0;
            }
        } else if (ev == XML_TEXT && in_at && in_para) {
            sb dec;
            sb_init(&dec);
            xml_decode_into(&dec, x.text, x.text_len);
            sb_append_html(&para, dec.buf, dec.len);
            sb_free(&dec);
        }
    }

    if (title.len) {
        sb_append(out, "<h2>");
        sb_append(out, title.buf);
        sb_append(out, "</h2>");
    }
    sb_append(out, body.buf);

    sb_free(&para);
    sb_free(&title);
    sb_free(&body);
}

char *convert_pptx(const source_file *src) {
    const char *base = path_basename(src->path);

    mz_zip_archive za;
    memset(&za, 0, sizeof(za));
    if (!mz_zip_reader_init_mem(&za, src->data, src->len, 0))
        return page_error(base, "Not a valid PPTX file",
                          "could not open ZIP container");

    sb out;
    sb_init(&out);
    page_begin(&out, base, 0);
    sb_append(&out,
              "<style>.slide{max-width:820px;margin:24px auto;padding:32px "
              "40px;border:1px solid var(--border);border-radius:8px;"
              "box-shadow:0 2px 8px var(--shadow)}"
              ".slide h2{margin-top:0;border:0}"
              ".slideno{color:var(--muted);font-size:.75em;margin:24px auto "
              "-20px;max-width:820px}</style><div>");

    int emitted = 0;
    for (int i = 1; i <= PPTX_MAX_SLIDES; i++) {
        char spath[128], rpath[160];
        snprintf(spath, sizeof(spath), "ppt/slides/slide%d.xml", i);
        snprintf(rpath, sizeof(rpath),
                 "ppt/slides/_rels/slide%d.xml.rels", i);
        size_t slen = 0, rlen = 0;
        char *sxml =
            mz_zip_reader_extract_file_to_heap(&za, spath, &slen, 0);
        if (!sxml)
            break;
        char *rxml =
            mz_zip_reader_extract_file_to_heap(&za, rpath, &rlen, 0);

        srel_list rels;
        srels_parse(&rels, rxml, rlen);

        sb_appendf(&out, "<div class=\"slideno\">%d</div>"
                         "<div class=\"slide\">", i);
        slide_to_html(&out, &za, sxml, slen, &rels);
        sb_append(&out, "</div>");
        emitted++;

        srels_free(&rels);
        mz_free(sxml);
        if (rxml)
            mz_free(rxml);
    }

    sb_append(&out, "</div>");
    page_end(&out, 0);
    mz_zip_reader_end(&za);

    if (!emitted) {
        sb_free(&out);
        return page_error(base, "Not a valid PPTX file",
                          "no slides found");
    }
    return sb_take(&out);
}
