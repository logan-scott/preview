#include "convert.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conv_docx.h"
#include "conv_ipynb.h"
#include "conv_pdf.h"
#include "conv_pptx.h"
#include "conv_xlsx.h"
#include "md4c-html.h"
#include "page.h"
#include "str.h"
#include "xmlmini.h"

#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

/* Above this size, skip client-side syntax highlighting (too slow). */
#define HLJS_MAX_BYTES (400 * 1024)
#define CSV_MAX_ROWS 10000
#define HEXDUMP_BYTES 512

/* --- helpers ------------------------------------------------------------ */

/* portable memmem */
static const void *mem_find(const void *hay, size_t haylen,
                            const void *needle, size_t nlen) {
    if (nlen == 0 || haylen < nlen)
        return NULL;
    const uint8_t *h = hay;
    for (size_t i = 0; i + nlen <= haylen; i++)
        if (memcmp(h + i, needle, nlen) == 0)
            return h + i;
    return NULL;
}

static const char *image_mime(const uint8_t *d, size_t n, const char *ext) {
    /* magic first — it can't lie */
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (n >= 3 && memcmp(d, "\xff\xd8\xff", 3) == 0)     return "image/jpeg";
    if (n >= 6 && (memcmp(d, "GIF87a", 6) == 0 ||
                   memcmp(d, "GIF89a", 6) == 0))          return "image/gif";
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 &&
        memcmp(d + 8, "WEBP", 4) == 0)                    return "image/webp";
    if (n >= 2 && memcmp(d, "BM", 2) == 0)                return "image/bmp";
    /* text-ish formats by extension */
    if (strcmp(ext, "svg") == 0)  return "image/svg+xml";
    if (strcmp(ext, "ico") == 0)  return "image/x-icon";
    if (strcmp(ext, "avif") == 0) return "image/avif";
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0)
        return "image/jpeg";
    return "application/octet-stream";
}

static void append_data_uri(sb *s, const char *mime, const uint8_t *data,
                            size_t len) {
    sb_appendf(s, "data:%s;base64,", mime);
    sb_append_base64(s, data, len);
}

/* Directory part of a path, including trailing slash ("" if none). */
static char *path_dir(const char *path) {
    const char *base = path_basename(path);
    size_t n = (size_t)(base - path);
    char *dir = malloc(n + 1);
    memcpy(dir, path, n);
    dir[n] = '\0';
    return dir;
}

/* --- markdown ------------------------------------------------------------ */

static void md_out(const MD_CHAR *text, MD_SIZE size, void *ud) {
    sb_append_n((sb *)ud, text, size);
}

/* The webview loads our HTML from a string, so relative <img> references
 * in markdown would have no base to resolve against (and WebKit refuses
 * file:// subresources from non-file origins anyway). Embed local images
 * as data URIs instead — but only content that verifiably IS an image:
 * documents may reference arbitrary paths, and inlining non-image files
 * would copy their bytes into the page. */
