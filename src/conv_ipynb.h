#ifndef PREVIEW_CONV_IPYNB_H
#define PREVIEW_CONV_IPYNB_H

#include "convert.h"

/* Jupyter notebook (.ipynb) → HTML: markdown cells rendered as markdown,
 * code cells syntax-highlighted, and outputs (stream text, text/plain,
 * images, and error tracebacks) shown beneath each cell. */
char *convert_ipynb(const source_file *src);

#endif
