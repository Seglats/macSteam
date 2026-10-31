/* cowtest.c — is an in-process COW privatize of a translated file-backed
 * executable page survivable on this kernel?
 *
 * Rasmus, 2026-10-31: machine crashes are an accepted cost of this ladder, on
 * the single condition that nothing is ever written to a file that affects
 * boot. This tool writes no file except the step log in /tmp; every mutation
 * is in-memory, in-process, and every tier except 5 writes the SAME byte it
 * read, so pass or fail leaves behaviour unchanged.
 *
 * Ladder (argv[1] = highest tier; a run executes tiers 1..N in order,
 * stopping at the first failure):
 *   1  main executable __LINKEDIT (read-only file page)        vanilla control
 *   2  a private PROT_EXEC file mapping of our own binary     cold exec page
 *   3  libRosettaRuntime __TEXT, data page 0x60000 (the __const/__cstring
 *      range, past __text at 0x4e6f4 — the protection mechanics on Rosetta's
 *      exec mapping with zero code inside the window)
 *   4  libRosettaRuntime __TEXT, code page 0x4c000, byte at 0x4e800 inside
 *      runtime_entry (runs only at process birth) — a real code page
 *   5  tier-3 page but writing a DIFFERENT byte, spinning 5 s, then restoring
 *      the original — does code-signing enforcement kill us asynchronously?
 *
 * Per ROSETTA_NT_APPROACH.md §4: mach_vm_protect(RW|VM_PROT_COPY) — COW
 * first, never W+X — read, write, sys_icache_invalidate, restore the original
 * protection. Windows are 16 KiB: Apple Silicon uses 16 K pages even for
 * translated processes. Tiers 1–2 warm every code path this tool uses, so no
 * fresh translation (which would run translator code) happens inside a tier
 * 3–5 window.
 *
 * Every step is appended+fsynced to /tmp/rosetta-re/cowtest.log BEFORE and
 * AFTER, so a kernel wedge leaves the exact step named on disk. Outcomes go
 * into the experiment ledger in NOTES.md.
 *
 *   clang -arch x86_64 -O1 -Wall -o cowtest cowtest.c && ./cowtest 5
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <libkern/OSCacheControl.h>

/* Translated processes run with 4K pages (confirmed by the 2026-10-31 tier-1
 * SIGBUS: a 16K window masked down from __LINKEDIT overran into the adjacent
 * Rosetta JIT region and stripped execute from translated code — instant
 * KERN_PROTECTION_FAILURE on fetch, machine unharmed). So: 4K granularity,
 * and every window is clamped to never cross a vm region boundary. */
static unsigned long PS = 0x1000;
static int g_fd = -1;

static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); va_end(ap);
    fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fflush(stdout);
}

static int read_bytes(mach_vm_address_t a, void *out, mach_vm_size_t n) {
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)out, &got) != KERN_SUCCESS)
        return 0;
    return got == n;
}

static unsigned prot_of(mach_vm_address_t a) {
    mach_vm_address_t addr = a;
    mach_vm_size_t size = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    natural_t depth = 0;
    if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                               (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
        return 0;
    return info.protection;
}

/* Size of the window [page, page+want) clamped to the containing vm region,
 * so a protection change can never spill into a neighbouring mapping (the
 * JIT region, most importantly). 0 = page not found / region too small. */
static mach_vm_size_t clamp_to_region(mach_vm_address_t page, mach_vm_size_t want) {
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            return 0;
        if (info.is_submap) { depth++; continue; }
        if (page >= addr && page < addr + size) {
            if (addr + size - page < want) want = addr + size - page;
            return want >= PS ? want : 0;
        }
        if (addr > page) return 0;
        addr += size;
    }
}

