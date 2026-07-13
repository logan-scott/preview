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

/* ---- cell formats (xl/styles.xml) ---------------------------------------- */

typedef enum { FMT_GENERAL, FMT_DATE, FMT_DATETIME, FMT_TIME, FMT_PERCENT }
    fmt_kind;

typedef struct {
    int *xf_fmt;   /* cellXfs index → numFmtId */
    int nxf, cap_xf;
    int *cf_id;    /* custom numFmt id */
    char **cf_code;
    int ncf, cap_cf;
} xlsx_styles;

static fmt_kind builtin_fmt_kind(int id) {
    switch (id) {
    case 14: case 15: case 16: case 17: return FMT_DATE;
    case 22:                            return FMT_DATETIME;
    case 18: case 19: case 20: case 21:
    case 45: case 46: case 47:          return FMT_TIME;
    case 9: case 10:                    return FMT_PERCENT;
    default:                            return FMT_GENERAL;
    }
}

/* Classify a custom format code by the field letters it uses (outside
 * quoted literals and [bracketed] sections). */
static fmt_kind custom_fmt_kind(const char *code) {
    int has_date = 0, has_time = 0, has_pct = 0, q = 0, brk = 0;
    for (const char *p = code; *p; p++) {
        char c = *p;
        if (q) { if (c == '"') q = 0; continue; }
        if (c == '"') { q = 1; continue; }
        if (c == '[') { brk = 1; continue; }
        if (c == ']') { brk = 0; continue; }
        if (brk) continue;
        if (c == '\\') { if (p[1]) p++; continue; }
        switch (c) {
        case 'y': case 'Y': case 'd': case 'D': has_date = 1; break;
        case 'h': case 'H': case 's': case 'S': has_time = 1; break;
        case '%': has_pct = 1; break;
        }
    }
    if (has_pct) return FMT_PERCENT;
    if (has_date && has_time) return FMT_DATETIME;
    if (has_date) return FMT_DATE;
    if (has_time) return FMT_TIME;
    return FMT_GENERAL;
}

static fmt_kind style_fmt_kind(const xlsx_styles *st, int s_index) {
    if (!st || s_index < 0 || s_index >= st->nxf)
        return FMT_GENERAL;
    int numfmt = st->xf_fmt[s_index];
    if (numfmt < 164) /* built-in */
        return builtin_fmt_kind(numfmt);
    for (int i = 0; i < st->ncf; i++)
        if (st->cf_id[i] == numfmt)
            return custom_fmt_kind(st->cf_code[i]);
    return FMT_GENERAL;
}

static void styles_parse(xlsx_styles *st, const char *xml, size_t len) {
    memset(st, 0, sizeof(*st));
    if (!xml)
        return;
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;
    int in_cellxfs = 0; /* the real cell formats (not cellStyleXfs) */
    while ((ev = xml_next(&x)) != XML_EOF) {
        if (ev == XML_ELEM_START || ev == XML_ELEM_EMPTY) {
            if (strcmp(x.name, "cellXfs") == 0) {
                in_cellxfs = 1;
            } else if (strcmp(x.name, "numFmt") == 0) {
                char *id = xml_attr(&x, "numFmtId");
                char *code = xml_attr(&x, "formatCode");
                if (id && code) {
                    if (st->ncf == st->cap_cf) {
                        st->cap_cf = st->cap_cf ? st->cap_cf * 2 : 16;
                        st->cf_id = realloc(st->cf_id,
                                            st->cap_cf * sizeof(int));
                        st->cf_code = realloc(st->cf_code,
                                              st->cap_cf * sizeof(char *));
                    }
                    st->cf_id[st->ncf] = atoi(id);
                    st->cf_code[st->ncf] = code;
                    st->ncf++;
                    code = NULL; /* owned by table now */
                }
                free(id);
                free(code);
            } else if (in_cellxfs && strcmp(x.name, "xf") == 0) {
                char *nf = xml_attr(&x, "numFmtId");
                if (st->nxf == st->cap_xf) {
                    st->cap_xf = st->cap_xf ? st->cap_xf * 2 : 32;
                    st->xf_fmt = realloc(st->xf_fmt,
                                         st->cap_xf * sizeof(int));
                }
                st->xf_fmt[st->nxf++] = nf ? atoi(nf) : 0;
                free(nf);
            }
        } else if (ev == XML_ELEM_END && strcmp(x.name, "cellXfs") == 0) {
            in_cellxfs = 0;
        }
    }
}

