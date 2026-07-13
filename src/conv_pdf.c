#include "conv_pdf.h"

#include <stdio.h>
#include <string.h>

#include "page.h"
#include "str.h"

#ifdef PREVIEW_NO_PDF

char *convert_pdf(const source_file *src) {
    return page_error(path_basename(src->path),
                      "PDF support was not built into this binary",
                      "rebuild with mupdf available (see README)");
}

#else

#include <mupdf/fitz.h>

/* 2x nominal 72dpi so pages stay sharp on hidpi displays; CSS scales the
 * <img> back down to page width. */
#define PDF_RENDER_SCALE 2.0f
/* Keep the generated document bounded for very long PDFs. */
#define PDF_MAX_PAGES 200

char *convert_pdf(const source_file *src) {
    const char *base = path_basename(src->path);

    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!ctx)
        return page_error(base, "Could not initialize PDF engine", NULL);

    char errbuf[256] = "";
    fz_stream *stm = NULL;
    fz_document *doc = NULL;
    sb s;
    sb_init(&s);
    int emitted_pages = 0, page_count = 0;

    fz_var(stm);
    fz_var(doc);

    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        stm = fz_open_memory(ctx, src->data, src->len);
        doc = fz_open_document_with_stream(ctx, ".pdf", stm);

        if (fz_needs_password(ctx, doc))
            fz_throw(ctx, FZ_ERROR_GENERIC,
                     "document is password-protected");

        page_count = fz_count_pages(ctx, doc);
        int render_count =
            page_count < PDF_MAX_PAGES ? page_count : PDF_MAX_PAGES;

        page_begin(&s, base, 0);
        sb_append(&s,
                  "<style>"
                  "body{background:var(--code-bg)}"
                  ".pdf{display:flex;flex-direction:column;align-items:center;"
                  "gap:16px;padding:24px 12px}"
                  ".pdf img{width:min(100%,850px);height:auto;background:#fff;"
                  "box-shadow:0 2px 12px var(--shadow)}"
                  ".pagecount{color:var(--muted);font-size:.8em}"
                  "</style><div class=\"pdf\">");

        for (int i = 0; i < render_count; i++) {
            fz_matrix ctm = fz_scale(PDF_RENDER_SCALE, PDF_RENDER_SCALE);
            fz_pixmap *pix = NULL;
            fz_buffer *png = NULL;
            fz_var(pix);
            fz_var(png);
            fz_try(ctx) {
                pix = fz_new_pixmap_from_page_number(ctx, doc, i, ctm,
                                                     fz_device_rgb(ctx), 0);
                png = fz_new_buffer_from_pixmap_as_png(
                    ctx, pix, fz_default_color_params);
                unsigned char *pngdata;
                size_t pnglen = fz_buffer_storage(ctx, png, &pngdata);
                sb_appendf(&s, "<img id=\"page%d\" src=\"data:image/png;base64,",
                           i + 1);
                sb_append_base64(&s, pngdata, pnglen);
                sb_append(&s, "\" alt=\"\">");
                emitted_pages++;
            }
            fz_always(ctx) {
                fz_drop_buffer(ctx, png);
                fz_drop_pixmap(ctx, pix);
            }
            fz_catch(ctx) {
                sb_appendf(&s,
                           "<p class=\"pagecount\">page %d failed to "
                           "render</p>",
                           i + 1);
            }
        }

        if (page_count > render_count)
            sb_appendf(&s,
                       "<p class=\"pagecount\">Showing first %d of %d "
                       "pages.</p>",
                       render_count, page_count);
        sb_appendf(&s, "<p class=\"pagecount\">%d page%s</p></div>",
                   page_count, page_count == 1 ? "" : "s");
        page_end(&s, 0);
    }
    fz_always(ctx) {
        fz_drop_document(ctx, doc);
        fz_drop_stream(ctx, stm);
    }
    fz_catch(ctx) {
        snprintf(errbuf, sizeof(errbuf), "%s", fz_caught_message(ctx));
    }
    fz_drop_context(ctx);

    if (errbuf[0] || emitted_pages == 0) {
        sb_free(&s);
        return page_error(base, "Could not render PDF",
                          errbuf[0] ? errbuf : "no pages could be rendered");
    }
    return sb_take(&s);
}

#endif /* PREVIEW_NO_PDF */
