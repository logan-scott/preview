#include "detect.h"

#include <ctype.h>
#include <string.h>

const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

const char *path_ext(const char *path) {
    static char buf[16];
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || dot[1] == '\0')
        return "";
    size_t n = strlen(dot + 1);
    if (n >= sizeof(buf))
        return "";
    for (size_t i = 0; i <= n; i++)
        buf[i] = (char)tolower((unsigned char)dot[1 + i]);
    return buf;
}

/* --- extension table ------------------------------------------------- */

typedef struct {
    const char *ext;
    filetype type;
} ext_map;

static const ext_map EXT_TABLE[] = {
    {"md", FT_MARKDOWN},   {"markdown", FT_MARKDOWN}, {"mdown", FT_MARKDOWN},
    {"mkd", FT_MARKDOWN},

    {"json", FT_JSON},     {"jsonl", FT_JSON},        {"geojson", FT_JSON},

    {"csv", FT_CSV},       {"tsv", FT_CSV},

    {"png", FT_IMAGE},     {"jpg", FT_IMAGE},         {"jpeg", FT_IMAGE},
    {"gif", FT_IMAGE},     {"webp", FT_IMAGE},        {"bmp", FT_IMAGE},
    {"svg", FT_IMAGE},     {"ico", FT_IMAGE},         {"avif", FT_IMAGE},

    {"tga", FT_IMAGE_STB}, {"psd", FT_IMAGE_STB},     {"hdr", FT_IMAGE_STB},
    {"pnm", FT_IMAGE_STB}, {"ppm", FT_IMAGE_STB},     {"pgm", FT_IMAGE_STB},
    {"pic", FT_IMAGE_STB},

    {"html", FT_HTML},     {"htm", FT_HTML},          {"xhtml", FT_HTML},

    {"pdf", FT_PDF},
    {"docx", FT_DOCX},
    {"pptx", FT_PPTX},
    {"xlsx", FT_XLSX},

    /* common text/source extensions; anything else falls through to the
     * printable-bytes heuristic, so this list is a fast path, not a gate */
    {"txt", FT_TEXT},      {"log", FT_TEXT},          {"c", FT_TEXT},
    {"h", FT_TEXT},        {"cc", FT_TEXT},           {"cpp", FT_TEXT},
    {"hpp", FT_TEXT},      {"py", FT_TEXT},           {"rb", FT_TEXT},
    {"rs", FT_TEXT},       {"go", FT_TEXT},           {"js", FT_TEXT},
    {"ts", FT_TEXT},       {"jsx", FT_TEXT},          {"tsx", FT_TEXT},
    {"java", FT_TEXT},     {"kt", FT_TEXT},           {"swift", FT_TEXT},
    {"sh", FT_TEXT},       {"bash", FT_TEXT},         {"zsh", FT_TEXT},
    {"yaml", FT_TEXT},     {"yml", FT_TEXT},          {"toml", FT_TEXT},
    {"ini", FT_TEXT},      {"cfg", FT_TEXT},          {"conf", FT_TEXT},
    {"xml", FT_TEXT},      {"sql", FT_TEXT},          {"diff", FT_TEXT},
    {"patch", FT_TEXT},    {"tex", FT_TEXT},          {"m", FT_TEXT},
    {"mm", FT_TEXT},       {"lua", FT_TEXT},          {"pl", FT_TEXT},
    {"php", FT_TEXT},      {"cs", FT_TEXT},           {"scala", FT_TEXT},
    {"hs", FT_TEXT},       {"el", FT_TEXT},           {"vim", FT_TEXT},
    {"zig", FT_TEXT},      {"d", FT_TEXT},            {"r", FT_TEXT},
    {"mk", FT_TEXT},       {"cmake", FT_TEXT},        {"gradle", FT_TEXT},
};

static filetype type_from_ext(const char *ext) {
    if (!ext[0])
        return FT_UNKNOWN;
    for (size_t i = 0; i < sizeof(EXT_TABLE) / sizeof(EXT_TABLE[0]); i++)
        if (strcmp(ext, EXT_TABLE[i].ext) == 0)
            return EXT_TABLE[i].type;
    return FT_UNKNOWN;
}

