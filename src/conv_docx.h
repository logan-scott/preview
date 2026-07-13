#ifndef PREVIEW_CONV_DOCX_H
#define PREVIEW_CONV_DOCX_H

#include "convert.h"

/* DOCX → HTML: paragraphs, headings, bold/italic/underline/strike,
 * super/subscript, hyperlinks, bullet & numbered lists (incl. nesting),
 * tables (with colspan), and inline images. */
char *convert_docx(const source_file *src);

#endif
