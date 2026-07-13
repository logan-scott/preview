#include "ziputil.h"

void zcap_init(zcap *z, mz_zip_archive *za) {
    z->za = za;
    z->remaining = ZCAP_DEFAULT_BUDGET;
}

void *zcap_extract(zcap *z, const char *name, size_t *out_len) {
    *out_len = 0;
    mz_uint32 idx;
    if (!mz_zip_reader_locate_file_v2(z->za, name, NULL, 0, &idx))
        return NULL;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(z->za, idx, &st))
        return NULL;
    if (st.m_uncomp_size > z->remaining)
        return NULL; /* over budget: refuse rather than inflate */
    void *p = mz_zip_reader_extract_to_heap(z->za, idx, out_len, 0);
    if (p)
        z->remaining -= *out_len;
    return p;
}
