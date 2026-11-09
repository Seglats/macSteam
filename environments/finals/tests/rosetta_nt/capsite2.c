/* capsite2.c — insert-land forensics (r21, 2026-11-08).
 *
 * The re-armed r19 insert reproduces the r20-era translator assert at site2
 * while site1 executes cleanly. This carrier captures the physical evidence
 * before the death: run site1 (translate + execute through the insert),
 * then scan the readable JIT arena with guarded direct loads for our insert
 * chain (movz/movk x26 + br x26 — Rosetta never emits x26/x27), dump the TB
 * words around every hit (is the chain intact? did a relocation clobber the
 * adr? is the bl still the 94000000 placeholder?), and only then run site2,
 * which asserts. Ring capture stays on for the stream side.
 *
 * Scan safety per the 2026-11-07 method constraint: mach_vm_region_recurse
 * for metadata only; contents via direct loads behind sigsetjmp SIGSEGV/
 * SIGBUS recovery; no mach copy/protection call anywhere.
 *
 *   clang -arch x86_64 -O1 -Wall -o capsite2 capsite2.c
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>

static int g_fd = -1;
static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); fflush(stdout);
}
static void sigsys_noop(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; }

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

/* ---- guarded direct reader (thunkmap-proven shape) ---- */
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_armed = 0;
static void recover(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    if (g_armed) siglongjmp(g_jmp, 1);
    _exit(77);
}
static int safe_read32(const uint32_t *p, uint32_t *out) {
    g_armed = 1;
    if (sigsetjmp(g_jmp, 1)) { g_armed = 0; return 0; }
    *out = *(volatile const uint32_t *)p;
    g_armed = 0;
    return 1;
}

/* our insert chain: br x26 preceded by a movz/movk x26 family word within
 * 4 words. Rosetta never allocates x26. */
static int is_movz_x26_family(uint32_t w) {
    return (w & 0xffe0001fu) == 0xd2800000u + 26u ||   /* movz x26 */
           (w & 0xffe0001fu) == 0xf2800000u + 26u ||   /* movk x26 */
           (w & 0xffe0001fu) == 0xf2a00000u + 26u ||
           (w & 0xffe0001fu) == 0xf2c00000u + 26u ||
           (w & 0xffe0001fu) == 0xf2e00000u + 26u;
}
#define BR_X26 0xd61f0340u

static void scan_and_dump(void) {
    logf("scan: walking regions (metadata + guarded loads only)\n");
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    unsigned regions = 0, hits = 0;
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        mach_vm_address_t next = addr + size;
        if (!info.is_submap && size <= 512ull * 1024 * 1024 &&
            (info.protection & VM_PROT_READ) &&
            ((info.protection | info.max_protection) & VM_PROT_EXECUTE)) {
            char nm[256] = {0};
            proc_regionfilename(getpid(), addr, nm, sizeof nm - 1);
            if (!nm[0]) {
                regions++;
                for (unsigned long off = 0; off + 40 <= size; off += 4) {
                    const uint32_t *pg = (const uint32_t *)(addr + off);
                    uint32_t w;
                    if (!safe_read32(pg, &w)) break;   /* unreadable page: stop this region */
                    if (w != BR_X26) continue;
                    /* confirm an x26 materialisation word within 4 words before */
                    int ok = 0;
                    for (int k = 1; k <= 4 && !ok; k++) {
                        uint32_t p;
                        if (safe_read32(pg - k, &p) && is_movz_x26_family(p)) ok = 1;
                    }
                    if (!ok) continue;
                    /* dump 12 words before .. 8 after the br */
                    char path[128];
                    snprintf(path, sizeof path, "/tmp/rosetta-re/tbdump-%u.bin", hits);
                    FILE *f = fopen(path, "wb");
                    logf("HIT %u at %016llx (region %016llx-%016llx) -> %s  words:",
                         hits, (unsigned long long)(addr + off),
                         (unsigned long long)addr, (unsigned long long)next, path);
                    for (int k = -12; k <= 8; k++) {
                        uint32_t v;
                        if (safe_read32(pg + k, &v)) {
                            logf(" %08x", v);
                            if (f) fwrite(&v, 4, 1, f);
                        } else { logf(" XXXXXXXX"); break; }
                    }
                    logf("\n");
                    if (f) fclose(f);
                    hits++;
                    off += 8 * 4;
                }
            }
        }
        addr = next;
    }
    logf("scan done: %u regions, %u insert-chain hits\n", regions, hits);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_fd = open("/tmp/rosetta-re/capsite2.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("log"); return 1; }
    logf("=== capsite2 pid %d\n", (int)getpid());

    struct sigaction rec = {.sa_sigaction = recover, .sa_flags = SA_SIGINFO};
    sigemptyset(&rec.sa_mask);
    sigaction(SIGSEGV, &rec, NULL);
    sigaction(SIGBUS, &rec, NULL);
    struct sigaction ss = {.sa_sigaction = sigsys_noop, .sa_flags = SA_SIGINFO};
    sigemptyset(&ss.sa_mask);
    sigaction(SIGSYS, &ss, NULL);
    logf("handlers installed\n");

    unsigned long sink[2];
    /* phase 1: site1 twice — translate, execute through the insert, re-execute */
    sink[0] = sink[1] = 0; site1(sink);
    logf("site1 run1: %016lx %016lx\n", sink[0], sink[1]);
    sink[0] = sink[1] = 0; site1(sink);
    logf("site1 run2: %016lx %016lx\n", sink[0], sink[1]);

    /* phase 2: physical evidence while still alive */
    scan_and_dump();

    /* phase 3: provoke the assert (site2 first execution) */
    logf("phase3: calling site2 (expected translator assert)\n");
    sink[0] = sink[1] = 0; site2(sink);
    logf("site2 run1 SURVIVED: %016lx %016lx\n", sink[0], sink[1]);
    sink[0] = sink[1] = 0; site3(sink);
    logf("site3 run1: %016lx %016lx\n", sink[0], sink[1]);
    logf("=== capsite2 done\n");
    return 0;
}
