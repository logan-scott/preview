#ifndef PREVIEW_CONV_PDF_H
#define PREVIEW_CONV_PDF_H

#include "convert.h"

/* PDF → HTML with one PNG <img> per page. Compiled against mupdf unless
 * PREVIEW_NO_PDF is defined, in which case it returns an error page. */
char *convert_pdf(const source_file *src);

#endif