static char *embed_local_images(const char *html, const char *srcdir) {
    sb out;
    sb_init(&out);
    const char *p = html;
    while (*p) {
        const char *tag = strstr(p, "<img");
        if (!tag) {
            sb_append(&out, p);
            break;
        }
        const char *gt = strchr(tag, '>');
        if (!gt) {
            sb_append(&out, p);
            break;
        }
        /* find src="..." inside the tag */
        const char *srcattr = NULL;
        for (const char *q = tag + 4; q < gt - 4; q++) {
            if ((q[-1] == ' ' || q[-1] == '\t' || q[-1] == '"' ||
                 q[-1] == '\'') &&
                strncmp(q, "src=", 4) == 0 && (q[4] == '"' || q[4] == '\'')) {
                srcattr = q;
                break;
            }
        }
        if (!srcattr) {
            sb_append_n(&out, p, (size_t)(gt + 1 - p));
            p = gt + 1;
            continue;
        }
        char quote = srcattr[4];
        const char *vstart = srcattr + 5;
        const char *vend = memchr(vstart, quote, (size_t)(gt - vstart));
        if (!vend) {
            sb_append_n(&out, p, (size_t)(gt + 1 - p));
            p = gt + 1;
            continue;
        }
        size_t vlen = (size_t)(vend - vstart);
        char *url = malloc(vlen + 1);
        memcpy(url, vstart, vlen);
        url[vlen] = '\0';

        int remote = strncmp(url, "http:", 5) == 0 ||
                     strncmp(url, "https:", 6) == 0 ||
                     strncmp(url, "data:", 5) == 0 ||
                     strncmp(url, "//", 2) == 0;

        uint8_t *img = NULL;
        size_t imglen = 0;
        if (!remote) {
            const char *fspath = url;
            if (strncmp(fspath, "file://", 7) == 0)
                fspath += 7;
            char full[4096];
            if (fspath[0] == '/')
                snprintf(full, sizeof(full), "%s", fspath);
            else
                snprintf(full, sizeof(full), "%s%s", srcdir, fspath);
            const char *err;
            img = read_entire_file(full, &imglen, &err);
        }

        const char *mime =
            img ? image_mime(img, imglen, path_ext(url)) : NULL;
        if (mime && strcmp(mime, "image/svg+xml") == 0) {
            /* svg is identified by extension only; require the content
             * to actually look like svg before inlining it */
            size_t scan = imglen < 1024 ? imglen : 1024;
            if (!mem_find(img, scan, "<svg", 4))
                mime = NULL;
        }
        if (mime && strcmp(mime, "application/octet-stream") != 0) {
            /* copy tag up to the value, splice in the data URI */
            sb_append_n(&out, p, (size_t)(vstart - p));
            append_data_uri(&out, mime, img, imglen);
            sb_append_n(&out, vend, (size_t)(gt + 1 - vend));
        } else if (img) {
            /* exists but isn't an image: keep the surrounding markup,
             * drop only the reference itself */
            sb_append_n(&out, p, (size_t)(tag - p));
            sb_append(&out, "<span></span>");
        } else {
            sb_append_n(&out, p, (size_t)(gt + 1 - p));
        }
        free(img);
        free(url);
        p = gt + 1;
    }
    return sb_take(&out);
}

/* md4c emits link targets verbatim (HTML-escaped but scheme-unchecked).
 * Rewrite any <a href="..."> whose target has a disallowed scheme —
 * javascript:, data:, file:, ... — to an inert fragment link. */
static char *sanitize_links(const char *html) {
    sb out;
    sb_init(&out);
    const char *p = html;
    while (*p) {
        const char *a = strstr(p, "<a href=\"");
        if (!a) {
            sb_append(&out, p);
            break;
        }
        const char *vstart = a + 9;
        const char *vend = strchr(vstart, '"');
        if (!vend) {
            sb_append(&out, p);
            break;
        }
        sb_append_n(&out, p, (size_t)(vstart - p));

        /* the value is HTML-escaped; decode entities before checking so
         * "jav&#x09;ascript:" can't sneak past */
        sb raw;
        sb_init(&raw);
        xml_decode_into(&raw, vstart, (size_t)(vend - vstart));
        if (safe_link_scheme(raw.buf))
            sb_append_n(&out, vstart, (size_t)(vend - vstart));
        else
            sb_append(&out, "#blocked");
        sb_free(&raw);
        p = vend;
    }
    return sb_take(&out);
}

static char *convert_markdown(const source_file *src) {
    sb body;
    sb_init(&body);
    /* MD_FLAG_NOHTML: raw HTML in markdown is rendered as literal text.
     * The webview has network access, so letting documents carry live
     * <script> would turn any malicious README into an exfiltration
     * vehicle. Fidelity cost: <details>, <br> etc. show as text. */
    int rc = md_html((const MD_CHAR *)src->data, (MD_SIZE)src->len, md_out,
                     &body, MD_DIALECT_GITHUB | MD_FLAG_NOHTML, 0);
    if (rc != 0) {
        sb_free(&body);
        return page_error(path_basename(src->path), "Could not parse markdown",
                          src->path);
    }
    char *sanitized = sanitize_links(body.buf);
    sb_free(&body);
    char *dir = path_dir(src->path);
    char *embedded = embed_local_images(sanitized, dir);
    free(sanitized);
    free(dir);

    sb s;
    sb_init(&s);
    page_begin(&s, path_basename(src->path), PAGE_HLJS);
    sb_append(&s, "<div class=\"content\">");
    sb_append(&s, embedded);
    sb_append(&s, "</div>");
    page_end(&s, PAGE_HLJS);
    free(embedded);
    return sb_take(&s);
}

