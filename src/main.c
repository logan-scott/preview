#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WEBVIEW_HEADER
#include "webview/webview.h"

#include "conv_pdf.h"
#include "convert.h"
#include "detect.h"
#include "page.h"
#include "sandbox.h"
#include "str.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#define PREVIEW_VERSION "0.2.1"

static void usage(FILE *out) {
    fprintf(out,
            "usage: preview [options] <file>\n"
            "\n"
            "Open <file> in a native viewer window. The file type is\n"
            "detected automatically and rendered as HTML.\n"
            "\n"
            "Supported types:\n"
            "  Markdown, images (png/jpg/gif/webp/bmp/svg/...), plain text\n"
            "  and source code, JSON, CSV/TSV, HTML, PDF, DOCX, XLSX, PPTX.\n"
            "\n"
            "Options:\n"
            "  -h, --help          show this help and exit\n"
            "  -V, --version       print version and exit\n"
            "  -w, --watch         re-render when the file changes on disk\n"
            "  --no-remote         block remote images (offline; no "
            "tracking beacons)\n"
            "  --dump-html         write the generated HTML to stdout and\n"
            "                      exit (no window)\n"
            "  --close-after <ms>  auto-close the window after <ms> "
            "(for testing)\n"
            "\n"
            "In the window: Esc closes; arrows, space, and page keys "
            "scroll.\n"
            "The color scheme follows the OS light/dark setting.\n"
            "\n"
            "Examples:\n"
            "  preview report.docx\n"
            "  preview --watch notes.md\n"
            "  preview --dump-html data.csv > data.html\n");
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

/* --- rendering + optional file watching -------------------------------- */

typedef struct {
    char *html; /* converted HTML (owned), or */
    char *url;  /* file:// url for a raw .html file (owned) */
} render_result;

/* Read and render a file. Returns both fields NULL if the file cannot be
 * read (e.g. mid-save); the converters themselves never fail — corrupt
 * content yields an error page. */
static render_result build_render(const char *file) {
    render_result r = {NULL, NULL};
    size_t len = 0;
    const char *err = NULL;
    uint8_t *data = read_entire_file(file, &len, &err);
    if (!data)
        return r;
    filetype ft = detect_filetype(file, data, len);
    if (ft == FT_HTML) {
        char real[4096];
        if (realpath(file, real)) {
            sb u;
            sb_init(&u);
            sb_append(&u, "file://");
            sb_append(&u, real);
            r.url = sb_take(&u);
        }
    }
    if (!r.url) {
        source_file src = {file, data, len, ft};
        r.html = convert_to_html(&src);
    }
    free(data);
    return r;
}

/* Push a freshly built render into the live window (runs on the UI
 * thread via webview_dispatch). Takes ownership of the heap arg. */
static void apply_render(webview_t w, void *arg) {
    render_result *r = arg;
    if (r->url)
        webview_navigate(w, r->url);
    else if (r->html)
        webview_set_html(w, r->html);
    free(r->html);
    free(r->url);
    free(r);
}

typedef struct {
    webview_t w;
    const char *file;
    volatile int *running;
} watch_args;

static void *watch_thread(void *p) {
    watch_args *a = p;
    struct stat st;
    time_t last = 0;
    if (stat(a->file, &st) == 0)
        last = st.st_mtime;
    while (*a->running) {
        struct timespec ts = {0, 250 * 1000 * 1000}; /* 250 ms poll */
        nanosleep(&ts, NULL);
        if (!*a->running)
            break;
        if (stat(a->file, &st) != 0 || st.st_mtime == last)
            continue;
        last = st.st_mtime;
        render_result rr = build_render(a->file);
        if ((rr.html || rr.url) && *a->running) {
            render_result *heap = malloc(sizeof(*heap));
            *heap = rr;
            webview_dispatch(a->w, apply_render, heap);
        } else {
            free(rr.html);
            free(rr.url);
        }
    }
    return NULL;
}

/* Resolve the absolute path to this executable so the PDF worker can be
 * re-exec'd reliably regardless of how preview was invoked. */
static int resolve_self(char *buf, size_t cap) {
#if defined(__APPLE__)
    (void)cap;
    char raw[4096];
    uint32_t sz = sizeof(raw);
    if (_NSGetExecutablePath(raw, &sz) != 0)
        return 0;
    return realpath(raw, buf) != NULL;
#else
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return 1;
#endif
}

/* Hidden worker: render a single PDF to stdout, then exit. Invoked by
 * convert_pdf() in a sandboxed child process. */
static int pdf_worker(const char *path) {
    size_t len = 0;
    const char *err = NULL;
    uint8_t *data = read_entire_file(path, &len, &err);
    if (!data)
        return 1;
    /* Confine the syscalls available before handing bytes to mupdf. */
    sandbox_restrict();
    source_file src = {path, data, len, FT_PDF};
    char *html = pdf_render_inproc(&src);
    if (html)
        fputs(html, stdout);
    free(html);
    free(data);
    return 0;
}

/* Hidden self-test: apply the sandbox, then attempt to exec a program —
 * the capability an exploit most wants and one both backends forbid. A
 * working sandbox kills this process (Linux seccomp) or makes execve fail
 * (macOS Seatbelt). If exec instead succeeds, this process is replaced by
 * /usr/bin/true and exits 0, which the caller reads as "not blocked".
 *   exit 2  no sandbox available (skip)
 *   exit 4  exec blocked with an error
 *   killed  exec blocked by termination (seccomp)
 *   exit 0  NOT blocked (failure) */
static int sandbox_selftest(void) {
    if (!sandbox_restrict()) {
        fprintf(stderr, "sandbox: unavailable on this platform\n");
        return 2;
    }
    fprintf(stderr, "sandbox: applied\n"); /* an allowed syscall (write) */
    execl("/usr/bin/true", "true", (char *)NULL);
    execl("/bin/true", "true", (char *)NULL);
    return 4; /* exec returned: blocked with an error */
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--render-pdf-worker") == 0)
        return pdf_worker(argv[2]);
    if (argc == 2 && strcmp(argv[1], "--sandbox-selftest") == 0)
        return sandbox_selftest();

    static char self_path[4096];
    if (resolve_self(self_path, sizeof(self_path)))
        preview_self = self_path;

    const char *file = NULL;
    int dump_html = 0;
    int watch = 0;
    int close_after = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("preview %s\n", PREVIEW_VERSION);
            return 0;
        } else if (strcmp(a, "-w") == 0 || strcmp(a, "--watch") == 0) {
            watch = 1;
        } else if (strcmp(a, "--no-remote") == 0) {
            page_offline = 1;
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

    /* --dump-html: raw bytes for HTML, converted HTML otherwise. */
    if (dump_html) {
        size_t len = 0;
        const char *err = NULL;
        uint8_t *data = read_entire_file(file, &len, &err);
        if (!data) {
            fprintf(stderr, "preview: %s: %s\n", file, err);
            return 1;
        }
        filetype ft = detect_filetype(file, data, len);
        if (ft == FT_HTML) {
            fwrite(data, 1, len, stdout);
        } else {
            source_file src = {file, data, len, ft};
            char *html = convert_to_html(&src);
            fputs(html, stdout);
            free(html);
        }
        free(data);
        return 0;
    }

    render_result rr = build_render(file);
    if (!rr.html && !rr.url) {
        fprintf(stderr, "preview: %s: cannot read file\n", file);
        return 1;
    }

    webview_t w = webview_create(0, NULL);
    if (!w) {
        fprintf(stderr, "preview: failed to create window\n");
        free(rr.html);
        free(rr.url);
        return 1;
    }
    webview_set_title(w, path_basename(file));
    webview_set_size(w, 960, 720, WEBVIEW_HINT_NONE);
    webview_bind(w, "previewQuit", on_quit, w);
    /* Esc closes; space/arrows scroll natively. Runs on every page load. */
    webview_init(w,
                 "window.addEventListener('keydown',e=>{"
                 "if(e.key==='Escape'){previewQuit();}});");
    if (rr.url)
        webview_navigate(w, rr.url);
    else
        webview_set_html(w, rr.html);

    pthread_t closer, watcher;
    closer_args ca = {w, close_after};
    if (close_after >= 0)
        pthread_create(&closer, NULL, closer_thread, &ca);

    volatile int running = 1;
    watch_args wa = {w, file, &running};
    if (watch)
        pthread_create(&watcher, NULL, watch_thread, &wa);

    webview_run(w);

    running = 0;
    if (watch)
        pthread_join(watcher, NULL);
    if (close_after >= 0)
        pthread_join(closer, NULL);
    webview_destroy(w);
    free(rr.html);
    free(rr.url);
    return 0;
}
