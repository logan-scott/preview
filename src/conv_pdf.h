#ifndef PREVIEW_CONV_PDF_H
#define PREVIEW_CONV_PDF_H

#include "convert.h"

/* PDF → HTML with one PNG <img> per page. Compiled against mupdf unless
 * PREVIEW_NO_PDF is defined, in which case it returns an error page.
 *
 * convert_pdf() renders in an isolated child process when possible, so a
 * malformed PDF that exploits mupdf cannot corrupt the main process — a
 * crash in the child is reported as an error page. pdf_render_inproc()
 * does the actual mupdf work and is what the child (and the no-sandbox
 * fallback) calls. */
char *convert_pdf(const source_file *src);
char *pdf_render_inproc(const source_file *src);

/* Absolute path to this executable, used to re-exec the PDF worker. Set
 * once from main; NULL disables sandboxing (renders in-process instead). */
extern const char *preview_self;

#endif
