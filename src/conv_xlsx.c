#include "conv_xlsx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz/miniz.h"

#include "page.h"
#include "str.h"
#include "xmlmini.h"
#include "ziputil.h"

#define XLSX_MAX_SHEETS 16
#define XLSX_MAX_ROWS 5000
#define XLSX_MAX_COLS 256

/* ---- shared strings ----------------------------------------------------- */

typedef struct {
    char **v;
    size_t n, cap;
} strtab;

static void strtab_parse(strtab *st, const char *xml, size_t len) {
    st->v = NULL;
    st->n = st->cap = 0;
    if (!xml)
        return;
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;
    int in_si = 0, in_t = 0, in_rph = 0;
    sb cur;
    sb_init(&cur);
    while ((ev = xml_next(&x)) != XML_EOF) {
        if (ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) {
            if (strcmp(x.name, "si") == 0) {
                in_si = 1;
                cur.len = 0;
                cur.buf[0] = '\0';
            } else if (strcmp(x.name, "rPh") == 0) {
                in_rph = 1; /* phonetic ruby text: skip */
            } else if (strcmp(x.name, "t") == 0 && ev == XML_ELEM_START) {
                in_t = 1;
            }
        } else if (ev == XML_ELEM_END) {
            if (strcmp(x.name, "si") == 0) {
                if (st->n == st->cap) {
                    st->cap = st->cap ? st->cap * 2 : 64;
                    st->v = realloc(st->v, st->cap * sizeof(char *));
                }
                char *s = malloc(cur.len + 1);
                memcpy(s, cur.buf, cur.len + 1);
                st->v[st->n++] = s;
                in_si = 0;
            } else if (strcmp(x.name, "rPh") == 0) {
                in_rph = 0;
            } else if (strcmp(x.name, "t") == 0) {
                in_t = 0;
            }
        } else if (ev == XML_TEXT && in_si && in_t && !in_rph) {
            xml_decode_into(&cur, x.text, x.text_len);
        }
    }
    sb_free(&cur);
}

static void strtab_free(strtab *st) {
    for (size_t i = 0; i < st->n; i++)
        free(st->v[i]);
    free(st->v);
}

/* ---- cell reference → column index -------------------------------------- */

static int col_of_ref(const char *ref) {
    int col = 0, letters = 0;
    /* real refs are at most 3 letters ("XFD"); more means a malformed or
     * hostile file, and unbounded accumulation would overflow */
    for (; *ref >= 'A' && *ref <= 'Z' && letters < 4; ref++, letters++)
        col = col * 26 + (*ref - 'A' + 1);
    return letters ? col - 1 : -1; /* 0-based */
}

/* ---- one worksheet → <table> --------------------------------------------- */

static void sheet_to_table(sb *out, const char *xml, size_t len,
                           const strtab *strs) {
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;

    sb_append(out, "<table>");
    int rows = 0, truncated = 0;
    int in_row = 0, in_v = 0, in_is = 0, in_t = 0;
    int cur_col = 0, next_col = 0;
    char cell_type[16] = "";
    sb val;
    sb_init(&val);

    while ((ev = xml_next(&x)) != XML_EOF) {
        if (ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) {
            if (strcmp(x.name, "row") == 0) {
                if (rows >= XLSX_MAX_ROWS) {
                    truncated = 1;
                    break;
                }
                sb_append(out, "<tr>");
                in_row = 1;
                next_col = 0;
                rows++;
            } else if (in_row && strcmp(x.name, "c") == 0) {
                char *r = xml_attr(&x, "r");
                char *t = xml_attr(&x, "t");
                cur_col = r ? col_of_ref(r) : next_col;
                if (cur_col < 0 || cur_col >= XLSX_MAX_COLS)
                    cur_col = next_col;
                snprintf(cell_type, sizeof(cell_type), "%s", t ? t : "");
                free(r);
                free(t);
                /* fill gaps left by omitted empty cells */
                for (; next_col < cur_col && next_col < XLSX_MAX_COLS;
                     next_col++)
                    sb_append(out, "<td></td>");
                val.len = 0;
                val.buf[0] = '\0';
                if (ev == XML_ELEM_EMPTY) { /* empty cell present */
                    sb_append(out, "<td></td>");
                    next_col++;
                }
            } else if (in_row && strcmp(x.name, "v") == 0 &&
                       ev == XML_ELEM_START) {
                in_v = 1;
            } else if (in_row && strcmp(x.name, "is") == 0 &&
                       ev == XML_ELEM_START) {
                in_is = 1;
            } else if (in_is && strcmp(x.name, "t") == 0 &&
                       ev == XML_ELEM_START) {
                in_t = 1;
            }
        } else if (ev == XML_ELEM_END) {
            if (strcmp(x.name, "row") == 0) {
                sb_append(out, "</tr>");
                in_row = 0;
            } else if (strcmp(x.name, "c") == 0) {
                sb_append(out, "<td>");
                if (strcmp(cell_type, "s") == 0) { /* shared string */
                    size_t idx = (size_t)atol(val.buf);
                    if (idx < strs->n)
                        sb_append_html(out, strs->v[idx],
                                       strlen(strs->v[idx]));
                } else if (strcmp(cell_type, "b") == 0) {
                    sb_append(out, val.buf[0] == '1' ? "TRUE" : "FALSE");
                } else {
                    sb_append_html(out, val.buf, val.len);
                }
                sb_append(out, "</td>");
                next_col = cur_col + 1;
            } else if (strcmp(x.name, "v") == 0) {
                in_v = 0;
            } else if (strcmp(x.name, "is") == 0) {
                in_is = 0;
            } else if (strcmp(x.name, "t") == 0) {
                in_t = 0;
            }
        } else if (ev == XML_TEXT && (in_v || (in_is && in_t))) {
            xml_decode_into(&val, x.text, x.text_len);
        }
    }
    sb_free(&val);
    sb_append(out, "</table>");
    if (truncated)
        sb_appendf(out, "<p class=\"filehead\">Showing first %d rows.</p>",
                   XLSX_MAX_ROWS);
}

