#ifndef PREVIEW_CONVERT_H
#define PREVIEW_CONVERT_H

#include "detect.h"

typedef struct {
    const char *path; /* as given on the command line */
    const uint8_t *data;
    size_t len;
    filetype type;
} source_file;

/* Convert a file to a complete HTML document (malloc'd, caller frees).
 * Never returns NULL — unsupported/corrupt input yields an error page.
 * FT_HTML is handled by the caller (navigated directly, not converted). */
char *convert_to_html(const source_file *src);

#endif
