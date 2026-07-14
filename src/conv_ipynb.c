#include "conv_ipynb.h"

#include <stdlib.h>
#include <string.h>

#include "cjson/cJSON.h"
#include "md4c-html.h"

#include "page.h"
#include "str.h"

/* "source" and "text" fields are either a string or an array of strings
 * (lines). Append the joined content to out. */
static void append_source(sb *out, const cJSON *node) {
    if (!node)
        return;
    if (cJSON_IsString(node)) {
        sb_append(out, node->valuestring);
    } else if (cJSON_IsArray(node)) {
        const cJSON *line;
        cJSON_ArrayForEach(line, node) {
            if (cJSON_IsString(line))
                sb_append(out, line->valuestring);
        }
    }
}

static void md_out(const MD_CHAR *text, MD_SIZE size, void *ud) {
    sb_append_n((sb *)ud, text, size);
}

/* Strip ANSI SGR escape sequences (used in tracebacks) in place-ish: copy
 * the printable text into out. */
static void append_no_ansi(sb *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == 0x1b && i + 1 < n && s[i + 1] == '[') {
            i += 2;
            while (i < n && s[i] != 'm' && !(s[i] >= '@' && s[i] <= '~'))
                i++;
            /* i now at the final byte of the sequence; loop ++ skips it */
        } else {
            sb_append_html(out, &s[i], 1);
        }
    }
}

static const char *notebook_language(const cJSON *root) {
    const cJSON *md = cJSON_GetObjectItemCaseSensitive(root, "metadata");
    if (!md)
        return "python";
    const cJSON *li =
        cJSON_GetObjectItemCaseSensitive(md, "language_info");
    if (li) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(li, "name");
        if (cJSON_IsString(name) && name->valuestring[0])
            return name->valuestring;
    }
    const cJSON *ks = cJSON_GetObjectItemCaseSensitive(md, "kernelspec");
    if (ks) {
        const cJSON *lang =
            cJSON_GetObjectItemCaseSensitive(ks, "language");
        if (cJSON_IsString(lang) && lang->valuestring[0])
            return lang->valuestring;
    }
    return "python";
}

/* Only allow a plausible highlight.js language token into a class name. */
static int safe_lang(const char *s) {
    if (!s || !s[0] || strlen(s) > 20)
        return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '_'))
            return 0;
    return 1;
}

static void render_markdown_cell(sb *out, const cJSON *cell) {
    sb md;
    sb_init(&md);
    append_source(&md, cJSON_GetObjectItemCaseSensitive(cell, "source"));
    sb html;
    sb_init(&html);
    if (md_html((const MD_CHAR *)md.buf, (MD_SIZE)md.len, md_out, &html,
                MD_DIALECT_GITHUB | MD_FLAG_NOHTML, 0) == 0) {
        sb_append(out, "<div class=\"nb-md\">");
        sb_append(out, html.buf);
        sb_append(out, "</div>");
    }
    sb_free(&md);
    sb_free(&html);
}

/* Render one output object (execute_result/display_data/stream/error). */
static void render_output(sb *out, const cJSON *o) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(o, "output_type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(t, "stream") == 0) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(o, "name");
        int is_err = cJSON_IsString(name) &&
                     strcmp(name->valuestring, "stderr") == 0;
        sb txt;
        sb_init(&txt);
        append_source(&txt, cJSON_GetObjectItemCaseSensitive(o, "text"));
        sb_appendf(out, "<pre class=\"nb-out%s\">",
                   is_err ? " nb-err" : "");
        sb_append_html(out, txt.buf, txt.len);
        sb_append(out, "</pre>");
        sb_free(&txt);
        return;
    }

    if (strcmp(t, "error") == 0) {
        sb tb;
        sb_init(&tb);
        const cJSON *trace =
            cJSON_GetObjectItemCaseSensitive(o, "traceback");
        const cJSON *line;
        if (cJSON_IsArray(trace)) {
            cJSON_ArrayForEach(line, trace) {
                if (cJSON_IsString(line)) {
                    append_no_ansi(&tb, line->valuestring,
                                   strlen(line->valuestring));
                    sb_append(&tb, "\n");
                }
            }
        }
        sb_append(out, "<pre class=\"nb-out nb-err\">");
        sb_append_n(out, tb.buf, tb.len); /* already HTML-escaped */
        sb_append(out, "</pre>");
        sb_free(&tb);
        return;
    }

    /* execute_result / display_data: prefer image, then plain text. Rich
     * text/html is shown as escaped text — a notebook is untrusted, and
     * its HTML could carry scripts. */
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(o, "data");
    if (!data)
        return;
    const char *img_keys[] = {"image/png", "image/jpeg", "image/gif"};
    const char *img_mime[] = {"image/png", "image/jpeg", "image/gif"};
    for (int i = 0; i < 3; i++) {
        const cJSON *img = cJSON_GetObjectItemCaseSensitive(data, img_keys[i]);
        if (cJSON_IsString(img) && img->valuestring[0]) {
            /* value is already base64 */
            sb_appendf(out, "<img class=\"nb-img\" src=\"data:%s;base64,",
                       img_mime[i]);
            /* strip whitespace/newlines the JSON may contain */
            for (const char *p = img->valuestring; *p; p++)
                if (*p != '\n' && *p != '\r' && *p != ' ')
                    sb_append_n(out, p, 1);
            sb_append(out, "\" alt=\"\">");
            return;
        }
    }
    const cJSON *plain =
        cJSON_GetObjectItemCaseSensitive(data, "text/plain");
    if (plain) {
        sb txt;
        sb_init(&txt);
        append_source(&txt, plain);
        sb_append(out, "<pre class=\"nb-out\">");
        sb_append_html(out, txt.buf, txt.len);
        sb_append(out, "</pre>");
        sb_free(&txt);
        return;
    }
    const cJSON *html =
        cJSON_GetObjectItemCaseSensitive(data, "text/html");
    if (html) {
        sb txt;
        sb_init(&txt);
        append_source(&txt, html);
        sb_append(out, "<pre class=\"nb-out nb-rawhtml\">");
        sb_append_html(out, txt.buf, txt.len);
        sb_append(out, "</pre>");
        sb_free(&txt);
    }
}