/* ---- workbook ------------------------------------------------------------- */

typedef struct {
    char *name;
    char *rid;
} sheet_info;

char *convert_xlsx(const source_file *src) {
    const char *base = path_basename(src->path);

    mz_zip_archive za;
    memset(&za, 0, sizeof(za));
    if (!mz_zip_reader_init_mem(&za, src->data, src->len, 0))
        return page_error(base, "Not a valid XLSX file",
                          "could not open ZIP container");
    zcap zc;
    zcap_init(&zc, &za);

    size_t wblen = 0, sslen = 0, rellen = 0;
    char *wb = zcap_extract(&zc, "xl/workbook.xml", &wblen);
    if (!wb) {
        mz_zip_reader_end(&za);
        return page_error(base, "Not a valid XLSX file",
                          "xl/workbook.xml missing or oversized");
    }
    char *ss = zcap_extract(&zc, "xl/sharedStrings.xml", &sslen);
    char *rels = zcap_extract(&zc, "xl/_rels/workbook.xml.rels", &rellen);

    strtab strs;
    strtab_parse(&strs, ss, sslen);

    /* sheet list from workbook.xml */
    sheet_info sheets[XLSX_MAX_SHEETS];
    int nsheets = 0;
    {
        xml_reader x;
        xml_init(&x, wb, wblen);
        xml_event ev;
        while ((ev = xml_next(&x)) != XML_EOF) {
            if ((ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) &&
                strcmp(x.name, "sheet") == 0 && nsheets < XLSX_MAX_SHEETS) {
                sheets[nsheets].name = xml_attr(&x, "name");
                sheets[nsheets].rid = xml_attr(&x, "id"); /* matches r:id */
                if (sheets[nsheets].name && sheets[nsheets].rid)
                    nsheets++;
                else {
                    free(sheets[nsheets].name);
                    free(sheets[nsheets].rid);
                }
            }
        }
    }

    sb out;
    sb_init(&out);
    page_begin(&out, base, 0);
    sb_append(&out, "<div class=\"content-wide datatable\">");

    int emitted = 0;
    for (int i = 0; i < nsheets; i++) {
        /* resolve rId → worksheet path via workbook rels */
        char zpath[512] = "";
        if (rels) {
            xml_reader x;
            xml_init(&x, rels, rellen);
            xml_event ev;
            while ((ev = xml_next(&x)) != XML_EOF) {
                if ((ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) &&
                    strstr(x.name, "Relationship") &&
                    strcmp(x.name, "Relationships") != 0) {
                    char *id = xml_attr(&x, "Id");
                    char *tg = xml_attr(&x, "Target");
                    if (id && tg && strcmp(id, sheets[i].rid) == 0) {
                        if (tg[0] == '/')
                            snprintf(zpath, sizeof(zpath), "%s", tg + 1);
                        else
                            snprintf(zpath, sizeof(zpath), "xl/%s", tg);
                    }
                    free(id);
                    free(tg);
                    if (zpath[0])
                        break;
                }
            }
        }
        if (!zpath[0])
            snprintf(zpath, sizeof(zpath), "xl/worksheets/sheet%d.xml",
                     i + 1);

        size_t shlen = 0;
        char *sh = zcap_extract(&zc, zpath, &shlen);
        if (!sh)
            continue;
        sb_append(&out, "<h2>");
        sb_append_html(&out, sheets[i].name, strlen(sheets[i].name));
        sb_append(&out, "</h2>");
        sheet_to_table(&out, sh, shlen, &strs);
        mz_free(sh);
        emitted++;
    }

    sb_append(&out, "</div>");
    page_end(&out, 0);

    for (int i = 0; i < nsheets; i++) {
        free(sheets[i].name);
        free(sheets[i].rid);
    }
    strtab_free(&strs);
    mz_free(wb);
    if (ss)
        mz_free(ss);
    if (rels)
        mz_free(rels);
    mz_zip_reader_end(&za);

    if (!emitted) {
        sb_free(&out);
        return page_error(base, "Could not read any worksheets", NULL);
    }
    return sb_take(&out);
}
