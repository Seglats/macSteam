/* capsite.c — syscall-site capture carrier (r19 revival, capture-first).
 *
 * Run under DYLD_INSERT of hooklib.dylib with MACFINALS_HOOK_RING=1 and
 * DISABLE_AOT=1: three marked `syscall` sites (rax=0xffffffff, the game's
 * shape), each executed so Rosetta translates it fresh while the append-hook
 * ring records the true per-site emission. The movabs marker pairs make each
 * site's block locatable both in the captured ring (immediate halves in the
 * movz/movk materialisation) and in the finished TB (the stores' operands).
 *
 * The sites live in the binary's own file-backed text, like the game's —
 * DISABLE_AOT means they JIT at first execution either way. A SIGSYS noop
 * handler rides the stock path (Rosetta's emulated SIGSYS context is already
 * past the syscall, so returning simply resumes).
 *
 *   clang -arch x86_64 -O1 -Wall -o capsite capsite.c
 *   env -i HOME=/tmp DYLD_INSERT_LIBRARIES=$PWD/hooklib.dylib \
 *       MACFINALS_HOOK_RING=1 DISABLE_AOT=1 ./capsite
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>

static int g_fd = -1;
static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); fflush(stdout);
}

static void sigsys_noop(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; }

/* Each site: marker A store, mov rax,-1, SYSCALL, marker B store, ret.
 * rdi = sink (first arg). rcx/r11 clobbered by syscall. */
#define SITE(n, MA, MB)                                                   \
    static void site##n(unsigned long *sink) {                            \
        __asm__ volatile(                                                 \
            "movabs $" #MA ", %%r8\n\t"                                   \
            "movq   %%r8, (%%rdi)\n\t"                                    \
            "movl   $0xffffffff, %%eax\n\t"                               \
            "syscall\n\t"                                                 \
            "movabs $" #MB ", %%r8\n\t"                                   \
            "movq   %%r8, 8(%%rdi)\n\t"                                   \
            : : "D"(sink) : "r8", "rax", "rcx", "r11", "memory");         \
    }
SITE(1, 0x1122334455667788, 0x8877665544332211)
SITE(2, 0x99aabbccddeeff00, 0x00ffeeddccbbaa99)
SITE(3, 0x55aa55aa55aa55aa, 0xaa55aa55aa55aa55)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_fd = open("/tmp/rosetta-re/capsite.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("log"); return 1; }
    logf("=== capsite pid %d (3 marked syscall sites, rax=-1)\n", (int)getpid());

    struct sigaction ss = {.sa_sigaction = sigsys_noop, .sa_flags = SA_SIGINFO};
    sigemptyset(&ss.sa_mask);
    sigaction(SIGSYS, &ss, NULL);
    logf("SIGSYS noop installed\n");

    unsigned long sink[2] = {0, 0};
    for (int rep = 0; rep < 4; rep++) {
        sink[0] = sink[1] = 0;
        site1(sink); logf("site1 rep%d: %016lx %016lx\n", rep, sink[0], sink[1]);
        sink[0] = sink[1] = 0;
        site2(sink); logf("site2 rep%d: %016lx %016lx\n", rep, sink[0], sink[1]);
        sink[0] = sink[1] = 0;
        site3(sink); logf("site3 rep%d: %016lx %016lx\n", rep, sink[0], sink[1]);
    }
    logf("all reps done; sleeping 1s for ring flush\n");
    sleep(1);
    logf("=== capsite done\n");
    return 0;
}
