#ifndef PREVIEW_CONV_PDF_H
#define PREVIEW_CONV_PDF_H

#include "convert.h"

/* PDF → HTML.
 *
 * With mupdf (the default), each page is rendered to a PNG <img>, and
 * convert_pdf() does this in an isolated, syscall-sandboxed child process
 * so a malformed PDF that exploits mupdf cannot corrupt the main process;
 * pdf_render_inproc() does the actual mupdf work and is what the child
 * (and the no-sandbox fallback) calls.
 *
 * When built with PREVIEW_NO_PDF (no mupdf), the same functions instead
 * emit a page that renders the PDF client-side with a bundled copy of
 * pdf.js — no native dependency, no subprocess. */
char *convert_pdf(const source_file *src);
char *pdf_render_inproc(const source_file *src);

/* Absolute path to this executable, used to re-exec the PDF worker. Set
 * once from main; NULL disables sandboxing (renders in-process instead). */
extern const char *preview_self;

#endif
