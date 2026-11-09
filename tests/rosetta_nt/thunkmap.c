/* thunkmap.c — read-only locator for Rosetta's finished syscall thunks, built
 * under the 2026-11-07 method constraints (CLAUDE.md / NOTES.md "r20"):
 *
 *   - NO mach_vm_read_overwrite, NO mach_vm_write, NO mach_vm_protect. The
 *     read-overwrite copy path panicked this kernel twice on translated exec
 *     regions; none of that family is called here.
 *   - Region bounds come from mach_vm_region_recurse (metadata only — the
 *     same call cowtest/sysdump/jitdump and -40's converter_poll have run
 *     hundreds of times clean).
 *   - Page CONTENTS are read with direct user-space loads guarded by a
 *     sigsetjmp SIGSEGV/SIGBUS recovery handler: a bad page costs a
 *     catchable signal in our own process, never a kernel panic.
 *   - Read-only. It never writes any memory, never patches anything. The one
 *     unknown it answers: what is the thunk region's CURRENT protection —
 *     RW (plain stores can patch it) or RX (a protection change would be
 *     needed, which requires Rasmus's per-tier sign-off)?
 *
 * Every step is appended+fsynced to /tmp/rosetta-re/thunkmap.log BEFORE it
 * runs, so a kernel wedge would leave the exact step named on disk.
 *
 *   clang -arch x86_64 -O1 -Wall -o thunkmap thunkmap.c && ./thunkmap
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>

/* ---- verified finished-thunk signature (r20, ground-truthed 2026-11-07) */
#define TH_REGION    0x4000u     /* thunk region size: 16K */
#define TH_ENTRY_OFF 0x200u      /* strict entry */
#define TH_TAG_OFF   0x22cu      /* orr x18,#bit63 */
#define TH_ENTB_OFF  0x230u      /* b -> dispatch */
#define TH_DEF_OFF   0x2b30u     /* the one branch feeding the trap */
#define TH_TRAP_OFF  0x2eb8u     /* brk #5 + b +0x400 */
static const uint32_t TH_ENTRY_SIG[3] = {0xa9010640, 0xa9020e42, 0xa9031644};
#define TH_W_TAG   0xb2410252u
#define TH_W_ENTB  0x14000a38u
#define TH_W_DEF   0x140000e2u
#define TH_W_TRAP0 0xd42000a0u
#define TH_W_TRAP1 0x17ffff51u

static int g_fd = -1;
static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); va_end(ap);
    fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fflush(stdout);
}

/* ---- guarded direct reader */
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_armed = 0;
static void recover(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si; (void)uc;
    if (g_armed) siglongjmp(g_jmp, 1);
    /* a fault outside the guarded reader is a real bug in this tool: die
     * loudly rather than masking it */
    _exit(77);
}
static int safe_read32(const uint32_t *p, uint32_t *out) {
    g_armed = 1;
    if (sigsetjmp(g_jmp, 1)) { g_armed = 0; return 0; }
    *out = *(volatile const uint32_t *)p;
    g_armed = 0;
    return 1;
}

/* the no-op SIGSYS handler: under Rosetta the emulated SIGSYS context's RIP
 * is already past the syscall instruction, so returning simply resumes */
static void sigsys_noop(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; }

/* minimal trigger: a direct invalid-class syscall plus a class-0 one, on an
 * anon RWX page so Rosetta JIT-translates them (AOT covers only file-backed
 * images) — this is what makes the process own a finished thunk at all */
static const unsigned char code[] = {
    0xb8, 0xff, 0xff, 0xff, 0xff,   /* mov $0xffffffff,%eax */
    0x0f, 0x05,                     /* syscall */
    0xb8, 0x50, 0x00, 0x00, 0x00,   /* mov $0x50,%eax (class 0) */
    0x0f, 0x05,                     /* syscall */
    0xc3,                           /* ret */
};

