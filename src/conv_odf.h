#ifndef PREVIEW_CONV_ODF_H
#define PREVIEW_CONV_ODF_H

#include "convert.h"

/* OpenDocument (ODT / ODS / ODP) → HTML. Reads content.xml from the ZIP
 * and maps the shared ODF text model to HTML: headings, paragraphs, spans
 * with bold/italic/underline/strike, bullet & numbered lists, tables, and
 * inline images. Works across the text/spreadsheet/presentation variants
 * since they share the same paragraph, list, and table elements. */
char *convert_odf(const source_file *src);

#endif