/* --- plain text / source code -------------------------------------------- */

/* Basenames with well-known languages but no (useful) extension. */
static const char *language_for(const char *path) {
    const char *base = path_basename(path);
    if (strcmp(base, "Makefile") == 0 || strcmp(base, "makefile") == 0 ||
        strcmp(base, "GNUmakefile") == 0)
        return "makefile";
    if (strcmp(base, "Dockerfile") == 0)
        return "dockerfile";
    if (strcmp(base, "CMakeLists.txt") == 0)
        return "cmake";
    const char *ext = path_ext(path);
    return ext[0] ? ext : NULL;
}

static char *convert_text(const source_file *src) {
    unsigned flags = src->len <= HLJS_MAX_BYTES ? PAGE_HLJS : 0;
    sb s;
    sb_init(&s);
    page_begin(&s, path_basename(src->path), flags);
    const char *lang = language_for(src->path);
    sb_append(&s, "<div class=\"content-wide\"><pre style=\"tab-size:4\"><code");
    if (lang && flags)
        sb_appendf(&s, " class=\"language-%s\"", lang);
    sb_append(&s, ">");
    sb_append_html(&s, (const char *)src->data, src->len);
    sb_append(&s, "</code></pre></div>");
    page_end(&s, flags);
    return sb_take(&s);
}

/* --- json ----------------------------------------------------------------- */

static void json_indent(sb *out, int depth) {
    sb_append(out, "\n");
    for (int i = 0; i < depth; i++)
        sb_append(out, "  ");
}

/* Token-level re-indenter: no full parse needed, handles strings/escapes
 * correctly, and never fails — malformed JSON just comes out lightly
 * reformatted. */
static void json_pretty(sb *out, const char *s, size_t n) {
    int depth = 0, in_str = 0, esc = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (in_str) {
            sb_append_n(out, &c, 1);
            if (esc)
                esc = 0;
            else if (c == '\\')
                esc = 1;
            else if (c == '"')
                in_str = 0;
            continue;
        }
        switch (c) {
        case '"':
            in_str = 1;
            sb_append_n(out, &c, 1);
            break;
        case '{':
        case '[': {
            /* keep empty containers on one line */
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' ||
                             s[j] == '\r'))
                j++;
            if (j < n && s[j] == (c == '{' ? '}' : ']')) {
                sb_append(out, c == '{' ? "{}" : "[]");
                i = j;
            } else {
                sb_append_n(out, &c, 1);
                depth++;
                json_indent(out, depth);
            }
            break;
        }
        case '}':
        case ']':
            depth = depth > 0 ? depth - 1 : 0;
            json_indent(out, depth);
            sb_append_n(out, &c, 1);
            break;
        case ',':
            sb_append_n(out, &c, 1);
            json_indent(out, depth);
            break;
        case ':':
            sb_append(out, ": ");
            break;
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            break;
        default:
            sb_append_n(out, &c, 1);
        }
    }
}

static char *convert_json(const source_file *src) {
    sb pretty;
    sb_init(&pretty);
    json_pretty(&pretty, (const char *)src->data, src->len);

    unsigned flags = pretty.len <= HLJS_MAX_BYTES ? PAGE_HLJS : 0;
    sb s;
    sb_init(&s);
    page_begin(&s, path_basename(src->path), flags);
    sb_append(&s, "<div class=\"content-wide\"><pre><code");
    if (flags)
        sb_append(&s, " class=\"language-json\"");
    sb_append(&s, ">");
    sb_append_html(&s, pretty.buf, pretty.len);
    sb_append(&s, "</code></pre></div>");
    page_end(&s, flags);
    sb_free(&pretty);
    return sb_take(&s);
}

