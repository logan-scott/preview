#ifndef PREVIEW_CONV_PPTX_H
#define PREVIEW_CONV_PPTX_H

#include "convert.h"

/* PPTX → HTML: one card per slide with title, body text lines, and
 * images. Content flows top-to-bottom; no positional layout. */
char *convert_pptx(const source_file *src);

#endif
