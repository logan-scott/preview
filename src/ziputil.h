#ifndef PREVIEW_ZIPUTIL_H
#define PREVIEW_ZIPUTIL_H

#include <stddef.h>

#include "miniz/miniz.h"

/* Budgeted ZIP extraction. OOXML files are attacker-controllable ZIPs, so
 * every part's declared uncompressed size is checked against a per-archive
 * budget before anything is inflated — a zip bomb fails cleanly instead of
 * exhausting memory. */
typedef struct {
    mz_zip_archive *za;
    size_t remaining; /* bytes of extraction budget left */
} zcap;

/* 512 MB total per archive: far above any real document, far below harm. */
#define ZCAP_DEFAULT_BUDGET ((size_t)512 * 1024 * 1024)

void zcap_init(zcap *z, mz_zip_archive *za);

/* Extract a part to heap (caller frees with mz_free). Returns NULL if the
 * part is missing or its declared size would exceed the budget. */
void *zcap_extract(zcap *z, const char *name, size_t *out_len);

#endif