/* --- csv ------------------------------------------------------------------ */

static char csv_sniff_delim(const char *ext, const uint8_t *d, size_t n) {
    if (strcmp(ext, "tsv") == 0)
        return '\t';
    int commas = 0, semis = 0, tabs = 0, in_q = 0;
    for (size_t i = 0; i < n && d[i] != '\n'; i++) {
        char c = (char)d[i];
        if (c == '"')
            in_q = !in_q;
        else if (!in_q) {
            if (c == ',') commas++;
            else if (c == ';') semis++;
            else if (c == '\t') tabs++;
        }
    }
    if (tabs > commas && tabs > semis) return '\t';
    if (semis > commas) return ';';
    return ',';
}

static char *convert_csv(const source_file *src) {
    char delim = csv_sniff_delim(path_ext(src->path), src->data, src->len);
    sb s;
    sb_init(&s);
    page_begin(&s, path_basename(src->path), 0);
    sb_append(&s, "<div class=\"content-wide datatable\"><table>");

    const char *p = (const char *)src->data;
    const char *end = p + src->len;
    int row = 0, truncated = 0;
    sb cell;
    sb_init(&cell);

    while (p < end) {
        if (row >= CSV_MAX_ROWS) {
            truncated = 1;
            break;
        }
        const char *tag = row == 0 ? "th" : "td";
        if (row == 0)
            sb_append(&s, "<thead><tr>");
        else
            sb_appendf(&s, "<tr><td class=\"rownum\">%d</td>", row);
        if (row == 0)
            sb_append(&s, "<th class=\"rownum\"></th>");

        int row_done = 0;
        while (!row_done) {
            cell.len = 0;
            cell.buf[0] = '\0';
            if (p < end && *p == '"') { /* quoted field */
                p++;
                while (p < end) {
                    if (*p == '"' && p + 1 < end && p[1] == '"') {
                        sb_append_n(&cell, "\"", 1);
                        p += 2;
                    } else if (*p == '"') {
                        p++;
                        break;
                    } else {
                        sb_append_n(&cell, p, 1);
                        p++;
                    }
                }
                /* skip to delimiter/newline */
                while (p < end && *p != delim && *p != '\n' && *p != '\r')
                    p++;
            } else {
                while (p < end && *p != delim && *p != '\n' && *p != '\r') {
                    sb_append_n(&cell, p, 1);
                    p++;
                }
            }
            sb_appendf(&s, "<%s>", tag);
            sb_append_html(&s, cell.buf, cell.len);
            sb_appendf(&s, "</%s>", tag);

            if (p >= end) {
                row_done = 1;
            } else if (*p == delim) {
                p++;
                if (p >= end)
                    row_done = 1; /* trailing delimiter: done after this */
            } else { /* newline(s) */
                if (*p == '\r')
                    p++;
                if (p < end && *p == '\n')
                    p++;
                row_done = 1;
            }
        }
        sb_append(&s, "</tr>");
        if (row == 0)
            sb_append(&s, "</thead><tbody>");
        row++;
    }
    sb_free(&cell);
    sb_append(&s, "</tbody></table>");
    if (truncated)
        sb_appendf(&s,
                   "<p class=\"filehead\">Showing first %d rows only.</p>",
                   CSV_MAX_ROWS);
    sb_append(&s, "</div>");
    page_end(&s, 0);
    return sb_take(&s);
}

/* --- images ---------------------------------------------------------------- */

static char *image_page(const source_file *src, const char *mime,
                        const uint8_t *data, size_t len) {
    sb s;
    sb_init(&s);
    page_begin(&s, path_basename(src->path), 0);
    sb_append(&s, "<div class=\"imgview\"><img id=\"im\" src=\"");
    append_data_uri(&s, mime, data, len);
    sb_append(&s, "\" alt=\"\"></div><script>"
                  "const im=document.getElementById('im');"
                  "im.addEventListener('load',()=>{document.title+="
                  "' \\u2014 '+im.naturalWidth+'\\u00d7'+im.naturalHeight;});"
                  "</script>");
    page_end(&s, 0);
    return sb_take(&s);
}

