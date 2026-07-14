#ifndef PREVIEW_CONV_RTF_H
#define PREVIEW_CONV_RTF_H

#include "convert.h"

/* RTF → HTML. A minimal reader: paragraphs, bold/italic/underline, tabs
 * and line breaks, \'hh bytes (cp1252) and \uN unicode. Font, color, and
 * style tables and other ignorable destinations are skipped. Not a
 * full-fidelity RTF renderer — enough to read a document. */
char *convert_rtf(const source_file *src);

#endif
