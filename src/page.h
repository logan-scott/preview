#ifndef PREVIEW_PAGE_H
#define PREVIEW_PAGE_H

#include "str.h"

/* page_begin/page_end wrap converter output in a complete HTML document
 * with the shared stylesheet (light + dark via prefers-color-scheme). */
enum {
    PAGE_HLJS = 1 << 0, /* include highlight.js + theme + init script */
};

void page_begin(sb *s, const char *title, unsigned flags);
void page_end(sb *s, unsigned flags);

/* Complete error document (used for corrupt/unsupported files). */
char *page_error(const char *title, const char *message, const char *detail);

#endif
