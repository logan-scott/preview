#ifndef PREVIEW_COMPAT_H
#define PREVIEW_COMPAT_H

/* Small cross-platform shims so main.c builds on Windows (WebView2) as
 * well as POSIX. Only the primitives preview actually needs: a background
 * thread, a millisecond sleep, and absolute-path resolution.
 *
 * This header is included by a single translation unit (main.c), so the
 * static definitions below are not duplicated across the program. */

#include <stddef.h>
#include <stdlib.h>

#if defined(_WIN32)

#include <windows.h>

typedef HANDLE preview_thread;

struct preview_thread_arg {
    void *(*fn)(void *);
    void *arg;
};

static DWORD WINAPI preview_thread_trampoline(LPVOID p) {
    struct preview_thread_arg a = *(struct preview_thread_arg *)p;
    free(p);
    a.fn(a.arg);
    return 0;
}

static int preview_thread_start(preview_thread *t, void *(*fn)(void *),
                                void *arg) {
    struct preview_thread_arg *a = malloc(sizeof(*a));
    if (!a)
        return -1;
    a->fn = fn;
    a->arg = arg;
    HANDLE h = CreateThread(NULL, 0, preview_thread_trampoline, a, 0, NULL);
    if (!h) {
        free(a);
        return -1;
    }
    *t = h;
    return 0;
}

static void preview_thread_join(preview_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static void preview_sleep_ms(long ms) { Sleep((DWORD)ms); }

static int preview_realpath(const char *in, char *out, size_t cap) {
    DWORD n = GetFullPathNameA(in, (DWORD)cap, out, NULL);
    return n > 0 && n < cap;
}

#else /* POSIX */

#include <pthread.h>
#include <time.h>

typedef pthread_t preview_thread;

static int preview_thread_start(preview_thread *t, void *(*fn)(void *),
                                void *arg) {
    return pthread_create(t, NULL, fn, arg);
}

static void preview_thread_join(preview_thread t) { pthread_join(t, NULL); }

static void preview_sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int preview_realpath(const char *in, char *out, size_t cap) {
    (void)cap;
    return realpath(in, out) != NULL;
}

#endif

#endif /* PREVIEW_COMPAT_H */
