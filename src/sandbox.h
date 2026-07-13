#ifndef PREVIEW_SANDBOX_H
#define PREVIEW_SANDBOX_H

/* Best-effort syscall confinement for the current process, applied by the
 * PDF render worker before it touches attacker-controlled data. It blocks
 * the operations a memory-corruption exploit in mupdf would need to do
 * real damage — spawning programs and opening network connections — while
 * still allowing the computation and file reads rendering needs.
 *
 * Returns 1 if a sandbox was installed, 0 if none is available on this
 * platform (in which case the process still runs; the subprocess isolation
 * in conv_pdf.c remains the baseline containment). */
int sandbox_restrict(void);

#endif
