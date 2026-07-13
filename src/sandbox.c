#include "sandbox.h"

/* ---- macOS: Seatbelt (sandbox_init) -------------------------------------- */
#if defined(__APPLE__)

#include <stdlib.h>

/* sandbox_init is deprecated but remains functional for CLI tools and is
 * the only in-process option without app entitlements. */
extern int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
extern void sandbox_free_error(char *errorbuf);

int sandbox_restrict(void) {
    /* Allow the compute and file reads mupdf needs (fonts, dylibs), deny
     * the two things an exploit wants: the network and launching other
     * programs. */
    static const char profile[] =
        "(version 1)\n"
        "(allow default)\n"
        "(deny network*)\n"
        "(deny process-exec*)\n"
        "(deny process-fork)\n";
    char *err = NULL;
    if (sandbox_init(profile, 0, &err) == 0)
        return 1;
    if (err)
        sandbox_free_error(err);
    return 0;
}

/* ---- Linux: seccomp-BPF --------------------------------------------------- */
#elif defined(__linux__)

#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#if defined(__x86_64__)
#define PREVIEW_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define PREVIEW_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

int sandbox_restrict(void) {
#ifndef PREVIEW_AUDIT_ARCH
    return 0; /* unknown architecture: no filter */
#else
    /* Deny the syscalls an exploit would use to escalate beyond this
     * process: run a program, or open a socket. Everything else — mmap,
     * file reads for fonts, futex for any threading — is allowed, so
     * rendering is unaffected. Denied calls terminate the process. */
    struct sock_filter filter[] = {
        /* reject a mismatched architecture outright */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PREVIEW_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

        /* load the syscall number */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, nr)),

#define DENY(n)                                                              \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (n), 0, 1),                          \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)

        DENY(__NR_execve),
        DENY(__NR_execveat),
        DENY(__NR_socket),
        DENY(__NR_connect),
        DENY(__NR_ptrace),
#ifdef __NR_fork
        DENY(__NR_fork),
#endif
#ifdef __NR_vfork
        DENY(__NR_vfork),
#endif
#undef DENY

        /* default: allow */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return 0;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0)
        return 0;
    return 1;
#endif
}

/* ---- other platforms ------------------------------------------------------ */
#else

int sandbox_restrict(void) { return 0; }

#endif
