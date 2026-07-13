/* The webview library is header-only C++ exposing a C API. This is the one
 * C++ translation unit in the project; it emits the real symbols the C code
 * links against. WEBVIEW_STATIC makes WEBVIEW_API 'extern' instead of
 * 'inline' so the definitions aren't discarded. */
#define WEBVIEW_STATIC
#include "webview/webview.h"