static const char *prot_str(unsigned p) {
    static char b[8];
    b[0] = (p & VM_PROT_READ) ? 'r' : '-';
    b[1] = (p & VM_PROT_WRITE) ? 'w' : '-';
    b[2] = (p & VM_PROT_EXECUTE) ? 'x' : '-';
    b[3] = 0;
    return b;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_fd = open("/tmp/rosetta-re/thunkmap.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("log"); return 2; }
    logf("=== thunkmap pid %d: read-only, no mach copy calls\n", (int)getpid());

    struct sigaction rec = {.sa_sigaction = recover, .sa_flags = SA_SIGINFO};
    sigemptyset(&rec.sa_mask);
    sigaction(SIGSEGV, &rec, NULL);
    sigaction(SIGBUS, &rec, NULL);
    struct sigaction ss = {.sa_sigaction = sigsys_noop, .sa_flags = SA_SIGINFO};
    sigemptyset(&ss.sa_mask);
    sigaction(SIGSYS, &ss, NULL);
    logf("handlers installed\n");

    logf("trigger: translating direct syscalls on an anon page\n");
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 1; }
    memcpy(page, code, sizeof code);
    for (int i = 0; i < 8; i++) ((void (*)(void))page)();
    logf("trigger done\n");

    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    unsigned regions = 0, skipped_pages = 0, thunks = 0;

    logf("walk: begin (metadata only)\n");
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        mach_vm_address_t next = addr + size;
        if (!info.is_submap && size <= 512ull * 1024 * 1024 &&
            !(info.protection & VM_PROT_READ) &&
            ((info.protection | info.max_protection) & VM_PROT_EXECUTE) &&
            size <= 0x10000) {
            /* exec-but-not-directly-readable anon region (<=64K): this is
             * where the finished thunks live — Rosetta maps them PROT_NONE
             * against the process's own loads (run 1: every direct load on
             * these faulted; run 2: skipping them found 0 thunks in 325
             * readable regions). The ONLY reader that has ever seen these
             * pages is the kernel copy path in its sysdump shape: 4K
             * mach_vm_read_overwrite, post-startup, single-threaded, one
             * region at a time — run clean dozens of times incl. twice this
             * week. The shape that PANICKED (2026-11-07) was 256K chunks at
             * dyld-constructor time with a second mach client live. We stay
             * strictly inside the proven envelope, logging each region
             * before touching it. */
            char nm[256] = {0};
            proc_regionfilename(getpid(), addr, nm, sizeof nm - 1);
            if (!nm[0]) {
                logf("MACHREAD region %016llx-%016llx cur %s max %s share %u\n",
                     (unsigned long long)addr, (unsigned long long)next,
                     prot_str(info.protection), prot_str(info.max_protection),
                     info.share_mode);
                static uint32_t buf[0x1000 / 4];
                for (unsigned long off = 0; off < size; off += 0x1000) {
                    mach_vm_size_t got = 0;
                    logf("  read page %016llx (+%lx)\n",
                         (unsigned long long)(addr + off), off);
                    if (mach_vm_read_overwrite(mach_task_self(), addr + off, 0x1000,
                                               (mach_vm_address_t)buf, &got) != KERN_SUCCESS) {
                        logf("  -> read FAILED, stopping this region\n");
                        break;
                    }
                    for (unsigned i = 0; i + 1 < 0x1000 / 4; i++) {
                        if (buf[i] != TH_W_TRAP0 || buf[i + 1] != TH_W_TRAP1) continue;
                        mach_vm_address_t tbase =
                            (addr + off + (mach_vm_address_t)i * 4) - TH_TRAP_OFF;
                        logf("  TRAP PAIR at %016llx -> thunk base %016llx%s\n",
                             (unsigned long long)(addr + off + i * 4),
                             (unsigned long long)tbase,
                             (tbase & (TH_REGION - 1)) ? " (NOT 16K-aligned)"
                                                       : " (16K-aligned)");
                    }
                }
            }
        }
        if (!info.is_submap && size <= 512ull * 1024 * 1024 &&
            (info.protection & VM_PROT_READ) &&
            ((info.protection | info.max_protection) & VM_PROT_EXECUTE)) {
            char nm[256] = {0};
            proc_regionfilename(getpid(), addr, nm, sizeof nm - 1);
            if (!nm[0] && (mach_vm_address_t)page != addr) {
                /* anonymous readable+exec candidate: direct-load scan, page
                 * by page, fault-tolerant (inner PROT_NONE holes skip) */
                logf("scan region %016llx-%016llx cur %s max %s share %u\n",
                     (unsigned long long)addr, (unsigned long long)next,
                     prot_str(info.protection), prot_str(info.max_protection),
                     info.share_mode);
                unsigned bad_pages = 0, hits_here = 0;
                for (unsigned long off = 0; off < size; off += 0x1000) {
                    const uint32_t *pg = (const uint32_t *)(addr + off);
                    unsigned words = 0x1000 / 4;
                    uint32_t w0;
                    if (!safe_read32(pg, &w0)) {
                        logf("  fault page %016llx — skipped\n",
                             (unsigned long long)(addr + off));
                        bad_pages++; skipped_pages++;
                        continue;
                    }
                    for (unsigned i = 0; i + 1 < words; i++) {
                        uint32_t a, b;
                        if (!safe_read32(&pg[i], &a)) break;
                        if (a != TH_W_TRAP0) continue;
                        if (!safe_read32(&pg[i + 1], &b) || b != TH_W_TRAP1) continue;
                        mach_vm_address_t tbase =
                            (addr + off + (mach_vm_address_t)i * 4) - TH_TRAP_OFF;
                        /* full signature check at the candidate base */
                        uint32_t s0, s1, s2, tag, entb, def;
                        if (tbase & (TH_REGION - 1)) continue;
                        if (!safe_read32((uint32_t *)(tbase + TH_ENTRY_OFF), &s0) ||
                            !safe_read32((uint32_t *)(tbase + TH_ENTRY_OFF + 4), &s1) ||
                            !safe_read32((uint32_t *)(tbase + TH_ENTRY_OFF + 8), &s2) ||
                            s0 != TH_ENTRY_SIG[0] || s1 != TH_ENTRY_SIG[1] ||
                            s2 != TH_ENTRY_SIG[2])
                            continue;
                        if (!safe_read32((uint32_t *)(tbase + TH_TAG_OFF), &tag) ||
                            tag != TH_W_TAG) continue;
                        if (!safe_read32((uint32_t *)(tbase + TH_ENTB_OFF), &entb) ||
                            entb != TH_W_ENTB) continue;
                        if (!safe_read32((uint32_t *)(tbase + TH_DEF_OFF), &def) ||
                            def != TH_W_DEF) continue;
                        /* verified finished thunk — record everything */
                        logf("THUNK #%u at %016llx (region %016llx-%016llx %llu bytes "
                             "cur %s max %s share %u)\n",
                             thunks, (unsigned long long)tbase,
                             (unsigned long long)addr, (unsigned long long)next,
                             (unsigned long long)size,
                             prot_str(info.protection), prot_str(info.max_protection),
                             info.share_mode);
                        char path[128];
                        snprintf(path, sizeof path,
                                 "/tmp/rosetta-re/thunkmap-%u.bin", thunks);
                        FILE *f = fopen(path, "wb");
                        if (f) {
                            for (unsigned k = 0; k < TH_REGION / 4; k++) {
                                uint32_t w;
                                if (!safe_read32((uint32_t *)(tbase) + k, &w)) break;
                                fwrite(&w, 4, 1, f);
                            }
                            fclose(f);
                        }
                        thunks++; hits_here++;
                        off += TH_REGION - 0x1000;   /* skip past this thunk */
                        break;
                    }
                }
                if (hits_here || bad_pages)
                    logf("  region %016llx: %u thunk(s), %u unreadable page(s)\n",
                         (unsigned long long)addr, hits_here, bad_pages);
                regions++;
            }
        }
        addr = next;
    }
    logf("walk done: %u candidate regions, %u thunks, %u skipped pages\n",
         regions, thunks, skipped_pages);
    logf("=== thunkmap done\n");
    return thunks ? 0 : 1;
}
