/* capsite5.c — anon-image site carrier (r21c: what flips the second emitter?).
 *
 * capsite (file-backed Mach-O text) and capsite4 (1/4/8 threads) both EXECUTE
 * the v6 chains (entered 11/44/88 — concurrency is not the switch). Wine's
 * syscall sites live in PE images; this carrier runs ONE site whose bytes are
 * memcpy'd from the binary into an anonymous RWX page — identical machine
 * code, different image backing. entered>0 => anon mappings use the hooked
 * path too (switch is elsewhere); entered==0 with markers intact => the
 * second-emitter switch is the image/backing type.
 *
 *   clang -arch x86_64 -O1 -Wall -o capsite5 capsite5.c
 *   env -i HOME=/tmp DYLD_INSERT_LIBRARIES=$PWD/hooklib.dylib \
 *       MACFINALS_HOOK_RING=1 DISABLE_AOT=1 ./capsite5
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>

static int g_fd = -1;
static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); fflush(stdout);
}
static void sigsys_noop(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; }

/* same shape/markers as capsite site1 */
static void site1(unsigned long *sink) {
    __asm__ volatile(
        "movabs $0x1122334455667788, %%r8\n\t"
        "movq   %%r8, (%%rdi)\n\t"
        "movl   $0xffffffff, %%eax\n\t"
        "syscall\n\t"
        "movabs $0x8877665544332211, %%r8\n\t"
        "movq   %%r8, 8(%%rdi)\n\t"
        : : "D"(sink) : "r8", "rax", "rcx", "r11", "memory");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_fd = open("/tmp/rosetta-re/capsite5.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("log"); return 1; }
    logf("=== capsite5 pid %d (anon-copy site1 only)\n", (int)getpid());

    struct sigaction ss = {.sa_sigaction = sigsys_noop, .sa_flags = SA_SIGINFO};
    sigemptyset(&ss.sa_mask);
    sigaction(SIGSYS, &ss, NULL);

    /* warm file-backed site1 FIRST so the emitter is up, then never call it
     * again — every later entry count belongs to the anon copy */
    unsigned long sink[2] = {0, 0};
    site1(sink);
    logf("warm file-backed site1: %016lx %016lx\n", sink[0], sink[1]);

    void *page = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 1; }
    memcpy(page, (void *)site1, 64);
    /* x86_64: coherent I-cache, no flush needed for same-thread execution */
    void (*anon_site1)(unsigned long *) = (void (*)(unsigned long *))page;
    logf("anon copy at %p (src %p, 64 bytes)\n", page, (void *)site1);

    for (int rep = 0; rep < 4; rep++) {
        sink[0] = sink[1] = 0;
        anon_site1(sink);
        logf("anon site1 rep%d: %016lx %016lx\n", rep, sink[0], sink[1]);
    }
    logf("done; sleeping 1s for ring flush\n");
    sleep(1);
    logf("=== capsite5 done\n");
    return 0;
}