static void styles_free(xlsx_styles *st) {
    free(st->xf_fmt);
    for (int i = 0; i < st->ncf; i++)
        free(st->cf_code[i]);
    free(st->cf_id);
    free(st->cf_code);
}

/* days since 1970-01-01 → Gregorian y/m/d (Howard Hinnant's algorithm) */
static void civil_from_days(long z, int *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yr = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yr + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

/* Render an Excel serial date/time (1900 system) per the format kind. */
static void append_serial(sb *out, double serial, fmt_kind k) {
    /* 25569 = Excel serial of 1970-01-01 in the 1900 date system; the
     * constant already absorbs the historical 1900 leap-year quirk for
     * modern dates. */
    long day = (long)serial;
    long unix_days = day - 25569;
    double frac = serial - (double)day;
    long secs = (long)(frac * 86400.0 + 0.5);
    if (secs >= 86400) { secs -= 86400; unix_days++; }
    int hh = (int)(secs / 3600), mm = (int)((secs % 3600) / 60),
        ss = (int)(secs % 60);
    int y, mo, d;
    civil_from_days(unix_days, &y, &mo, &d);
    char buf[40];
    if (k == FMT_TIME)
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mm, ss);
    else if (k == FMT_DATETIME)
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", y, mo, d, hh,
                 mm);
    else
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, mo, d);
    sb_append(out, buf);
}

static void append_numeric(sb *out, const char *v, fmt_kind k) {
    if (k == FMT_DATE || k == FMT_DATETIME || k == FMT_TIME) {
        append_serial(out, atof(v), k);
    } else if (k == FMT_PERCENT) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%g%%", atof(v) * 100.0);
        sb_append(out, buf);
    } else {
        sb_append_html(out, v, strlen(v));
    }
}

/* ---- one worksheet → <table> --------------------------------------------- */

static void sheet_to_table(sb *out, const char *xml, size_t len,
                           const strtab *strs, const xlsx_styles *styles) {
    xml_reader x;
    xml_init(&x, xml, len);
    xml_event ev;

    sb_append(out, "<table>");
    int rows = 0, truncated = 0;
    int in_row = 0, in_v = 0, in_is = 0, in_t = 0;
    int cur_col = 0, next_col = 0;
    int cell_style = -1;
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
                char *s = xml_attr(&x, "s");
                cur_col = r ? col_of_ref(r) : next_col;
                if (cur_col < 0 || cur_col >= XLSX_MAX_COLS)
                    cur_col = next_col;
                snprintf(cell_type, sizeof(cell_type), "%s", t ? t : "");
                cell_style = s ? atoi(s) : -1;
                free(r);
                free(t);
                free(s);
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
                } else if (cell_type[0] == '\0' && val.len > 0) {
                    /* numeric cell: apply date/percent formatting by style */
                    append_numeric(out, val.buf,
                                   style_fmt_kind(styles, cell_style));
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
    size_t stlen = 0;
    char *stxml = zcap_extract(&zc, "xl/styles.xml", &stlen);

    strtab strs;
    strtab_parse(&strs, ss, sslen);
    xlsx_styles styles;
    styles_parse(&styles, stxml, stlen);

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
        sheet_to_table(&out, sh, shlen, &strs, &styles);
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
    styles_free(&styles);
    mz_free(wb);
    if (ss)
        mz_free(ss);
    if (rels)
        mz_free(rels);
    if (stxml)
        mz_free(stxml);
    mz_zip_reader_end(&za);

    if (!emitted) {
        sb_free(&out);
        return page_error(base, "Could not read any worksheets", NULL);
    }
    return sb_take(&out);
}
