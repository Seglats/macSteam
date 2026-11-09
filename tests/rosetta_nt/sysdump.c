/* sysdump.c — dump Rosetta's JIT translation of a direct x86 `syscall` instruction.
 *
 * The game's protected code issues `syscall` directly (32k/s with eax=0xffffffff under
 * THE FINALS). Wine never sees those as calls — Rosetta translates `syscall` into a
 * runtime thunk; an invalid class traps brk #5 -> exception server -> SIGSYS. This tool
 * captures the PER-SITE emission (the code translated for the `syscall` instruction
 * itself, which loads the x86 registers and calls the shared per-page thunk) and the
 * gs:[0x30] load emission (to find the GS-base slot in the x18 thread context), by
 * running marker-wrapped code copied onto an anonymous RWX page (JIT-translated; AOT
 * only covers file-backed images) and then dumping every anonymous RWX region.
 *
 *   clang -arch x86_64 -O1 -o sysdump sysdump.c && ./sysdump
 *
 * Writes /tmp/rosetta-re/sys-N.bin (+ base in sys-N.txt). Find the block via the
 * movz/movk marker immediates, disassemble with wrap_raw.py + otool.
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

#define M1 0xfeed1001cafe0001UL   /* before the 0xffffffff syscall */
#define M2 0xfeed1002cafe0002UL   /* before the 0x50 (class 0) syscall */
#define M3 0xfeed1003cafe0003UL   /* before the 0x2000014 getpid syscall */
#define M4 0xfeed1004cafe0004UL   /* before the gs:[0x30] load */

/* movabs imm64 is little-endian in the instruction stream. */
#define Q(x) (unsigned char)(x),         (unsigned char)((x) >> 8),  \
            (unsigned char)((x) >> 16),  (unsigned char)((x) >> 24), \
            (unsigned char)((x) >> 32),  (unsigned char)((x) >> 40), \
            (unsigned char)((x) >> 48),  (unsigned char)((x) >> 56)

/* x86-64: markers stored through %rdi (kept alive), syscalls, gs load, ret.
 * A direct `syscall` from a plain binary raises SIGSYS for the invalid classes;
 * the handler resumes at the (already advanced) post-syscall RIP, which is the
 * next marker store — so the whole block translates and runs. */
static const unsigned char sys_code[] = {
    0x49, 0xb8, Q(M1),                    /* movabs $M1,%r8            */
    0x4c, 0x89, 0x07,                     /* mov %r8,(%rdi)            */
    0xb8, 0xff, 0xff, 0xff, 0xff,         /* mov $0xffffffff,%eax      */
    0x0f, 0x05,                           /* syscall                   */
    0x49, 0xb8, Q(M2),                    /* movabs $M2,%r8            */
    0x4c, 0x89, 0x07,                     /* mov %r8,(%rdi)            */
    0xb8, 0x50, 0x00, 0x00, 0x00,         /* mov $0x50,%eax (class 0)  */
    0x0f, 0x05,                           /* syscall                   */
    0x49, 0xb8, Q(M3),                    /* movabs $M3,%r8            */
    0x4c, 0x89, 0x07,                     /* mov %r8,(%rdi)            */
    0xb8, 0x14, 0x00, 0x00, 0x02,         /* mov $0x2000014,%eax getpid*/
    0x0f, 0x05,                           /* syscall                   */
    0x49, 0xb8, Q(M4),                    /* movabs $M4,%r8            */
    0x4c, 0x89, 0x07,                     /* mov %r8,(%rdi)            */
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00, /* mov %gs:0x30,%rax */
    0x48, 0x89, 0x07,                     /* mov %rax,(%rdi)           */
    0xc3,                                 /* ret                       */
};

static void sigsys_handler(int sig, siginfo_t *si, void *vctx)
{
    (void)sig; (void)si; (void)vctx;
    /* Resume at the context RIP (already past the syscall): nothing to patch. */
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
        int hit = ((info.max_protection & VM_PROT_EXECUTE) && (info.max_protection & VM_PROT_WRITE))
                  || (addr >= 0x100000000ULL && addr < 0x30000000000ULL);
        if (hit && size <= (unsigned long)512 * 1024 * 1024) {
            char name[512];
            unsigned char page[4096], zeros[4096] = {0};
            FILE *out = NULL;
            unsigned long base = addr, nonzero_pages = 0, written = 0;
            for (unsigned long off = 0; off < size; off += 4096) {
                unsigned n = size - off < 4096 ? (unsigned)(size - off) : 4096;
                if (mach_vm_read_overwrite(mach_task_self(), addr + off, n,
                                           (mach_vm_address_t)page, &got) != KERN_SUCCESS) {
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
                    snprintf(name, sizeof name, "/tmp/rosetta-re/sys-%d.bin", part);
                    out = fopen(name, "wb");
                    snprintf(name, sizeof name, "/tmp/rosetta-re/sys-%d.txt", part);
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
                printf("region %016lx-%016lx pages %lu -> sys-%d.bin (%lu bytes)\n",
                       base, (unsigned long)(base + size), nonzero_pages, part, written);
                part++; regions++;
            }
        }
        addr += size;
    }
    return regions;
}

/* DO NOT re-add code that makes libRosettaRuntime's file-backed TEXT pages
 * writable (mprotect RWX/RW, mach_vm_write, anything similar). On macOS 27.0
 * (26A428) that wedges a translated thread unkillably in the kernel and froze
 * the whole machine on 2026-10-31 (~00:35, forced power-off, no panic file).
 * Any Rosetta correction must limit itself to anonymous RWX pages - the JIT
 * region and our own mappings - never file-backed code. */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa = {.sa_sigaction = sigsys_handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, NULL);

    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 1; }
    memcpy(page, sys_code, sizeof sys_code);
    unsigned long sink[4] = {0};
    for (int i = 0; i < 3; i++)
        ((void (*)(unsigned long *))page)(sink);
    printf("markers: %016lx %016lx %016lx %016lx\n", sink[0], sink[1], sink[2], sink[3]);
    int n = dump_anon_rwx();
    printf("dumped %d region(s)\n", n);
    return 0;
}
