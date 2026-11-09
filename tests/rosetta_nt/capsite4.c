/* capsite4.c — multi-threaded syscall-site carrier (r21b follow-up).
 *
 * capsite (single-threaded) inserts v6 chains that EXECUTE (entered=11).
 * A Wine process inserts the same chains into the same scratch stream, they
 * are installed intact into the 128MB RWX VMA — and never execute, because
 * the executed TBs are emitted by a SECOND path that never calls the hooked
 * append (NOTES.md r21b/r21c: ins N miss 0, single bucket, raw-bl#0 drafts
 * elsewhere). This carrier tests the obvious switch: THREADS. Same three
 * marked sites, but executed from N threads concurrently. If entered drops
 * to 0 with >=2 threads, concurrency flips Rosetta to the second emitter
 * and the hunt narrows to it. N via argv[1] (default 4).
 *
 *   clang -arch x86_64 -O1 -Wall -o capsite4 capsite4.c -pthread
 *   env -i HOME=/tmp DYLD_INSERT_LIBRARIES=$PWD/hooklib.dylib \
 *       MACFINALS_HOOK_RING=1 DISABLE_AOT=1 ./capsite4 4
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>

static int g_fd = -1;
static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); fflush(stdout);
}
static void sigsys_noop(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; }

/* identical sites to capsite — same markers, so the shape is comparable */
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

static void *worker(void *arg) {
    long id = (long)arg;
    unsigned long sink[2];
    char nm[32];
    snprintf(nm, sizeof nm, "t%ld", id);
    for (int rep = 0; rep < 4; rep++) {
        sink[0] = sink[1] = 0;
        site1(sink);
        logf("%s site1 rep%d: %016lx %016lx\n", nm, rep, sink[0], sink[1]);
        sink[0] = sink[1] = 0;
        site2(sink);
        logf("%s site2 rep%d: %016lx %016lx\n", nm, rep, sink[0], sink[1]);
        sink[0] = sink[1] = 0;
        site3(sink);
        logf("%s site3 rep%d: %016lx %016lx\n", nm, rep, sink[0], sink[1]);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int nthread = argc > 1 ? atoi(argv[1]) : 4;
    if (nthread < 1) nthread = 1;
    if (nthread > 16) nthread = 16;
    setvbuf(stdout, NULL, _IONBF, 0);
    g_fd = open("/tmp/rosetta-re/capsite4.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("log"); return 1; }
    logf("=== capsite4 pid %d threads %d\n", (int)getpid(), nthread);

    struct sigaction ss = {.sa_sigaction = sigsys_noop, .sa_flags = SA_SIGINFO};
    sigemptyset(&ss.sa_mask);
    sigaction(SIGSYS, &ss, NULL);

    pthread_t th[16];
    for (long i = 0; i < nthread; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < nthread; i++)
        pthread_join(th[i], NULL);
    logf("all threads joined; sleeping 1s for ring flush\n");
    sleep(1);
    logf("=== capsite4 done\n");
    return 0;
}
