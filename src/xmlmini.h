#ifndef PREVIEW_XMLMINI_H
#define PREVIEW_XMLMINI_H

#include <stddef.h>

#include "str.h"

/* Minimal non-validating XML pull parser — just enough for the
 * well-formed, namespace-prefixed XML inside OOXML packages.
 * Element names are reported verbatim including prefix ("w:p"). */

typedef enum {
    XML_EOF = 0,
    XML_ELEM_START, /* <w:p ...>  */
    XML_ELEM_EMPTY, /* <w:br/>    */
    XML_ELEM_END,   /* </w:p>     */
    XML_TEXT,       /* character data (entities not yet decoded) */
} xml_event;

typedef struct {
    const char *p, *end;
    /* valid after XML_ELEM_START/EMPTY/END: */
    char name[80];
    /* raw attribute region, valid after START/EMPTY: */
    const char *attrs;
    size_t attrs_len;
    /* valid after XML_TEXT: */
    const char *text;
    size_t text_len;
} xml_reader;

void xml_init(xml_reader *x, const char *data, size_t len);
xml_event xml_next(xml_reader *x);

/* Attribute lookup on the current start tag. Matches the name with or
 * without a namespace prefix ("id" matches r:id). Returns a malloc'd,
 * entity-decoded string, or NULL. */
char *xml_attr(const xml_reader *x, const char *name);

/* Append s[0..n) to out with XML entities decoded. */
void xml_decode_into(sb *out, const char *s, size_t n);

#endif