static char *convert_image(const source_file *src) {
    const char *mime = image_mime(src->data, src->len, path_ext(src->path));
    return image_page(src, mime, src->data, src->len);
}

static void png_write_cb(void *ctx, void *data, int size) {
    sb_append_n((sb *)ctx, data, (size_t)size);
}

static char *convert_image_stb(const source_file *src) {
    if (src->len > INT_MAX)
        return page_error(path_basename(src->path), "Image too large",
                          "stb_image takes sizes as int");
    int w, h, comp;
    stbi_uc *pix = stbi_load_from_memory(src->data, (int)src->len, &w, &h,
                                         &comp, 4);
    if (!pix)
        return page_error(path_basename(src->path), "Could not decode image",
                          stbi_failure_reason());
    sb png;
    sb_init(&png);
    int ok = stbi_write_png_to_func(png_write_cb, &png, w, h, 4, pix, w * 4);
    stbi_image_free(pix);
    if (!ok) {
        sb_free(&png);
        return page_error(path_basename(src->path), "Could not encode image",
                          src->path);
    }
    char *html = image_page(src, "image/png", (const uint8_t *)png.buf,
                            png.len);
    sb_free(&png);
    return html;
}

/* --- binary fallback -------------------------------------------------------- */

static char *convert_binary(const source_file *src) {
    sb s;
    sb_init(&s);
    page_begin(&s, path_basename(src->path), 0);
    sb_append(&s, "<div class=\"content\"><h2>No preview available</h2>"
                  "<p class=\"filehead\">");
    sb_append_html(&s, src->path, strlen(src->path));
    sb_appendf(&s, " &middot; %zu bytes &middot; binary</p><pre><code>",
               src->len);
    size_t n = src->len < HEXDUMP_BYTES ? src->len : HEXDUMP_BYTES;
    for (size_t off = 0; off < n; off += 16) {
        sb_appendf(&s, "%08zx  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < n)
                sb_appendf(&s, "%02x ", src->data[off + i]);
            else
                sb_append(&s, "   ");
            if (i == 7)
                sb_append(&s, " ");
        }
        sb_append(&s, " |");
        for (size_t i = 0; i < 16 && off + i < n; i++) {
            uint8_t c = src->data[off + i];
            char pc = (c >= 32 && c < 127) ? (char)c : '.';
            sb_append_html(&s, &pc, 1);
        }
        sb_append(&s, "|\n");
    }
    if (src->len > n)
        sb_appendf(&s, "... (%zu more bytes)\n", src->len - n);
    sb_append(&s, "</code></pre></div>");
    page_end(&s, 0);
    return sb_take(&s);
}

/* --- dispatch ----------------------------------------------------------------- */

typedef char *(*convert_fn)(const source_file *);

static const struct {
    filetype type;
    convert_fn fn;
} CONVERTERS[] = {
    {FT_MARKDOWN, convert_markdown},
    {FT_TEXT, convert_text},
    {FT_JSON, convert_json},
    {FT_IPYNB, convert_ipynb},
    {FT_CSV, convert_csv},
    {FT_IMAGE, convert_image},
    {FT_IMAGE_STB, convert_image_stb},
    {FT_BINARY, convert_binary},
    {FT_PDF, convert_pdf},
    {FT_DOCX, convert_docx},
    {FT_PPTX, convert_pptx},
    {FT_XLSX, convert_xlsx},
};

char *convert_to_html(const source_file *src) {
    for (size_t i = 0; i < sizeof(CONVERTERS) / sizeof(CONVERTERS[0]); i++)
        if (CONVERTERS[i].type == src->type)
            return CONVERTERS[i].fn(src);
    return page_error(path_basename(src->path), "Unsupported file type",
                      src->path);
}
