/* jitdump.c — dump Rosetta's JIT translations of a pushfq/popfq sequence.
 *
 * Runs as a plain x86-64 binary (translated). A noinline function wraps NT-flag
 * reads/writes between two 64-bit marker constants; after it runs, every
 * anonymous RWX region (the Rosetta JIT cache, thread context, return stacks) is
 * scanned and non-empty pages are dumped to /tmp/rosetta-re/jit-N.bin with the
 * region base address in jit-N.txt, for offline arm64 disassembly.
 *
 *   clang -arch x86_64 -O1 -o jitdump jitdump.c && ./jitdump
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>

#define M1 0xfeed0001cafe0001UL
#define M2 0xfeed0002cafe0002UL
#define M3 0xfeed0003cafe0003UL
#define M4 0xfeed0004cafe0004UL

/* IRETQ with NT set: on hardware #GP; under Rosetta today it just returns
 * through the frame, so the marker after it is reachable and translated. */
extern char after_iret_m[];
__attribute__((noinline)) static void marked_iret(void) {
    __asm__ volatile(
        "movabsq %[m1], %%rax\n\txorq %%r11, %%rax\n\t"
        "pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\t"
        "movq %%rsp,%%rax\n\tmovl %%ss,%%ecx\n\tpushq %%rcx\n\tpushq %%rax\n\tpushfq\n\t"
        "movl %%cs,%%ecx\n\tpushq %%rcx\n\t"
        "leaq _after_iret_m(%%rip),%%rcx\n\tpushq %%rcx\n\tiretq\n"
        ".globl _after_iret_m\n_after_iret_m:\n\t"
        "movabsq %[m2], %%rax\n\txorq %%r11, %%rax\n\t"
        "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq"
        :: [m1] "n"(M1), [m2] "n"(M2)
        : "rax", "rcx", "r11", "memory", "cc");
}

/* cli in user mode raises #GP on hardware. The SIGSEGV handler resumes at
 * _after_cli, so the block executes (and gets translated) but survives. */
extern char after_cli[];
__attribute__((noinline)) static void marked_cli(void) {
    __asm__ volatile(
        "movabsq %[m1], %%rax\n\txorq %%r11, %%rax\n\t"
        "cli\n"
        ".globl _after_cli\n_after_cli:\n\t"
        "movabsq %[m2], %%rax\n\txorq %%r11, %%rax\n\t"
        "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq"
        :: [m1] "n"(M3), [m2] "n"(M4)
        : "rax", "r11", "memory", "cc");
}

__attribute__((noinline)) static unsigned long marked_nt(void) {
    unsigned long flags, sink;
    __asm__ volatile(
        "movabsq %[m1], %%rax\n\t"
        "xorq %%r11, %%rax\n\t"
        "pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\t"
        "pushfq\n\tpopq %0\n\t"
        "movabsq %[m2], %%rax\n\t"
        "xorq %%r11, %%rax\n\t"
        : "=r"(flags), "=r"(sink)
        : [m1] "n"(M1), [m2] "n"(M2)
        : "rax", "r11", "memory", "cc");
    (void)sink;
    return flags;
}

static int dump_anon_rwx(void) {
    mach_vm_address_t addr = 0;
    mach_vm_size_t size, got;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count;
    natural_t depth = 0;
    int part = 0, regions = 0;
    for (;;) {
        count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS)
            break;
        if (info.is_submap) { depth++; continue; }
        /* max RWX: catches the JIT, and the file-backed AOT translations and
         * libRosettaRuntime, which are mapped r-x but can be made writable.
         * Also the low Rosetta area: the AOT data/descriptor pages live there. */
        int hit = ((info.max_protection & VM_PROT_EXECUTE) && (info.max_protection & VM_PROT_WRITE))
                  || (addr >= 0x100000000ULL && addr < 0x30000000000ULL);
        if (hit && size <= (unsigned long)512 * 1024 * 1024) {
            char name[64];
            unsigned char page[4096], zeros[4096] = {0};
            FILE *out = NULL;
            unsigned long base = addr, nonzero_pages = 0, written = 0;
            for (unsigned long off = 0; off < size; off += 4096) {
                unsigned n = size - off < 4096 ? (unsigned)(size - off) : 4096;
                /* Unmapped holes inside JIT regions are normal: skip, keep position. */
                if (mach_vm_read_overwrite(mach_task_self(), addr + off, n,
                                           (vm_address_t)page, &got) != KERN_SUCCESS) {
                    if (out) { fwrite(zeros, 1, n, out); written += n; }
                    continue;
                }
                int empty = 1;
                for (unsigned j = 0; j < n; j++) if (page[j]) { empty = 0; break; }
                if (empty) {
                    if (out) { fwrite(zeros, 1, n, out); written += n; }
                    continue;
                }
                if (!out) {
                    snprintf(name, sizeof name, "/tmp/rosetta-re/jit-%d.bin", part);
                    out = fopen(name, "wb");
                    snprintf(name, sizeof name, "/tmp/rosetta-re/jit-%d.txt", part);
                    FILE *meta = fopen(name, "w");
                    fprintf(meta, "base %016lx end %016lx\n", base, (unsigned long)(base + size));
                    fclose(meta);
                }
                if (fwrite(page, 1, n, out) != n) break;
                written += n;
                nonzero_pages++;
            }
            if (out) {
                fclose(out);
                printf("region %016lx-%016lx pages %lu -> jit-%d.bin (%lu bytes)\n",
                       base, (unsigned long)(base + size), nonzero_pages, part, written);
                part++; regions++;
            }
        }
        addr += size;
    }
    return regions;
}

static void *volatile skip_to; /* fault resume target for marked_cli */
static void fault_handler(int sig, siginfo_t *si, void *vctx) {
    ucontext_t *uc = vctx;
    if (skip_to) {
        uc->uc_mcontext->__ss.__rip = (uint64_t)skip_to;
        skip_to = NULL;
    }
}

/* DO NOT re-add code that makes libRosettaRuntime's file-backed TEXT pages
 * writable (mprotect RWX/RW, mach_vm_write, anything similar). On macOS 27.0
 * (26A428) that wedges a translated thread unkillably in the kernel and froze
 * the whole machine on 2026-10-31 (~00:35, forced power-off, no panic file).
 * Any Rosetta correction must limit itself to anonymous RWX pages - the JIT
 * region and our own mappings - never file-backed code. */
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    unsigned long f = marked_nt();
    marked_nt();
    marked_iret();
    skip_to = after_cli;
    marked_cli();
    printf("NT read back: %lu (marker kept alive: %lu)\n", (unsigned long)!!(f & 0x4000), (unsigned long)(f & 1));
    int n = dump_anon_rwx();
    printf("dumped %d region(s)\n", n);
    if (getenv("MF_PAUSE")) { printf("pausing for vmmap...\n"); fflush(stdout); sleep(120); }
    return 0;
}
