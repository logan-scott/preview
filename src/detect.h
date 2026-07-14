#ifndef PREVIEW_DETECT_H
#define PREVIEW_DETECT_H

#include <stddef.h>
#include <stdint.h>

/* Every renderable file type. Add a value here, a row to the tables in
 * detect.c, and a converter in convert.c to support a new format. */
typedef enum {
    FT_UNKNOWN = 0,
    FT_TEXT,      /* plain text and source code */
    FT_MARKDOWN,
    FT_JSON,
    FT_IPYNB, /* Jupyter notebook (JSON) */
    FT_CSV,
    FT_IMAGE,     /* formats the browser engine decodes natively */
    FT_IMAGE_STB, /* formats needing stb_image (tga, psd, hdr, pnm...) */
    FT_HTML,      /* rendered directly by the webview */
    FT_PDF,
    FT_DOCX,
    FT_PPTX,
    FT_XLSX,
    FT_ODF,       /* OpenDocument: odt / ods / odp */
    FT_BINARY,    /* unrenderable */
} filetype;

filetype detect_filetype(const char *path, const uint8_t *data, size_t len);
const char *filetype_name(filetype t);

/* "/a/b/c.txt" -> "c.txt" */
const char *path_basename(const char *path);
/* Lowercased extension without the dot ("tar.gz" -> "gz"); "" if none.
 * Returns a pointer to a static buffer. */
const char *path_ext(const char *path);

#endif
