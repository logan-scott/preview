#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WEBVIEW_HEADER
#include "webview/webview.h"

#include "convert.h"
#include "detect.h"
#include "str.h"

#define PREVIEW_VERSION "0.1.0"

static void usage(FILE *out) {
    fprintf(out,
            "usage: preview [options] <file>\n"
            "\n"
            "Open <file> in a native viewer window.\n"
            "\n"
            "options:\n"
            "  -h, --help          show this help\n"
            "  -V, --version       print version\n"
            "  --dump-html         print the generated HTML to stdout "
            "instead of opening a window\n"
            "  --close-after <ms>  auto-close the window (for testing)\n");
}

/* --- auto-close support (testing) -------------------------------------- */

typedef struct {
    webview_t w;
    int ms;
} closer_args;

static void do_terminate(webview_t w, void *arg) {
    (void)arg;
    webview_terminate(w);
}

/* JS binding: window.previewQuit() — wired to Esc below */
static void on_quit(const char *id, const char *req, void *arg) {
    (void)id;
    (void)req;
    webview_terminate((webview_t)arg);
}

static void *closer_thread(void *p) {
    closer_args *a = p;
    struct timespec ts = {a->ms / 1000, (long)(a->ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    webview_dispatch(a->w, do_terminate, NULL);
    return NULL;
}

int main(int argc, char **argv) {
    const char *file = NULL;
    int dump_html = 0;
    int close_after = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("preview %s\n", PREVIEW_VERSION);
            return 0;
        } else if (strcmp(a, "--dump-html") == 0) {
            dump_html = 1;
        } else if (strcmp(a, "--close-after") == 0 && i + 1 < argc) {
            close_after = atoi(argv[++i]);
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "preview: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        } else if (!file) {
            file = a;
        } else {
            fprintf(stderr, "preview: only one file at a time\n");
            return 2;
        }
    }
    if (!file) {
        usage(stderr);
        return 2;
    }

    size_t len = 0;
    const char *err = NULL;
    uint8_t *data = read_entire_file(file, &len, &err);
    if (!data) {
        fprintf(stderr, "preview: %s: %s\n", file, err);
        return 1;
    }

    filetype ft = detect_filetype(file, data, len);

    /* HTML renders natively in the webview; navigate to it directly so
     * relative subresources keep working. */
    char *navigate_url = NULL;
    char *html = NULL;
    if (ft == FT_HTML) {
        if (dump_html) {
            fwrite(data, 1, len, stdout);
            free(data);
            return 0;
        }
        char real[4096];
        if (!realpath(file, real)) {
            fprintf(stderr, "preview: cannot resolve path: %s\n", file);
            free(data);
            return 1;
        }
        sb u;
        sb_init(&u);
        sb_append(&u, "file://");
        sb_append(&u, real);
        navigate_url = sb_take(&u);
    } else {
        source_file src = {file, data, len, ft};
        html = convert_to_html(&src);
    }
    free(data);

    if (dump_html) {
        fputs(html, stdout);
        free(html);
        return 0;
    }

    webview_t w = webview_create(0, NULL);
    if (!w) {
        fprintf(stderr, "preview: failed to create window\n");
        free(html);
        free(navigate_url);
        return 1;
    }
    webview_set_title(w, path_basename(file));
    webview_set_size(w, 960, 720, WEBVIEW_HINT_NONE);
    webview_bind(w, "previewQuit", on_quit, w);
    /* Esc closes; space/arrows scroll natively. Runs on every page load. */
    webview_init(w,
                 "window.addEventListener('keydown',e=>{"
                 "if(e.key==='Escape'){previewQuit();}});");
    if (navigate_url)
        webview_navigate(w, navigate_url);
    else
        webview_set_html(w, html);

    pthread_t closer;
    closer_args ca = {w, close_after};
    if (close_after >= 0)
        pthread_create(&closer, NULL, closer_thread, &ca);

    webview_run(w);

    if (close_after >= 0)
        pthread_join(closer, NULL);
    webview_destroy(w);
    free(html);
    free(navigate_url);
    return 0;
}