/* --- magic bytes ------------------------------------------------------ */

static int starts_with(const uint8_t *data, size_t len, const char *magic,
                       size_t magic_len) {
    return len >= magic_len && memcmp(data, magic, magic_len) == 0;
}

/* Look inside a ZIP for OOXML markers. A full parse needs miniz; the
 * central directory stores path strings verbatim, so a byte scan for the
 * signature entry names is a reliable, dependency-free tell. */
static filetype sniff_zip(const uint8_t *data, size_t len) {
    /* scan the tail (central directory lives at the end) plus the head */
    struct { const char *needle; filetype type; } marks[] = {
        {"word/document.xml", FT_DOCX},
        {"ppt/presentation.xml", FT_PPTX},
        {"xl/workbook.xml", FT_XLSX},
    };
    for (size_t m = 0; m < 3; m++) {
        size_t nlen = strlen(marks[m].needle);
        if (len < nlen)
            continue;
        for (size_t i = 0; i + nlen <= len; i++)
            if (memcmp(data + i, marks[m].needle, nlen) == 0)
                return marks[m].type;
    }
    return FT_UNKNOWN;
}

static filetype type_from_magic(const uint8_t *d, size_t n) {
    if (starts_with(d, n, "%PDF-", 5))
        return FT_PDF;
    if (starts_with(d, n, "\x89PNG\r\n\x1a\n", 8))
        return FT_IMAGE;
    if (starts_with(d, n, "\xff\xd8\xff", 3))
        return FT_IMAGE; /* JPEG */
    if (starts_with(d, n, "GIF87a", 6) || starts_with(d, n, "GIF89a", 6))
        return FT_IMAGE;
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0)
        return FT_IMAGE;
    if (starts_with(d, n, "BM", 2) && n > 26)
        return FT_IMAGE;
    if (starts_with(d, n, "PK\x03\x04", 4))
        return sniff_zip(d, n);
    return FT_UNKNOWN;
}

/* --- text heuristic ---------------------------------------------------- */

static int looks_like_text(const uint8_t *d, size_t n) {
    size_t check = n < 8192 ? n : 8192;
    size_t suspicious = 0;
    for (size_t i = 0; i < check; i++) {
        uint8_t c = d[i];
        if (c == 0)
            return 0;
        /* control chars other than \t \n \r \f \e */
        if (c < 32 && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
            c != 27)
            suspicious++;
    }
    return check == 0 || suspicious * 20 < check; /* <5% odd bytes */
}

filetype detect_filetype(const char *path, const uint8_t *data, size_t len) {
    filetype t = type_from_ext(path_ext(path));

    /* Extension wins, except when magic bytes flatly contradict it for a
     * container format (e.g. a "foo.md" that is actually a PDF). */
    filetype magic = type_from_magic(data, len);

    if (t == FT_UNKNOWN)
        t = magic;

    if (t == FT_UNKNOWN)
        t = looks_like_text(data, len) ? FT_TEXT : FT_BINARY;

    /* A text-family claim on binary content: trust the magic/binary check */
    if ((t == FT_TEXT || t == FT_MARKDOWN || t == FT_JSON || t == FT_CSV) &&
        !looks_like_text(data, len))
        t = magic != FT_UNKNOWN ? magic : FT_BINARY;

    return t;
}

const char *filetype_name(filetype t) {
    switch (t) {
    case FT_TEXT:      return "text";
    case FT_MARKDOWN:  return "markdown";
    case FT_JSON:      return "json";
    case FT_CSV:       return "csv";
    case FT_IMAGE:     return "image";
    case FT_IMAGE_STB: return "image";
    case FT_HTML:      return "html";
    case FT_PDF:       return "pdf";
    case FT_DOCX:      return "docx";
    case FT_PPTX:      return "pptx";
    case FT_XLSX:      return "xlsx";
    case FT_BINARY:    return "binary";
    default:           return "unknown";
    }
}