static void render_code_cell(sb *out, const cJSON *cell, const char *lang) {
    const cJSON *ec =
        cJSON_GetObjectItemCaseSensitive(cell, "execution_count");
    sb_append(out, "<div class=\"nb-code\"><div class=\"nb-prompt\">");
    if (cJSON_IsNumber(ec))
        sb_appendf(out, "In [%d]:", ec->valueint);
    else
        sb_append(out, "In [ ]:");
    sb_append(out, "</div><pre><code");
    if (safe_lang(lang))
        sb_appendf(out, " class=\"language-%s\"", lang);
    sb_append(out, ">");
    sb src;
    sb_init(&src);
    append_source(&src, cJSON_GetObjectItemCaseSensitive(cell, "source"));
    sb_append_html(out, src.buf, src.len);
    sb_free(&src);
    sb_append(out, "</code></pre></div>"); /* close nb-code (prompt+code) */

    /* Outputs are siblings below the code, aligned under it via margin. */
    const cJSON *outputs =
        cJSON_GetObjectItemCaseSensitive(cell, "outputs");
    if (cJSON_IsArray(outputs)) {
        const cJSON *o;
        cJSON_ArrayForEach(o, outputs)
            render_output(out, o);
    }
}

char *convert_ipynb(const source_file *src) {
    const char *base = path_basename(src->path);
    cJSON *root = cJSON_ParseWithLength((const char *)src->data, src->len);
    if (!root)
        return page_error(base, "Could not parse notebook",
                          "invalid JSON");
    const cJSON *cells = cJSON_GetObjectItemCaseSensitive(root, "cells");
    if (!cJSON_IsArray(cells)) {
        cJSON_Delete(root);
        return page_error(base, "Not a valid notebook",
                          "no cells array");
    }
    const char *lang = notebook_language(root);

    sb s;
    sb_init(&s);
    page_begin(&s, base, PAGE_HLJS);
    sb_append(&s,
              "<style>"
              ".nb{max-width:900px;margin:0 auto;padding:24px 20px}"
              ".nb-code{display:flex;gap:10px;margin:10px 0}"
              ".nb-prompt{color:var(--muted);font-family:ui-monospace,"
              "monospace;font-size:.8em;white-space:nowrap;padding-top:18px;"
              "min-width:64px;text-align:right}"
              ".nb-code pre{flex:1;margin:0}"
              ".nb-md{margin:8px 0}"
              ".nb-out{background:var(--bg);border:1px solid var(--border);"
              "border-radius:6px;padding:8px 12px;margin:6px 0 6px 74px;"
              "white-space:pre-wrap;font-size:.85em}"
              ".nb-err{color:#b00020;border-color:#b0002040}"
              "@media(prefers-color-scheme:dark){.nb-err{color:#ff6b6b}}"
              ".nb-img{margin:6px 0 6px 74px;max-width:calc(100% - 74px)}"
              ".nb-rawhtml{color:var(--muted)}"
              "</style><div class=\"nb\">");

    const cJSON *cell;
    cJSON_ArrayForEach(cell, cells) {
        const cJSON *ct =
            cJSON_GetObjectItemCaseSensitive(cell, "cell_type");
        const char *type = cJSON_IsString(ct) ? ct->valuestring : "";
        if (strcmp(type, "markdown") == 0)
            render_markdown_cell(&s, cell);
        else if (strcmp(type, "code") == 0)
            render_code_cell(&s, cell, lang);
        /* raw cells are shown as-is text */
        else if (strcmp(type, "raw") == 0) {
            sb raw;
            sb_init(&raw);
            append_source(&raw,
                          cJSON_GetObjectItemCaseSensitive(cell, "source"));
            sb_append(&s, "<pre class=\"nb-out\">");
            sb_append_html(&s, raw.buf, raw.len);
            sb_append(&s, "</pre>");
            sb_free(&raw);
        }
    }

    sb_append(&s, "</div>");
    page_end(&s, PAGE_HLJS);
    cJSON_Delete(root);
    return sb_take(&s);
}