/* One COW cycle: privatize to RW (never W+X), read/write, flush, restore. */
static int cow_cycle(int tier, const char *what, mach_vm_address_t page,
                     unsigned off, int flip, unsigned spin) {
    uint8_t before = 0, after = 0, wrote;
    unsigned orig = prot_of(page);
    mach_vm_size_t win = clamp_to_region(page, PS);
    if (!win) { logf("tier %d: window clamp FAILED (region lookup)\n", tier); return 1; }
    logf("tier %d (%s) page %016llx prot %x win %llu: begin\n", tier, what,
         (unsigned long long)page, orig, (unsigned long long)win);
    if (!read_bytes(page + off, &before, 1)) { logf("tier %d: read-before FAILED\n", tier); return 1; }
    logf("tier %d: byte %02x; privatizing RW|COPY\n", tier, before);
    kern_return_t kr = mach_vm_protect(mach_task_self(), page, win, FALSE,
                                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    logf("tier %d: mach_vm_protect RW|COPY kr=%d\n", tier, kr);
    if (kr != KERN_SUCCESS) return 1;
    wrote = flip ? (uint8_t)~before : before;
    *(volatile uint8_t *)(page + off) = wrote;
    logf("tier %d: wrote %02x; icache flush\n", tier, wrote);
    sys_icache_invalidate((void *)page, win);
    logf("tier %d: flushed; restoring prot\n", tier);
    kr = mach_vm_protect(mach_task_self(), page, win, FALSE, orig);
    logf("tier %d: restore kr=%d; prot now %x\n", tier, kr, prot_of(page));
    if (kr != KERN_SUCCESS) return 1;
    if (!read_bytes(page + off, &after, 1)) { logf("tier %d: read-after FAILED\n", tier); return 1; }
    logf("tier %d: read-after %02x (%s)\n", tier, after, after == wrote ? "match" : "MISMATCH");
    if (after != wrote) return 1;
    while (spin--) { logf("tier %d: alive %u...\n", tier, spin + 1); sleep(1); }
    if (flip) { /* put the original byte back through a second cycle */
        logf("tier %d: restoring original byte %02x\n", tier, before);
        kr = mach_vm_protect(mach_task_self(), page, win, FALSE,
                             VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
        if (kr != KERN_SUCCESS) { logf("tier %d: re-privatize FAILED kr=%d\n", tier, kr); return 1; }
        *(volatile uint8_t *)(page + off) = before;
        sys_icache_invalidate((void *)page, win);
        kr = mach_vm_protect(mach_task_self(), page, win, FALSE, orig);
        logf("tier %d: original restored kr=%d\n", tier, kr);
        if (kr != KERN_SUCCESS) return 1;
    }
    return 0;
}

/* Main executable __LINKEDIT: a plain read-only file mapping. */
static mach_vm_address_t linkedit_page(void) {
    const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const uint8_t *p = (const uint8_t *)h;
    const struct load_command *lc = (const struct load_command *)(p + sizeof *h);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            if (!strcmp(sg->segname, "__LINKEDIT") && sg->vmsize)
                return ((mach_vm_address_t)(sg->vmaddr + slide)) & ~(PS - 1);
        }
        lc = (const struct load_command *)((const uint8_t *)lc + lc->cmdsize);
    }
    return 0;
}

/* A PROT_EXEC MAP_PRIVATE mapping of our own file: file-backed, executable,
 * translated-process — and by construction never executed from. */
static mach_vm_address_t own_file_exec_page(void) {
    char path[4096];
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    void *m = mmap(NULL, PS, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    close(fd);
    return m == MAP_FAILED ? 0 : (mach_vm_address_t)m;
}

/* The arm64 image mapped into this x86_64 process is libRosettaRuntime; its
 * __TEXT mapping starts with an arm64 Mach-O header. Matched by filename if
 * the kernel gives one, else by the arm64 header itself. */
static mach_vm_address_t rosetta_text_base(void) {
    mach_vm_address_t addr = 0, fallback = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    char buf[256];
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        if (info.is_submap) { depth++; continue; }
        if (info.protection & VM_PROT_EXECUTE) {
            unsigned char hdr[8];
            buf[0] = 0;
            proc_regionfilename(getpid(), addr, buf, sizeof buf);
            if (read_bytes(addr, hdr, sizeof hdr)) {
                uint32_t magic = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
                uint32_t cpu = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
                int arm64hdr = magic == 0xFEEDFACF && cpu == 0x0100000C;
                if (arm64hdr) {
                    logf("rosetta image %016llx-%016llx %s\n", (unsigned long long)addr,
                         (unsigned long long)(addr + size), buf[0] ? buf : "(no name)");
                    return addr;
                }
                if (!fallback && buf[0] && strstr(buf, "libRosettaRuntime")) fallback = addr;
            }
        }
        addr += size;
    }
    return fallback;
}

int main(int argc, char **argv) {
    PS = getpagesize();
    int maxtier = argc > 1 ? atoi(argv[1]) : 1;
    if (maxtier < 1 || maxtier > 5) maxtier = 1;
    g_fd = open("/tmp/rosetta-re/cowtest.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("/tmp/rosetta-re/cowtest.log"); return 2; }
    logf("=== cowtest pid %d tiers 1..%d\n", getpid(), maxtier);

    int rc = 0;
    mach_vm_address_t p;

    p = linkedit_page();
    if (maxtier >= 1) {
        if (!p) logf("tier 1: no __LINKEDIT found, skipping\n");
        else rc |= cow_cycle(1, "own __LINKEDIT r-- page", p, 0, 0, 0);
    }
    if (!rc && maxtier >= 2) {
        p = own_file_exec_page();
        if (!p) logf("tier 2: private exec mmap failed, skipping\n");
        else rc |= cow_cycle(2, "own binary, private PROT_EXEC file mapping", p, 0x800, 0, 0);
    }
    if (!rc && maxtier >= 3) {
        mach_vm_address_t rb = rosetta_text_base();
        if (!rb) { logf("tier 3: libRosettaRuntime mapping not found\n"); rc = 1; }
        else {
            rc |= cow_cycle(3, "rosetta __TEXT data page 0x60000", rb + 0x60000, 0x800, 0, 0);
            if (!rc && maxtier >= 4)
                rc |= cow_cycle(4, "rosetta __TEXT code page 0x4e000 (runtime_entry)", rb + 0x4e000, 0x800, 0, 0);
            if (!rc && maxtier >= 5)
                rc |= cow_cycle(5, "rosetta __TEXT data page 0x60000, modified byte", rb + 0x60000, 0x800, 1, 5);
        }
    }
    logf("=== cowtest done rc=%d\n", rc);
    return rc;
}
