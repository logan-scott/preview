#include "conv_dir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "detect.h"
#include "page.h"
#include "str.h"

#if defined(_WIN32)
static int abspath(const char *in, char *out, size_t cap) {
    return _fullpath(out, in, cap) != NULL;
}
#else
static int abspath(const char *in, char *out, size_t cap) {
    (void)cap;
    return realpath(in, out) != NULL;
}
#endif

typedef struct {
    char *name;
    int is_dir;
    long long size;
} dir_entry;

static int entry_cmp(const void *a, const void *b) {
    const dir_entry *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir; /* directories first */
    return strcmp(x->name, y->name);
}

/* Join dir + "/" + name into a fresh string. */
static char *join_path(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    int slash = dl > 0 && dir[dl - 1] == '/';
    size_t n = dl + (slash ? 0 : 1) + strlen(name) + 1;
    char *p = malloc(n);
    snprintf(p, n, "%s%s%s", dir, slash ? "" : "/", name);
    return p;
}

static void human_size(long long b, char *out, size_t cap) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    if (i == 0)
        snprintf(out, cap, "%lld B", b);
    else
        snprintf(out, cap, "%.1f %s", v, u[i]);
}

char *convert_directory(const char *path) {
    char absdir[4096];
    if (!abspath(path, absdir, sizeof(absdir)))
        snprintf(absdir, sizeof(absdir), "%s", path);

    DIR *d = opendir(absdir);
    if (!d)
        return NULL;

    dir_entry *entries = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (de->d_name[0] == '.') /* skip hidden */
            continue;
        char *full = join_path(absdir, de->d_name);
        struct stat st;
        int is_dir = 0;
        long long size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (long long)st.st_size;
        }
        free(full);
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            entries = realloc(entries, cap * sizeof(dir_entry));
        }
        entries[n].name = strdup(de->d_name);
        entries[n].is_dir = is_dir;
        entries[n].size = size;
        n++;
    }
    closedir(d);
    qsort(entries, n, sizeof(dir_entry), entry_cmp);

    sb s;
    sb_init(&s);
    page_begin(&s, path_basename(absdir[0] ? absdir : "/"), 0);
    sb_append(&s,
              "<style>.dir{max-width:820px;margin:0 auto;padding:24px 20px}"
              ".dir h1{font-size:1.3em;word-break:break-all}"
              ".dir a{display:flex;justify-content:space-between;gap:12px;"
              "padding:8px 12px;border-radius:6px;text-decoration:none;"
              "color:var(--fg);border-bottom:1px solid var(--border)}"
              ".dir a:hover{background:var(--code-bg)}"
              ".dir .nm{overflow:hidden;text-overflow:ellipsis;"
              "white-space:nowrap}"
              ".dir .meta{color:var(--muted);font-size:.85em;flex:none}"
              ".dir .up{color:var(--accent)}"
              "</style><div class=\"dir\"><h1>");
    sb_append_html(&s, absdir, strlen(absdir));
    sb_append(&s, "</h1>");

    /* parent directory ("..") unless we're at the root */
    if (strcmp(absdir, "/") != 0) {
        char *slash = strrchr(absdir, '/');
        char parent[4096];
        if (slash && slash != absdir)
            snprintf(parent, sizeof(parent), "%.*s",
                     (int)(slash - absdir), absdir);
        else
            snprintf(parent, sizeof(parent), "/");
        sb_append(&s, "<a class=\"entry up\" href=\"#\" data-path=\"");
        sb_append_html(&s, parent, strlen(parent));
        sb_append(&s, "\"><span class=\"nm\">../</span>"
                      "<span class=\"meta\">parent</span></a>");
    }

    for (size_t i = 0; i < n; i++) {
        char *full = join_path(absdir, entries[i].name);
        sb_append(&s, "<a class=\"entry\" href=\"#\" data-path=\"");
        sb_append_html(&s, full, strlen(full));
        sb_append(&s, "\"><span class=\"nm\">");
        sb_append_html(&s, entries[i].name, strlen(entries[i].name));
        if (entries[i].is_dir)
            sb_append(&s, "/");
        sb_append(&s, "</span><span class=\"meta\">");
        if (entries[i].is_dir) {
            sb_append(&s, "folder");
        } else {
            char sz[32];
            human_size(entries[i].size, sz, sizeof(sz));
            sb_append(&s, sz);
        }
        sb_append(&s, "</span></a>");
        free(full);
        free(entries[i].name);
    }
    free(entries);

    sb_append(&s, "</div><script>"
                  "document.querySelectorAll('.entry').forEach(function(a){"
                  "a.addEventListener('click',function(e){e.preventDefault();"
                  "if(window.previewOpen)previewOpen(a.dataset.path);});});"
                  "</script>");
    page_end(&s, 0);
    return sb_take(&s);
}
