#ifndef PREVIEW_CONV_DIR_H
#define PREVIEW_CONV_DIR_H

/* Render a directory as a browsable listing. Entries are links that call
 * the previewOpen() binding (wired in main.c) to re-render the window with
 * the chosen file or subdirectory. Returns malloc'd HTML (caller frees),
 * or NULL if the directory can't be read. */
char *convert_directory(const char *path);

#endif
