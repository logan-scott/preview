#include "conv_pdf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "page.h"
#include "str.h"

const char *preview_self = NULL;

#ifdef PREVIEW_NO_PDF

/* Without mupdf, render PDFs client-side with a bundled copy of pdf.js.
 * The library, its worker, and the PDF bytes are embedded (base64) into a
 * page that reconstructs them as blob URLs and draws each page to a
 * canvas — no native dependency, same output on macOS and Linux. */
#include "asset_pdfjs_js.h"
#include "asset_pdfjs_worker_js.h"

char *pdf_render_inproc(const source_file *src) {
    sb s;
    sb_init(&s);
    page_begin(&s, path_basename(src->path), PAGE_PDFJS);
    sb_append(&s,
              "<style>body{background:var(--code-bg)}"
              ".pdf{display:flex;flex-direction:column;align-items:center;"
              "gap:16px;padding:24px 12px}"
              ".pdf canvas{width:min(100%,850px);height:auto;background:#fff;"
              "box-shadow:0 2px 12px var(--shadow)}"
              ".pagecount,.pdferr{color:var(--muted);font-size:.8em}"
              "</style><div class=\"pdf\" id=\"pdf\"></div><script>\n");

    sb_append(&s, "var PDFJS_B64=\"");
    sb_append_base64(&s, ASSET_PDFJS_JS, ASSET_PDFJS_JS_len);
    sb_append(&s, "\";\nvar WORKER_B64=\"");
    sb_append_base64(&s, ASSET_PDFJS_WORKER_JS, ASSET_PDFJS_WORKER_JS_len);
    sb_append(&s, "\";\nvar PDF_B64=\"");
    sb_append_base64(&s, src->data, src->len);
    sb_append(&s, "\";\n");

    sb_append(
        &s,
        "function b64bytes(b){var s=atob(b),n=s.length,a=new Uint8Array(n);"
        "for(var i=0;i<n;i++)a[i]=s.charCodeAt(i);return a;}\n"
        "function blobUrl(bytes){return URL.createObjectURL("
        "new Blob([bytes],{type:'application/javascript'}));}\n"
        "var box=document.getElementById('pdf');\n"
        "function fail(m){var d=document.createElement('p');d.className="
        "'pdferr';d.textContent='Could not render PDF: '+m;"
        "box.appendChild(d);}\n"
        "var lib=document.createElement('script');\n"
        "lib.src=blobUrl(b64bytes(PDFJS_B64));\n"
        "lib.onload=function(){\n"
        "  pdfjsLib.GlobalWorkerOptions.workerSrc="
        "blobUrl(b64bytes(WORKER_B64));\n"
        "  pdfjsLib.getDocument({data:b64bytes(PDF_B64)}).promise.then("
        "function(pdf){\n"
        "    var n=Math.min(pdf.numPages,200),chain=Promise.resolve();\n"
        "    for(var p=1;p<=n;p++){(function(p){chain=chain.then(function(){\n"
        "      return pdf.getPage(p).then(function(page){\n"
        "        var vp=page.getViewport({scale:2});\n"
        "        var c=document.createElement('canvas');\n"
        "        c.width=vp.width;c.height=vp.height;box.appendChild(c);\n"
        "        return page.render({canvasContext:c.getContext('2d'),"
        "viewport:vp}).promise;\n"
        "      });});})(p);}\n"
        "    chain.then(function(){var d=document.createElement('p');"
        "d.className='pagecount';var t=pdf.numPages+' page'+"
        "(pdf.numPages===1?'':'s');d.textContent=t;box.appendChild(d);"
        "document.title+=' \\u2014 '+t;}).catch(function(e){fail(''+e);});\n"
        "  }).catch(function(e){fail(''+e);});\n"
        "};\n"
        "lib.onerror=function(){fail('could not load pdf.js');};\n"
        "document.head.appendChild(lib);\n"
        "</script>");
    page_end(&s, 0);
    return sb_take(&s);
}

char *convert_pdf(const source_file *src) { return pdf_render_inproc(src); }

#else

#include <mupdf/fitz.h>

/* 2x nominal 72dpi so pages stay sharp on hidpi displays; CSS scales the
 * <img> back down to page width. */
#define PDF_RENDER_SCALE 2.0f
/* Keep the generated document bounded for very long PDFs. */
#define PDF_MAX_PAGES 200
/* CPU seconds the isolated renderer may consume before it is killed. */
#define PDF_CPU_LIMIT 30

char *pdf_render_inproc(const source_file *src) {
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

/* Render a PDF in a throwaway child process. mupdf parses attacker-
 * controlled input; running it here means a memory-corruption bug can at
 * worst crash the child, which the parent turns into an error page, and
 * the child carries a CPU limit against runaway documents. The child
 * re-execs this binary in a hidden worker mode (see main.c), giving it a
 * clean address space rather than a fork of the GUI process. */
char *convert_pdf(const source_file *src) {
    const char *base = path_basename(src->path);

    /* No known self path (e.g. odd argv[0]) or explicit opt-out: fall
     * back to rendering in-process. */
    if (!preview_self || getenv("PREVIEW_PDF_NOSANDBOX"))
        return pdf_render_inproc(src);

    int fds[2];
    if (pipe(fds) != 0)
        return pdf_render_inproc(src);

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return pdf_render_inproc(src);
    }

    if (pid == 0) { /* child: become the PDF worker */
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        struct rlimit cpu = {PDF_CPU_LIMIT, PDF_CPU_LIMIT + 1};
        setrlimit(RLIMIT_CPU, &cpu);
        execl(preview_self, preview_self, "--render-pdf-worker", src->path,
              (char *)NULL);
        _exit(127); /* exec failed */
    }

    /* parent: drain the worker's HTML, then reap it */
    close(fds[1]);
    sb out;
    sb_init(&out);
    char buf[65536];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        sb_append_n(&out, buf, (size_t)n);
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok && out.len > 0)
        return sb_take(&out);

    sb_free(&out);
    if (WIFSIGNALED(status))
        return page_error(base, "Could not render PDF",
                          "the isolated renderer was terminated "
                          "(possibly a malformed or malicious file)");
    return page_error(base, "Could not render PDF",
                      "the isolated renderer exited unexpectedly");
}

#endif /* PREVIEW_NO_PDF */
