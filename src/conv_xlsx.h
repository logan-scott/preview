#ifndef PREVIEW_CONV_XLSX_H
#define PREVIEW_CONV_XLSX_H

#include "convert.h"

/* XLSX → HTML: one table per sheet, shared strings resolved, cached
 * formula values shown. No number formatting or styles. */
char *convert_xlsx(const source_file *src);

#endif
