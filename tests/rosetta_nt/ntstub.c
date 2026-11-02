/* ntstub.c — milestone 4, round 8: the NT fixups, live (x17 bit 5).
 *
 * Round history (full detail in NOTES.md):
 *  r1  call-site bl patches — counters 0 (predates the eager-translation
 *      discovery: static code is pre-translated before main).
 *  r2  emitter-entry patches + counter trampolines on a PLAIN anon page —
 *      HUNG (killable mach wait): instruction fetch from an unflagged anon
 *      page wedges the translator mid-translation.
 *  r3  pure trampoline on the plain anon page — still hung (same cause).
 *  r4  inline trampolines in dead cold blocks on the COW'd page — SIGSEGV
 *      before install: OUR stack smash (byte offsets used as word indices
 *      into a 0x480 array, ~0xB80 bytes past the end). Rosetta exonerated.
 *  r5  16-byte abs-jump entries + counter stubs on a MAP_TRANSLATED_
 *      ALLOW_EXECUTE page — one assert (pool-skip b off by one), then
 *      PASS: the pushf-gather emitter (0x2a7fc) hit 0->3 by a fresh-page
 *      trigger, restore verified. Labels corrected: 0x2a7fc is the pushf
 *      GATHER; 0x2abd0 is neither pushf nor popf (0 hits); the popf scatter
 *      emitter is still unidentified; translate_insn is 0x19d9c.
 *  r6  Hook translate_insn (the per-instruction choke point, per the
 *      MIT-licensed Lifeisawful/rosettax87_jit architecture — cleared for
 *      study by Rasmus 2026-10-31) with a stub that bumps a counter and
 *      captures x0 (the translation ctx) into the data page, then runs the
 *      displaced prologue and returns into the original. Result: ti 0->8,
 *      ctx captured; buffer object is allocator-adjacent (ctx+0x10 by
 *      address), not a field.
 *  r7  Append-ring hook at 0x18a0: 146-word emission trace captured
 *      (append-ring.bin) — the full pushf-gather and popf-scatter shapes,
 *      register-generic; NT bit 14 absent by construction on both sides.
 *      One crash from two hand-mis-encoded stub words; all hand words are
 *      now clang+otool-verified as a standing rule.
 *  r8  THIS. The NT fixups themselves: a freestanding arm64 C payload
 *      (payload_arm64.c -> payload.dylib -> payload_blob.h) flat-copied
 *      into a flagged region; the append stub (appendstub.S -> append_
 *      blob.h, assembler-built) calls ntfix_append(bufobj, word, ctl, ring)
 *      before the original helper, and the payload inserts
 *        lsr wT,w17,#5 ; bfi wD,wT,#14,#1   before each pushf push, and
 *        lsr wA,wS,#14 ; bfi x17,xA,#5,#1   before each popf msr NZCV,
 *      riding x17 bit 5 as the NT slot (bits 1-4 are Rosetta's DF/AC/ID/TF).
 *      The matcher is host-unit-tested against the recorded r7 ring
 *      (host_payload_test.c) before every deployment.
 *  r9  THIS. Checks 2/4, the exception context: a SIGTRAP handler records
 *      the ucontext EFLAGS delivered for an int3 raised with NT live in
 *      x17 bit 5 (check 2 — does Rosetta's NATIVE converter carry the bit,
 *      or only the JIT-emitted gather we fixed in r8?), then ORs 0x4000
 *      into the saved rflags and returns (check 4 — does the resume path
 *      write NT back so the pushfq after the trap reads it?). The context
 *      converter is ordinary Rosetta code, not emitted code, so the r8
 *      fixups are NOT expected to reach it; this round measures, not fixes.
 *
 * Safety envelope: UUID gate, entry-word anchors (gather 4 words, translate_
 * insn 4 words), region-clamped COW cycles for two 16-byte entry patches,
 * readback verification, full restore, DISABLE_AOT required. No sibling
 * freeze (task_threads never sees the exceptionserver; the reference project
 * patches live without it).
 *
 *   clang -arch x86_64 -O1 -Wall -o ntstub ntstub.c && DISABLE_AOT=1 ./ntstub
 * Controls: NOHOOK=1 (no patch, trigger only), EARLYDUMP=1 (startup dump).
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <libkern/OSCacheControl.h>

#ifndef MAP_TRANSLATED_ALLOW_EXECUTE
#define MAP_TRANSLATED_ALLOW_EXECUTE 0x2000
#endif

/* This build's fingerprint (fail-closed gate). */
static const uint8_t WANT_UUID[16] = {
    0xa1, 0xda, 0x2b, 0x2a, 0x3d, 0x68, 0x33, 0xcb,
    0xa6, 0x31, 0xfa, 0x77, 0x2d, 0xcd, 0xd1, 0x67};

/* /usr/libexec/rosetta/runtime — owns the x86-context compose/decompose
 * (r9; offsets are vm==file offsets, __TEXT vmaddr 0 fileoff 0). */
static const uint8_t RUNTIME_UUID[16] = {
    0xe9, 0x33, 0xbc, 0x84, 0xae, 0x96, 0x32, 0x5a,
    0xa3, 0x0e, 0x90, 0x38, 0x0d, 0xd2, 0x74, 0xeb};

/* File offsets inside libRosettaRuntime. */
#define OFF_GATHER_ENTRY  0x2a7fc   /* pushf flags-gather emitter (r5: live) */
#define OFF_TI_ENTRY      0x19d9c   /* translate_insn (choke point)          */
#define OFF_APPEND        0x18a0    /* append-one-word helper (all emission) */

/* Offsets inside /usr/libexec/rosetta/runtime (r9). */
#define OFF_COMPOSE       0x198b8   /* (nzcv, x17shadow) -> x86 EFLAGS      */
#define OFF_DECOMPOSE     0x1992c   /* x86 EFLAGS -> (nzcv, x17shadow)      */

/* The 4 prologue words each entry patch displaces (anchors + restore data). */
static const uint32_t PRO_GATHER[4] = {0xa9bc5ff8u, 0xa90157f6u, 0xa9024ff4u, 0xa9037bfdu};
static const uint32_t PRO_TI[4]     = {0xd10343ffu, 0xa9076ffcu, 0xa90867fau, 0xa9095ff8u};
static const uint32_t PRO_APPEND[4] = {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu, 0xaa0103f4u};
/* compose: lsr x8,x1,#2; lsr x9,x1,#1; cmn w0,#1; mov w10,#0x282
 * decompose: lsr x8,x0,#2; lsr x9,x0,#4; lsr x10,x0,#11; lsl x11,x0,#24 */
static const uint32_t PRO_COMPOSE[4]   = {0xd342fc28u, 0xd341fc29u, 0x3100041fu, 0x5280504au};
static const uint32_t PRO_DECOMPOSE[4] = {0xd342fc08u, 0xd344fc09u, 0xd34bfc0au, 0xd3689c0bu};

/* hand-assembled arm64 (x9/x10 are caller-saved scratch, dead at entry);
 * the append hook's stub is NOT hand-assembled — it is the assembler-built
 * blob from appendstub.S (append_blob.h) */
#define LDR_X9_LIT8   0x58000049u  /* ldr x9, #8   (literal at pc+8)      */
#define BR_X9         0xd61f0120u  /* br  x9                              */
#define B_PLUS_3      0x14000003u  /* b   +3 words (skip 8-byte literal)  */
#define LDR_X10_X9    0xf940012au  /* ldr x10, [x9]                       */
#define ADD_X10_1     0x9100054au  /* add x10, x10, #1                    */
#define STR_X10_X9    0xf900012au  /* str x10, [x9]                       */
#define STR_X0_X9_8   0xf9000520u  /* str x0, [x9, #8]  (capture ctx)     */

#include "payload_blob.h"           /* freestanding arm64 payload (r8)     */
#include "append_blob.h"            /* assembler-built append stub (r8)    */

#define STUB_GATHER_OFF 0x000       /* stub slots on the flagged page */
#define STUB_TI_OFF     0x100
#define STUB_APPEND_OFF 0x200
#define STUB_PAGE_LEN   0x4000      /* native page size on arm64 */

#define CTR_GATHER_OFF  0x100       /* slots on the plain-RW data page */
#define CTR_TI_OFF      0x108
#define CTX_TI_OFF      0x110
#define CTL_APPEND_OFF  0x120       /* payload ctl: [0]fix_g [1]fix_s [2]skip
                                     * [3]ring_at [4..11] last 8 events
                                     * [12] x18 (thread ctx) — written by the
                                     * append stub, read by the r10a probe */

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
            return want >= 0x1000 ? want : 0;
        }
        if (addr > page) return 0;
        addr += size;
    }
}

static int uuid_matches(mach_vm_address_t rb, const uint8_t want[16]) {
    static uint8_t buf[0x1000];
    if (!read_bytes(rb, buf, sizeof buf)) return 0;
    const struct mach_header_64 *h = (const struct mach_header_64 *)buf;
    const struct load_command *lc = (const struct load_command *)(h + 1);
    const uint8_t *end = buf + sizeof buf;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        if ((const uint8_t *)lc + lc->cmdsize > end) return 0;
        if (lc->cmd == LC_UUID) {
            const struct uuid_command *uc = (const struct uuid_command *)lc;
            return memcmp(uc->uuid, want, 16) == 0;
        }
        lc = (const struct load_command *)((const uint8_t *)lc + lc->cmdsize);
    }
    return 0;
}

/* UUID of a mapped image, or all-zero if unreadable */
static void image_uuid(mach_vm_address_t rb, uint8_t out[16]) {
    static uint8_t buf[0x1000];
    memset(out, 0, 16);
    if (!read_bytes(rb, buf, sizeof buf)) return;
    const struct mach_header_64 *h = (const struct mach_header_64 *)buf;
    const struct load_command *lc = (const struct load_command *)(h + 1);
    const uint8_t *end = buf + sizeof buf;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        if ((const uint8_t *)lc + lc->cmdsize > end) return;
        if (lc->cmd == LC_UUID) {
            memcpy(out, ((const struct uuid_command *)lc)->uuid, 16);
            return;
        }
        lc = (const struct load_command *)((const uint8_t *)lc + lc->cmdsize);
    }
}

/* Walk every executable mapping that starts with an arm64 Mach-O header and
 * return the one carrying the wanted UUID (several Rosetta images are arm64
 * Mach-O; only the UUID discriminates). */
static mach_vm_address_t find_image(const uint8_t want[16], const char *label) {
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    char buf[256];
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            return 0;
        if (info.is_submap) { depth++; continue; }
        if (info.protection & VM_PROT_EXECUTE) {
            unsigned char hdr[8];
            buf[0] = 0;
            proc_regionfilename(getpid(), addr, buf, sizeof buf);
            if (read_bytes(addr, hdr, sizeof hdr)) {
                uint32_t magic = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
                uint32_t cpu = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
                if (magic == 0xFEEDFACF && cpu == 0x0100000C) {
                    uint8_t u[16];
                    int d = -1, k;
                    image_uuid(addr, u);
                    for (k = 0; k < 16; k++) if (u[k] != want[k]) { d = k; break; }
                    logf("  find_image[%s] visits %016llx +%llx uuid "
                         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"
                         " want %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"
                         " first-diff %d (%02x vs %02x)\n",
                         label, (unsigned long long)addr, (unsigned long long)size,
                         u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                         u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15],
                         want[0], want[1], want[2], want[3], want[4], want[5], want[6], want[7],
                         want[8], want[9], want[10], want[11],
                         want[12], want[13], want[14], want[15],
                         d, d >= 0 ? u[d] : 0, d >= 0 ? want[d] : 0);
                    if (!memcmp(u, want, 16)) {
                        logf("%s at %016llx (%s)\n", label, (unsigned long long)addr,
                             buf[0] ? buf : "no name");
                        return addr;
                    }
                }
            }
        }
        addr += size;
    }
}

/* one line per in-process arm64 Mach-O executable mapping (diagnostic) */
static void log_image_inventory(const char *when) {
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    unsigned n = 0;
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        if (info.is_submap) { depth++; continue; }
        if (info.protection & VM_PROT_EXECUTE) {
            unsigned char hdr[8];
            if (read_bytes(addr, hdr, sizeof hdr)) {
                uint32_t magic = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
                uint32_t cpu = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
                if (magic == 0xFEEDFACF && cpu == 0x0100000C) {
                    uint8_t u[16];
                    image_uuid(addr, u);
                    logf("  image[%u] %016llx +%llx uuid %02x%02x%02x%02x-%02x%02x-%02x%02x"
                         "-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                         n++, (unsigned long long)addr, (unsigned long long)size,
                         u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                         u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
                }
            }
        }
        addr += size;
    }
    logf("image inventory %s: %u arm64 executable Mach-O mappings\n", when, n);
}

static mach_vm_address_t rosetta_image(void) { return find_image(WANT_UUID, "libRosettaRuntime"); }

/* The proven COW write: privatise, write, flush, restore protection. */
static int cow_write(mach_vm_address_t page, unsigned off, const void *data, unsigned len) {
    unsigned orig = prot_of(page);
    mach_vm_size_t win = clamp_to_region(page, 0x1000);
    if (!win) { logf("cow_write %llx: clamp FAILED\n", (unsigned long long)page); return 1; }
    kern_return_t kr = mach_vm_protect(mach_task_self(), page, win, FALSE,
                                       VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) { logf("cow_write %llx: RW|COPY kr=%d FAILED\n", (unsigned long long)page, kr); return 1; }
    memcpy((void *)(uintptr_t)(page + off), data, len);
    sys_icache_invalidate((void *)page, win);
    kr = mach_vm_protect(mach_task_self(), page, win, FALSE, orig);
    if (kr != KERN_SUCCESS) { logf("cow_write %llx: restore kr=%d FAILED\n", (unsigned long long)page, kr); return 1; }
    return 0;
}

/* Dump anonymous executable regions (the JIT translations) for offline
 * inspection. */
static void dump_jit(void) {
    mach_vm_address_t addr = 0;
    mach_vm_size_t size, total = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    char buf[256];
    void *jbuf = malloc(8u << 20);
    if (!jbuf) return;
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        if (info.is_submap) { depth++; continue; }
        buf[0] = 0;
        proc_regionfilename(getpid(), addr, buf, sizeof buf);
        if ((info.protection & VM_PROT_EXECUTE) && !buf[0] &&
            size <= (8u << 20) && total + size <= (16u << 20) &&
            read_bytes(addr, jbuf, size)) {
            char path[128];
            snprintf(path, sizeof path, "/tmp/rosetta-re/ntstub-jit-%016llx.bin",
                     (unsigned long long)addr);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { write(fd, jbuf, size); close(fd); total += size; }
        }
        addr += size;
    }
    free(jbuf);
    logf("jit dump: %llu bytes in /tmp/rosetta-re/ntstub-jit-*.bin\n",
         (unsigned long long)total);
}

/* --- fresh check code: brand-new anon pages, translated AFTER install ----- */
typedef unsigned long (*check_fn)(void);

/* r9 exception probe: run with NT live (x17 bit 5 via the r8 fixups), trap,
 * and look at the EFLAGS Rosetta's NATIVE arm->x86 converter put in the
 * ucontext — the emitted gather we fixed does not run on this path. Then
 * inject NT into the saved rflags and see whether the resume (x86->arm)
 * conversion writes it anywhere a later pushfq can read. */
static volatile unsigned long g_ctx_flags = ~0ul;

static void nt_sigtrap_handler(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
    g_ctx_flags = (unsigned long)uc->uc_mcontext->__ss.__rflags;
    uc->uc_mcontext->__ss.__rflags |= 0x4000;
}

/* r10b: the iretq #GP probe. Variant 3 sets NT (r8 scatter parks it in x17
 * bit 5), builds a full iret frame in the exact shape that produced the
 * ground-truth emission (jitdump marked_iret -> jit-3 @0x1b90), and iretqs.
 * With the r10b insert live the emitted stream traps BEFORE the thunk bl:
 * brk #3, fault code 0x91, the x86-bytes descriptor of the IRETQ itself as
 * the fault address (desc(iretq) = desc(lea target) - 2). The handler
 * records the delivered context and resumes at `after` (+31):
 * the JIT popped only 0x10 of the 0x28 frame by then (post-index ldp
 * advanced 0x10, the plain ldp nothing), so RSP is 0x18 low. */
static volatile unsigned long g_gp_count, g_gp_rip, g_gp_addr, g_gp_rsp,
    g_gp_flags, g_iret_after;
static int g_gp_resume;   /* R10B_RESUME=1 rip-only (jitdump-proven), =2 also rsp */

/* hex formatter for signal context — write(2)-only, async-signal-safe */
static void safelog_ul(char *p, unsigned long v) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) { p[15 - i] = hex[(v >> (i * 4)) & 15]; }
}

static void nt_sigsegv_handler(int sig, siginfo_t *si, void *uc_) {
    (void)sig;
    ucontext_t *uc = uc_;
    if (!g_gp_count) {
        g_gp_rip   = (unsigned long)uc->uc_mcontext->__ss.__rip;
        g_gp_addr  = (unsigned long)si->si_addr;
        g_gp_rsp   = (unsigned long)uc->uc_mcontext->__ss.__rsp;
        g_gp_flags = (unsigned long)uc->uc_mcontext->__ss.__rflags;
        g_gp_count = 1;
        /* ground truth first: log the delivered context raw; only then try
         * to resume, so a bad resume model can never mask what the fault
         * actually was */
        {
            char b[160];
            int n = 0;
            const char *tag = "GP rip";
            while (*tag) b[n++] = *tag++;
            safelog_ul(b + n, g_gp_rip); n += 16;
            b[n++] = ' '; b[n++] = 't'; b[n++] = 'o';
            safelog_ul(b + n, g_iret_after); n += 16;
            b[n++] = ' '; b[n++] = 'r'; b[n++] = 's'; b[n++] = 'p';
            safelog_ul(b + n, g_gp_rsp); n += 16;
            b[n++] = ' '; b[n++] = 'f'; b[n++] = 'l';
            safelog_ul(b + n, g_gp_flags); n += 16;
            /* the frame slots at rsp, +8, +0x10, +0x18 — settles which
             * selector/slot is where (both a CS and SS read as 0x2b here) */
            for (int k = 0; k < 4; k++) {
                unsigned long v = 0xdeade0de;
                memcpy(&v, (void *)(g_gp_rsp + k * 8), 8);
                b[n++] = ' '; b[n++] = 's' + k;
                safelog_ul(b + n, v); n += 16;
            }
            b[n++] = '\n';
            write(2, b, n);
        }
        if (!g_gp_resume) _exit(70);
    } else {
        /* second fault inside the resume experiment: log and bail loud */
        char b[96];
        int n = 0;
        const char *tag = "GP2 rip";
        while (*tag && n < 80) b[n++] = *tag++;
        safelog_ul(b + n, (unsigned long)uc->uc_mcontext->__ss.__rip); n += 16;
        b[n++] = ' '; b[n++] = 'r'; b[n++] = 's'; b[n++] = 'p';
        safelog_ul(b + n, (unsigned long)uc->uc_mcontext->__ss.__rsp); n += 16;
        b[n++] = '\n';
        write(2, b, n);
        _exit(77);
    }
    /* resume = rip override ONLY. Rosetta's sigreturn ignores __rsp edits
     * (measured, r10b stage 1 vs 2), so the target is the blob's fault
     * shim, which pops the one frame slot the JIT left (delivered rsp =
     * entry-0x8, the SS slot) itself. Flags stay as delivered: NT set is
     * the TRUTHFUL context for a #GP on iretq. */
    uc->uc_mcontext->__ss.__rip = g_iret_after;
}

static check_fn fresh_check(int variant) {
    /* variant 0: immediate encodings */
    static const unsigned char a[] = {
        0x9c,                                        /* pushfq */
        0x48, 0x81, 0x0c, 0x24, 0x00, 0x40, 0x00, 0x00, /* orq $0x4000,(%rsp) */
        0x9d,                                        /* popfq */
        0x9c, 0x58,                                  /* pushfq; popq %rax */
        0x9c,                                        /* pushfq */
        0x48, 0x81, 0x24, 0x24, 0xff, 0xbf, 0xff, 0xff, /* andq $~0x4000,(%rsp) */
        0x9d, 0xc3,                                  /* popfq; ret */
    };
    /* variant 1: register encodings via rcx — different bytes so the two
     * blocks can never share a translation */
    static const unsigned char b[] = {
        0x9c,
        0x48, 0xb8, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* movabs $0x4000,%rax */
        0x48, 0x09, 0x04, 0x24,                   /* orq %rax,(%rsp) */
        0x9d,
        0x9c, 0x59,                                /* pushfq; popq %rcx */
        0x9c,
        0x48, 0xb8, 0xff, 0xbf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* movabs $~0x4000 */
        0x48, 0x21, 0x04, 0x24,                   /* andq %rax,(%rsp) */
        0x9d,
        0x48, 0x89, 0xc8, 0xc3,                    /* movq %rcx,%rax; ret */
    };
    /* variant 2 (r9): NT set -> int3 -> flags read back AFTER the resume.
     * Different first bytes than a/b so the block can never share one. */
    static const unsigned char c[] = {
        0x9c,                                        /* pushfq */
        0x48, 0x81, 0x0c, 0x24, 0x00, 0x40, 0x00, 0x00, /* orq $0x4000,(%rsp) */
        0x9d,                                        /* popfq (NT -> x17 bit 5) */
        0xcc,                                        /* int3 -> nt_sigtrap_handler */
        0x9c, 0x58,                                  /* pushfq; popq %rax */
        0xc3,
    };
    /* variant 3 (r10b): full iret frame + iretq with NT live — the exact
     * shape of jitdump's marked_iret, so the emission matches the recorded
     * ground truth (jit-3 @0x1b90). Offsets: iretq at +29, after at +31;
     * the lea's rel32 is 3 (rip after lea = +28). At +34 the FAULT-SHIM.
     * Measured (r10b stage runs): the #GP arrives with rsp AT THE INTACT
     * FRAME TOP (entry-0x28; slots RIP,CS=2b,RFLAGS=4206,RSP,SS — the JIT's
     * register pops never advance the delivered rsp), and Rosetta's
     * sigreturn honors __rip but ignores __rsp edits. So the shim discards
     * the whole frame itself — same shape a Windows handler would see. */
    static const unsigned char d[] = {
        0x9c,                                        /* pushfq                  */
        0x48, 0x81, 0x0c, 0x24, 0x00, 0x40, 0x00, 0x00, /* orq $0x4000,(%rsp)   */
        0x9d,                                        /* popfq (NT -> x17 bit 5) */
        0x48, 0x89, 0xe0,                            /* movq %rsp,%rax          */
        0x8c, 0xd9,                                  /* movl %ss,%ecx           */
        0x51,                                        /* pushq %rcx (SS)         */
        0x50,                                        /* pushq %rax (RSP)        */
        0x9c,                                        /* pushfq   (RFLAGS)       */
        0x8c, 0xc9,                                  /* movl %cs,%ecx           */
        0x51,                                        /* pushq %rcx (CS)         */
        0x48, 0x8d, 0x0d, 0x03, 0x00, 0x00, 0x00,    /* lea after(%rip),%rcx    */
        0x51,                                        /* pushq %rcx (RIP)        */
        0x48, 0xcf,                                  /* iretq  <- #GP expected  */
        0x31, 0xc0,                                  /* after: xorl %eax,%eax   */
        0xc3,                                        /* ret (no-fault path)     */
        0x31, 0xc0,                                  /* +34 SHIM: xorl %eax,%eax*/
        0x48, 0x83, 0xc4, 0x28,                      /*        addq $0x28,%rsp  */
        0xc3,                                        /*        ret              */
    };
    const unsigned char *src = variant == 1 ? b : variant == 2 ? c : variant == 3 ? d : a;
    unsigned n = variant == 1 ? sizeof b : variant == 2 ? sizeof c
               : variant == 3 ? sizeof d : sizeof a;
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    memcpy(p, src, n);
    if (mprotect(p, 0x1000, PROT_READ | PROT_EXEC) != 0) return NULL;
    return (check_fn)p;
}

/* Early-dump timing probe (see header): is our check code pre-translated? */
static void earlydump_and_exit(void) {
    dump_jit();
    void *jbuf = malloc(16u << 20);
    mach_vm_address_t a = 0;
    mach_vm_size_t sz, found = 0;
    vm_region_submap_info_data_64_t info2;
    mach_msg_type_number_t c2;
    natural_t d2 = 0;
    for (;;) {
        c2 = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &a, &sz, &d2,
                                   (vm_region_recurse_info_t)&info2, &c2) != KERN_SUCCESS)
            break;
        if (info2.is_submap) { d2++; continue; }
        char nm[64] = {0};
        proc_regionfilename(getpid(), a, nm, sizeof nm);
        if ((info2.protection & VM_PROT_EXECUTE) && !nm[0] && sz <= (8u << 20) &&
            read_bytes(a, jbuf, sz)) {
            /* pushfq;orq $0x4000;(rsp);popfq as emitted: str x22,[x4,#-8]! /
             * ldr x23,[x4] / orr x22,x23,#0x4000 / str x22,[x4] / ldr x22,[x4],#8 */
            static const uint32_t fp[5] = {0xf81f8c96u, 0xf9400097u, 0xb27202f6u,
                                           0xf9000096u, 0xf8408496u};
            for (mach_vm_size_t o = 0; o + 20 <= sz; o++) {
                if (!memcmp((char *)jbuf + o, fp, sizeof fp)) {
                    logf("EARLY fingerprint @ %016llx+%llx\n",
                         (unsigned long long)a, (unsigned long long)o);
                    found++;
                }
            }
        }
        a += sz;
    }
    free(jbuf);
    logf("=== EARLYDUMP verdict: %llu fingerprint sites pre-call %s\n",
         (unsigned long long)found, found ? "(EAGER TRANSLATION CONFIRMED)" : "(no pre-translation)");
}

/* Build a counter stub at spage+slot:
 *   ldr x9,(counter literal); b +3; .quad counter;
 *   ldr x10,[x9]; add x10,x10,#1; str x10,[x9];
 *   [capture] str x0,[x9,#8]
 *   <4 displaced prologue words>;
 *   ldr x9,(back literal); br x9; .quad entry+16
 */
static void build_stub(uint32_t *s, uint64_t counter_addr, uintptr_t entry,
                       const uint32_t pro[4], int capture_x0) {
    s[0]  = LDR_X9_LIT8;
    s[1]  = B_PLUS_3;
    *(uint64_t *)&s[2] = counter_addr;
    s[4]  = LDR_X10_X9;
    s[5]  = ADD_X10_1;
    s[6]  = STR_X10_X9;
    s[7]  = capture_x0 ? STR_X0_X9_8 : pro[0];
    if (capture_x0) { s[8] = pro[0]; s[9] = pro[1]; s[10] = pro[2]; s[11] = pro[3]; }
    else            { s[8] = pro[1]; s[9] = pro[2]; s[10] = pro[3]; }
    if (capture_x0) {
        s[12] = LDR_X9_LIT8; s[13] = BR_X9;
        *(uint64_t *)&s[14] = entry + 16;
    } else {
        s[11] = LDR_X9_LIT8; s[12] = BR_X9;
        *(uint64_t *)&s[13] = entry + 16;
    }
}

/* Map the freestanding arm64 payload (payload_blob.h, generated from
 * payload.dylib by mkpayload.py, verified fixup/reloc-free at build time)
 * into one flagged region, layout-preserving: __TEXT (first 16K) sealed RX
 * after the copy + icache flush, __DATA (second 16K) stays RW for the
 * payload's bss statics. Entry first-words are readback-verified fail-closed.
 * Nothing here calls the payload from translated code — the append stub is
 * its only entry (that path is Rosetta-side, proven since r5b). */
static uint32_t *map_payload(void) {
    uint32_t *p = mmap(NULL, PAYLOAD_SPAN, PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE | MAP_TRANSLATED_ALLOW_EXECUTE, -1, 0);
    if (p == MAP_FAILED) { logf("=== abort: payload mmap FAILED errno=%d\n", errno); return NULL; }
    memcpy(p, payload_text, PAYLOAD_TEXT_WORDS * 4);
    sys_dcache_flush(p, PAYLOAD_TEXT_WORDS * 4);
    sys_icache_invalidate(p, PAYLOAD_TEXT_WORDS * 4);
    if (mach_vm_protect(mach_task_self(), (mach_vm_address_t)p, 0x4000, FALSE,
                        VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
        logf("=== abort: payload TEXT R|X FAILED\n");
        return NULL;
    }
    if (p[P_NTFIX_APPEND_OFF / 4] != P_NTFIX_APPEND_W0 ||
        p[P_NTFIX_INIT_OFF / 4] != P_NTFIX_INIT_W0 ||
        p[P_NTFIX_COMPOSE_OFF / 4] != P_NTFIX_COMPOSE_W0 ||
        p[P_NTFIX_DECOMPOSE_OFF / 4] != P_NTFIX_DECOMPOSE_W0) {
        logf("=== fail-closed: payload entries %08x/%08x/%08x/%08x unexpected "
             "(want %08x/%08x/%08x/%08x)\n",
             p[P_NTFIX_APPEND_OFF / 4], p[P_NTFIX_INIT_OFF / 4],
             p[P_NTFIX_COMPOSE_OFF / 4], p[P_NTFIX_DECOMPOSE_OFF / 4],
             P_NTFIX_APPEND_W0, P_NTFIX_INIT_W0,
             P_NTFIX_COMPOSE_W0, P_NTFIX_DECOMPOSE_W0);
        return NULL;
    }
    logf("payload mapped at %p (span %#x: TEXT RX, DATA RW)\n", (void *)p,
         (unsigned)PAYLOAD_SPAN);
    return p;
}

/* One 16-byte absolute jump over `entry` */
static int patch_abs_jump(mach_vm_address_t rb, uintptr_t entry_off, void *stub) {
    uint32_t jump[4] = {LDR_X9_LIT8, BR_X9, 0, 0};
    *(uint64_t *)&jump[2] = (uint64_t)stub;
    int rc = cow_write(rb + (entry_off & ~0xffful), entry_off & 0xfff, jump, 16);
    uint32_t got[4];
    read_bytes(rb + entry_off, got, sizeof got);
    if (rc || memcmp(got, jump, sizeof got)) {
        logf("=== install FAILED @%llx (rc=%d readback %08x)\n",
             (unsigned long long)entry_off, rc, got[0]);
        return 1;
    }
    return 0;
}

int main(void) {
    g_fd = open("/tmp/rosetta-re/ntstub.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("/tmp/rosetta-re/ntstub.log"); return 2; }
    logf("=== ntstub pid %d (round 9: exception-context carry, checks 2/4)\n", getpid());

    if (!getenv("DISABLE_AOT")) {
        logf("=== REFUSING: run with DISABLE_AOT=1 (cached translations bypass the hook)\n");
        return 2;
    }

    /* HOOKLIB=1: the injectable dylib (hooklib.c) already installed the
     * append hook and — when its image was mapped — the converters, at dyld
     * time, before main. This run only exercises the checks against that
     * install: no patching, no restore, counters via the dylib's exports.
     * Launch: DYLD_INSERT_LIBRARIES=hooklib.dylib DISABLE_AOT=1 HOOKLIB=1 ... */
    int hooklib = 0;
    uint64_t *hctl = NULL;
    uint32_t *hring = NULL;
    int (*hook_installed)(void) = NULL;
    if (getenv("HOOKLIB")) {
        uint64_t *(*cf)(void) = (uint64_t *(*)(void))dlsym(RTLD_DEFAULT, "macfinals_ctl");
        const uint32_t *(*rf)(void) =
            (const uint32_t *(*)(void))dlsym(RTLD_DEFAULT, "macfinals_ring");
        hook_installed = (int (*)(void))dlsym(RTLD_DEFAULT, "macfinals_installed");
        if (!cf || !cf()) {
            logf("=== abort: HOOKLIB set but macfinals_ctl() is NULL — dylib not injected?\n");
            return 2;
        }
        hctl = cf();
        hring = rf ? (uint32_t *)rf() : NULL;
        hooklib = 1;
        logf("HOOKLIB mode: dylib reports installed=%d, ctl %p — checks only\n",
             hook_installed ? hook_installed() : -1, (void *)hctl);
    }

    mach_vm_address_t rb = 0;
    if (!hooklib) {
        rb = rosetta_image();
        if (!rb) { logf("=== no UUID-matching libRosettaRuntime mapping; abort\n"); return 2; }
    }

    if (getenv("EARLYDUMP")) { earlydump_and_exit(); return 0; }

    /* anchors, pages and installs — the lab path. HOOKLIB mode skips all of
     * it: the dylib installed at dyld time; this process only checks. */
    uint32_t v[4] = {0, 0, 0, 0};
    char *dp = NULL;
    uint32_t *ring = hring;
    uint32_t *payload = NULL;
    mach_vm_address_t rt = 0;
    int cv_patched = 0;
    if (!hooklib) {
        /* anchors: all words the patches will displace must match exactly */
        read_bytes(rb + OFF_GATHER_ENTRY, v, sizeof v);
        if (memcmp(v, PRO_GATHER, sizeof v)) {
            logf("=== fail-closed: gather prologue %08x %08x %08x %08x unexpected\n",
                 v[0], v[1], v[2], v[3]);
            return 2;
        }
        read_bytes(rb + OFF_TI_ENTRY, v, sizeof v);
        if (memcmp(v, PRO_TI, sizeof v)) {
            logf("=== fail-closed: translate_insn prologue %08x %08x %08x %08x unexpected\n",
                 v[0], v[1], v[2], v[3]);
            return 2;
        }
        read_bytes(rb + OFF_APPEND, v, sizeof v);
        if (memcmp(v, PRO_APPEND, sizeof v)) {
            logf("=== fail-closed: append prologue %08x %08x %08x %08x unexpected\n",
                 v[0], v[1], v[2], v[3]);
            return 2;
        }
        logf("anchors: gather + translate_insn + append prologues verified (12 words)\n");

        /* counters/ctx: plain RW page — data only, never executed from */
        dp = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (dp == MAP_FAILED) { logf("=== abort: data page FAILED\n"); return 2; }
        *(uint64_t *)(dp + CTR_GATHER_OFF) = 0;
        *(uint64_t *)(dp + CTR_TI_OFF) = 0;
        *(uint64_t *)(dp + CTX_TI_OFF) = 0;
        memset(dp + CTL_APPEND_OFF, 0, 12 * 8);          /* payload ctl block */
        ring = mmap(NULL, 0x80000, PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE, -1, 0);
        if (ring == MAP_FAILED) { logf("=== abort: ring mmap FAILED\n"); return 2; }

        payload = map_payload();
        if (!payload) return 2;

        /* stubs: the ONLY executable anon pages — flagged for native arm64 */
        uint32_t *sp = mmap(NULL, STUB_PAGE_LEN, PROT_READ | PROT_WRITE,
                            MAP_ANON | MAP_PRIVATE | MAP_TRANSLATED_ALLOW_EXECUTE, -1, 0);
        if (sp == MAP_FAILED) { logf("=== abort: stub page mmap FAILED errno=%d\n", errno); return 2; }
        build_stub(sp + STUB_GATHER_OFF / 4, (uint64_t)(dp + CTR_GATHER_OFF),
                   rb + OFF_GATHER_ENTRY, PRO_GATHER, 0);
        build_stub(sp + STUB_TI_OFF / 4, (uint64_t)(dp + CTR_TI_OFF),
                   rb + OFF_TI_ENTRY, PRO_TI, 1);
        /* append stub: assembler-built blob (appendstub.S); patch its four
         * placeholder quads by value, fail-closed if any is not in place */
        memcpy(sp + STUB_APPEND_OFF / 4, stub_blob, STUB_LEN * 4);
        {
            uint32_t *s = sp + STUB_APPEND_OFF / 4;
            uint64_t want[4] = {0xDEAD000000000001ull, 0xDEAD000000000003ull,
                                0xDEAD000000000004ull, 0xDEAD000000000002ull};
            uint64_t vals[4] = {(uint64_t)(payload + P_NTFIX_APPEND_OFF / 4),
                                (uint64_t)(dp + CTL_APPEND_OFF), (uint64_t)ring,
                                rb + OFF_APPEND + 16};
            unsigned offs[4] = {PCALL_OFF, PCTL_OFF, PRING_OFF, PBACK_OFF};
            for (int i = 0; i < 4; i++) {
                if (*(uint64_t *)&s[offs[i]] != want[i]) {
                    logf("=== fail-closed: append stub slot %d (%08llx) unexpected\n",
                         i, (unsigned long long)*(uint64_t *)&s[offs[i]]);
                    return 2;
                }
                *(uint64_t *)&s[offs[i]] = vals[i];
            }
        }
        logf("append stub: %u words, pcall->payload+%#x, pback->entry+0x10\n",
             STUB_LEN, P_NTFIX_APPEND_OFF);
        logf("ti stub: %08x %08x %08x %08x %08x %08x %08x %08x | back %08x %08x\n",
             sp[STUB_TI_OFF / 4 + 0], sp[STUB_TI_OFF / 4 + 1], sp[STUB_TI_OFF / 4 + 2],
             sp[STUB_TI_OFF / 4 + 3], sp[STUB_TI_OFF / 4 + 4], sp[STUB_TI_OFF / 4 + 5],
             sp[STUB_TI_OFF / 4 + 6], sp[STUB_TI_OFF / 4 + 7],
             sp[STUB_TI_OFF / 4 + 12], sp[STUB_TI_OFF / 4 + 13]);
        sys_dcache_flush(sp, STUB_PAGE_LEN);
        sys_icache_invalidate(sp, STUB_PAGE_LEN);
        if (mach_vm_protect(mach_task_self(), (mach_vm_address_t)sp, STUB_PAGE_LEN, FALSE,
                            VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
            logf("=== abort: stub page R|X FAILED\n");
            return 2;
        }
        logf("stub page %p sealed R|X (flagged)\n", (void *)sp);

        /* install the three 16-byte abs jumps */
        if (getenv("NOHOOK")) {
            logf("NOHOOK: control run — nothing patched\n");
        } else {
            if (patch_abs_jump(rb, OFF_APPEND, sp + STUB_APPEND_OFF / 4)) return 1;
            if (patch_abs_jump(rb, OFF_GATHER_ENTRY, sp + STUB_GATHER_OFF / 4)) return 1;
            if (patch_abs_jump(rb, OFF_TI_ENTRY, sp + STUB_TI_OFF / 4)) return 1;
            logf("entries patched (append, gather, ti), readback verified\n");
        }

        /* r9c: the context converters in /usr/libexec/rosetta/runtime — full
         * replacements, so the abs jump does NOT chain back; ntfix_compose/
         * ntfix_decompose simply return to the original caller */
        rt = find_image(RUNTIME_UUID, "rosetta/runtime");
        if (getenv("NOHOOK")) {
            logf("NOHOOK: converters left alone\n");
        } else if (!rt) {
            logf("=== r9 SKIP: no UUID-matching rosetta/runtime mapping in-process\n");
        } else {
            read_bytes(rt + OFF_COMPOSE, v, sizeof v);
            if (memcmp(v, PRO_COMPOSE, sizeof v)) {
                logf("=== fail-closed: compose prologue %08x %08x %08x %08x unexpected\n",
                     v[0], v[1], v[2], v[3]);
                return 2;
            }
            read_bytes(rt + OFF_DECOMPOSE, v, sizeof v);
            if (memcmp(v, PRO_DECOMPOSE, sizeof v)) {
                logf("=== fail-closed: decompose prologue %08x %08x %08x %08x unexpected\n",
                     v[0], v[1], v[2], v[3]);
                return 2;
            }
            if (patch_abs_jump(rt, OFF_COMPOSE, payload + P_NTFIX_COMPOSE_OFF / 4)) return 1;
            if (patch_abs_jump(rt, OFF_DECOMPOSE, payload + P_NTFIX_DECOMPOSE_OFF / 4)) return 1;
            cv_patched = 1;
            logf("converters patched at runtime image %016llx, readback verified\n",
                 (unsigned long long)rt);
        }
    } else {
        /* diagnostics only: r10a wants the image address even though the
         * dylib did the patching (or its poll thread still is) */
        rt = find_image(RUNTIME_UUID, "rosetta/runtime (hooklib)");
    }

    /* r9: catch the variant-2 int3; record context flags, inject NT */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = nt_sigtrap_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGTRAP, &sa, NULL) != 0)
            logf("sigaction SIGTRAP FAILED errno=%d — exception check void\n", errno);
    }

    /* trigger: fresh anon x86 page — translated NOW, on this thread */
    check_fn ck = fresh_check(0);
    if (!ck) { logf("=== abort: fresh page alloc FAILED\n"); return 2; }
    uint64_t g0 = 0, t0 = 0, ctx = 0;
    if (dp) {
        g0 = *(uint64_t *)(dp + CTR_GATHER_OFF);
        t0 = *(uint64_t *)(dp + CTR_TI_OFF);
    }
    logf("calling fresh page at %p (%s)\n", (void *)ck,
         hooklib ? "hooklib install: gather/ti counter hooks not installed by design"
                 : "gather/ti counters live");
    unsigned long r = ck();
    uint64_t g1 = g0, t1 = t0;
    if (dp) {
        g1 = *(uint64_t *)(dp + CTR_GATHER_OFF);
        t1 = *(uint64_t *)(dp + CTR_TI_OFF);
        ctx = *(uint64_t *)(dp + CTX_TI_OFF);
    }
    logf("fresh check: NT=%lu (1 expected WITH fixups, 0 without) | gather %llu->%llu, ti %llu->%llu, ctx %016llx\n",
         (r >> 14) & 1, (unsigned long long)g0, (unsigned long long)g1,
         (unsigned long long)t0, (unsigned long long)t1, (unsigned long long)ctx);

    /* snapshot the append ring NOW, before post-trigger code translates more */
    uint64_t *ctl = hooklib ? hctl : (uint64_t *)(dp + CTL_APPEND_OFF);
    unsigned nwords = (unsigned)(ctl[3] & 0xffffffffull);
    if (nwords > 0x20000) nwords = 0x20000;
    logf("payload: gather fixups %llu, scatter fixups %llu, skipped %llu; ring %u words\n",
         (unsigned long long)ctl[0], (unsigned long long)ctl[1],
         (unsigned long long)ctl[2], nwords);
    for (int i = 4; i < 12; i++) {
        if (ctl[i])
            logf("  event[%d]: %s @ring %llu\n", i - 4,
                 ctl[i] >> 32 ? ((ctl[i] >> 32) == 1 ? "gather" : "scatter") : "?",
                 (unsigned long long)(ctl[i] & 0xffffffffull));
    }

    /* r10a: the IRETQ escape, measured. The append stub also stashes x18
     * (ctl[12]) — the Rosetta THREAD context while translated code runs.
     * The iret thunk saves regs there and its tail calls [ [ctx] + 0x28 ]
     * with x0 = &ctx+0x10: that slot is the native IRET handler. Follow the
     * chain, verify it against the runtime image, and record the offset —
     * pure measurement, nothing is patched. */
    if (!getenv("NOHOOK")) {
        uint64_t tctx = ctl[12] & ~(1ull << 63);   /* thunk tags bit 63: untag */
        uint64_t hdr[16] = {0};
        logf("r10a: x18 (thread ctx) = %016llx, prot %x\n",
             (unsigned long long)tctx, prot_of(tctx));
        if (tctx > 0x1000 && read_bytes(tctx, hdr, sizeof hdr)) {
            uint64_t nzcv = 0, shadow = 0, x30v = 0;
            read_bytes(tctx + 0x90, &nzcv, 8);
            read_bytes(tctx + 0x98, &shadow, 8);
            read_bytes(tctx + 0x08, &x30v, 8);
            logf("  ctx: [0]=%016llx (table?) [8]=%016llx (x30?) nzcv %llx shadow %llx\n",
                 (unsigned long long)hdr[0], (unsigned long long)x30v,
                 (unsigned long long)nzcv, (unsigned long long)shadow);
            uint64_t table = hdr[0], slots[9] = {0};
            if (table > 0x1000 && read_bytes(table, slots, sizeof slots)) {
                for (int i = 0; i < 9; i++)
                    logf("  table+%03x: %016llx%s\n", i * 8,
                         (unsigned long long)slots[i], i == 5 ? "  <- iret handler" : "");
                uint64_t handler = slots[5];
                if (rt && handler >= rt && handler < rt + 0x30000) {
                    uint32_t hw[6] = {0};
                    read_bytes(handler, hw, sizeof hw);
                    logf("r10a: iret handler %016llx = runtime+%llx; prot %x; first words "
                         "%08x %08x %08x %08x\n",
                         (unsigned long long)handler,
                         (unsigned long long)(handler - rt), prot_of(handler),
                         hw[0], hw[1], hw[2], hw[3]);
                } else if (handler) {
                    logf("r10a: handler %016llx NOT inside runtime image %016llx\n",
                         (unsigned long long)handler, (unsigned long long)rt);
                }
                /* the handler's bss continuation slots (invalid/valid CS paths
                 * store one of these to ctx+0x198; the thunk tail jumps there) */
                if (rt) {
                    uint64_t cs = 0;
                    for (int k = 0; k < 2; k++) {
                        uint64_t slot = 0, cont = 0;
                        uint32_t cw[4] = {0};
                        if (!read_bytes(rt + 0x3b5a0 + k * 0x220, &slot, 8)) break;
                        if (slot >= rt && slot < rt + 0x30000)
                            read_bytes(slot, cw, sizeof cw);
                        else if (slot) read_bytes(slot, cw, 4);
                        cs = slot;
                        logf("r10a: cont slot runtime+%llx -> %016llx (%s, first %08x %08x)\n",
                             (unsigned long long)(0x3b5a0 + k * 0x220),
                             (unsigned long long)cs,
                             cs >= rt && cs < rt + 0x30000 ? "runtime image" : "outside",
                             cw[0], cw[1]);
                    }
                }
            } else {
                logf("r10a: table at %016llx unreadable\n", (unsigned long long)table);
            }
        }
    }


    /* the trigger TB itself, before later translation evicts it: the buffer
     * object is allocator-adjacent to the captured ctx (r6/r7: bufobj address
     * == ctx + 0x10), and its {base,cursor} span is the emitted stream our
     * fixups were inserted into — dump and scan it for the five pairs */
    if (ctx && !getenv("NOHOOK")) {
        uint64_t bobj[3] = {0, 0, 0};
        if (read_bytes(ctx + 0x10, bobj, sizeof bobj) && bobj[0] > 0x100000000ull &&
            bobj[1] < (1u << 20) && bobj[2] >= bobj[1] + 12) {
            uint64_t n = bobj[1];          /* cursor is a byte offset into base */
            uint32_t *tb = malloc(n);
            if (tb && read_bytes(bobj[0], tb, n)) {
                unsigned g = 0, s = 0;
                for (uint64_t i = 0; i + 2 < n / 4; i++) {
                    uint32_t a = tb[i], b = tb[i + 1], c = tb[i + 2];
                    if ((a & 0xfffffc00u) == 0x53057c00u && ((a >> 5) & 31) == 17 &&
                        (b & 0xfffffc00u) == 0x33120000u && (c & 0xffffffe0u) == 0xf81f8c80u) {
                        g++;
                        logf("  TB gather fixup @+%#llx: %08x %08x | push %08x\n",
                             (unsigned long long)i * 4, a, b, c);
                    }
                    if ((a & 0xfffffc00u) == 0x530e7c00u && (b & 0xfffffc00u) == 0xb37b0000u &&
                        (b & 31) == 17 && (c & 0xffffffe0u) == 0xd51b4200u) {
                        s++;
                        logf("  TB scatter fixup @+%#llx: %08x %08x | msr %08x\n",
                             (unsigned long long)i * 4, a, b, c);
                    }
                }
                logf("TB scan (%llu bytes): %u gather + %u scatter fixups present\n",
                     (unsigned long long)n, g, s);
                char path[64] = "/tmp/rosetta-re/tb-fixups.bin";
                int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { write(fd, tb, n); close(fd); logf("TB dumped to %s\n", path); }
            }
            free(tb);
        } else {
            logf("bufobj at ctx+%#llx not readable as {base,cursor,limit}\n",
                 (unsigned long long)ctx);
        }
    }
    if (nwords) {
        char path[64] = "/tmp/rosetta-re/append-ring.bin";
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { write(fd, ring, (size_t)nwords * 4); close(fd); }
        unsigned shown = 0;
        for (unsigned i = 0; i < nwords && shown < 6; i++) {
            uint32_t w = ring[i];
            int is_msr = (w & 0xfffff3e0u) == 0xd51b4200u;
            int is_mov = (w & 0xffe0001fu) == 0x52800000u &&
                         ((w >> 5) & 0xffffu) == 0x202u;
            if (!is_msr && !is_mov) continue;
            logf("  ring %s @%u:", is_msr ? "msr NZCV" : "movz 0x202", i);
            for (int j = -6; j <= 6; j++) {
                int k = (int)i + j;
                if (k >= 0 && (unsigned)k < nwords) {
                    if (j) dprintf(g_fd, " ");
                    dprintf(g_fd, "%08x", ring[k]);
                }
            }
            dprintf(g_fd, "\n");
            shown++;
        }
        if (!shown) logf("  (no msr/movz signature inside the ring)\n");
        logf("ring dumped to %s\n", path);
    }

    /* read the emission buffer object at ctx+0x10 ({base,cursor,limit}) and
     * scan the emitted tail for the two register-agnostic signatures */
    if (ctx && !getenv("NOHOOK")) {
        uint64_t bobj[3] = {0, 0, 0};
        if (read_bytes(ctx + 0x10, bobj, sizeof bobj) && bobj[0] && bobj[1] > bobj[0]) {
            uint64_t len = bobj[1] - bobj[0];
            logf("emission buffer: base %016llx cursor- %016llx (%llu bytes)\n",
                 (unsigned long long)bobj[0], (unsigned long long)bobj[1],
                 (unsigned long long)len);
            uint64_t n = len > 0x10000 ? 0x10000 : len;
            uint32_t *tail = malloc(n);
            if (tail && read_bytes(bobj[1] - n, tail, n)) {
                unsigned msr = 0, movz = 0;
                for (uint64_t i = 0; i < n / 4; i++) {
                    uint32_t w = tail[i];
                    if ((w & 0xfffff3e0u) == 0xd51b4200u) msr++;           /* msr NZCV,xN */
                    if ((w & 0xffe0001fu) == 0x52800000u &&
                        ((w >> 5) & 0xffffu) == 0x202u) movz++;             /* movz Wd,#0x202 */
                }
                logf("emitted tail scan (last %llu bytes): msr NZCV x%u, movz #0x202 x%u\n",
                     (unsigned long long)n, msr, movz);
                char path[64] = "/tmp/rosetta-re/ti-buffer.bin";
                int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { write(fd, tail, n); close(fd); logf("tail dumped to %s\n", path); }
            }
            free(tail);
        } else {
            logf("ctx+0x10 not a buffer; dumping 0x40 words at ctx %016llx\n",
                 (unsigned long long)ctx);
            uint64_t words[0x40] = {0};
            if (read_bytes(ctx, words, sizeof words)) {
                for (int i = 0; i < 0x40; i += 4)
                    logf("  ctx+%03x: %016llx %016llx %016llx %016llx\n", i * 8,
                         (unsigned long long)words[i], (unsigned long long)words[i + 1],
                         (unsigned long long)words[i + 2], (unsigned long long)words[i + 3]);
                /* heuristic: find a {base,cursor,limit} triple in the first 0x200 */
                for (int i = 0; i < 0x3d; i++) {
                    uint64_t base = words[i], cur = words[i + 1], lim = words[i + 2];
                    if (base > 0x100000000ull && cur > base && lim > cur &&
                        cur - base < (16ull << 20) && lim - cur < (64ull << 20)) {
                        logf("  candidate buffer object at ctx+%03x: base %016llx cursor %016llx limit %016llx (%llu bytes emitted)\n",
                             i * 8, (unsigned long long)base, (unsigned long long)cur,
                             (unsigned long long)lim, (unsigned long long)(cur - base));
                    }
                }
            } else {
                logf("  ctx itself unreadable\n");
            }
        }
    }
    /* r9 exception probe — after the TB/ring dumps so their buffers stay
     * pristine; translates blob c + the handler while hooks are live */
    check_fn ce = fresh_check(2);
    if (!ce) { logf("=== abort: r9 fresh page alloc FAILED\n"); return 2; }
    g_ctx_flags = ~0ul;
    {
        unsigned long rv = ce();
        logf("exception check #1 (converters as installed at startup): ctx flags %08lx (NT=%lu, check 2) | post-resume NT=%lu (check 4)%s\n",
             g_ctx_flags, (unsigned long)((g_ctx_flags >> 14) & 1),
             (unsigned long)((rv >> 14) & 1),
             g_ctx_flags == ~0ul ? " [HANDLER DID NOT RUN]" : "");
    }

    /* the ThreadContext machinery may be demand-mapped by the first
     * exception — rescan now, with a full image inventory */
    log_image_inventory("after exception #1");
    if (hooklib) {
        /* chicken-and-egg under the dylib: the converter image appears with
         * the first exception (the one check #1 just raised), and the dylib's
         * poll thread installs within ticks — wait for that, then check #2
         * is the decisive run through the dylib's own install path. */
        int waited = 0;
        while (hook_installed && !(hook_installed() & 2) && waited < 15000) {
            usleep(100000);
            waited += 100;
        }
        logf("HOOKLIB: converters %s (%d ms after exception #1)\n",
             hook_installed && (hook_installed() & 2) ? "installed" : "STILL ABSENT",
             waited);
    }
    if (!hooklib && !cv_patched && !getenv("NOHOOK")) {
        mach_vm_address_t rt2 = find_image(RUNTIME_UUID, "rosetta/runtime (post-exception)");
        if (rt2) {
            rt = rt2;
            read_bytes(rt + OFF_COMPOSE, v, sizeof v);
            read_bytes(rt + OFF_DECOMPOSE, v, sizeof v);
            if (!memcmp(v, PRO_DECOMPOSE, sizeof v)) {
                uint32_t c4[4];
                read_bytes(rt + OFF_COMPOSE, c4, sizeof c4);
                if (!memcmp(c4, PRO_COMPOSE, sizeof c4)) {
                    if (!patch_abs_jump(rt, OFF_COMPOSE, payload + P_NTFIX_COMPOSE_OFF / 4) &&
                        !patch_abs_jump(rt, OFF_DECOMPOSE, payload + P_NTFIX_DECOMPOSE_OFF / 4)) {
                        cv_patched = 1;
                        logf("converters patched post-exception at %016llx\n",
                             (unsigned long long)rt);
                    }
                } else {
                    logf("=== fail-closed: post-exception compose prologue mismatch\n");
                }
            } else {
                logf("=== fail-closed: post-exception decompose prologue mismatch\n");
            }
        }
    }
    if (cv_patched || (hooklib && hook_installed && (hook_installed() & 2))) {
        g_ctx_flags = ~0ul;
        unsigned long rv = ce();
        logf("exception check #2 (%s): ctx flags %08lx (NT=%lu, check 2) | post-resume NT=%lu (check 4)%s\n",
             hooklib ? "dylib install path" : "post-exception retry path",
             g_ctx_flags, (unsigned long)((g_ctx_flags >> 14) & 1),
             (unsigned long)((rv >> 14) & 1),
             hooklib ? " — THE hooklib check" : "");
    }

    /* r10b: check 3 — IRETQ with NT set must raise #GP. The insert (in the
     * emitted stream since the payload rebuild) traps before the thunk bl
     * with fault code 0x91 and desc(iretq) as the x86 fault address.
     * Expected with hooks: one SIGSEGV, rip and si_addr == blob+29, NT set
     * in the delivered flags, ctl[13] >= 1. NOHOOK control: insert never
     * emitted, no fault, iretq is silently consumed to `after`. */
    check_fn ci = fresh_check(3);
    if (!ci) { logf("=== abort: r10b fresh page alloc FAILED\n"); return 2; }
    {
        unsigned char *base = (unsigned char *)(void *)ci;
        g_iret_after = (unsigned long)(base + 34);   /* the fault shim */
        g_gp_count = 0;
        ctl[15] = getenv("NO_IRET") ? 1 : 0;   /* payload gate, read at arm */
        {
            const char *rs = getenv("R10B_RESUME");
            g_gp_resume = (rs && rs[0] == '2') ? 2 : (rs && rs[0] == '1') ? 1 : 0;
        }
        uint64_t if0 = ctl[13], is0 = ctl[14];
        struct sigaction sa, old;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = nt_sigsegv_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &old);
        unsigned long rv = ci();
        sigaction(SIGSEGV, &old, NULL);
        logf("r10b: SIGSEGV count=%lu rip=%016lx (iretq %016lx, delta %ld) "
             "addr=%016lx rsp=%016lx flags=%08lx (NT=%lu) | ret %08lx | iret fixups %llu->%llu, skips %llu->%llu\n",
             g_gp_count, g_gp_rip, (unsigned long)base + 29,
             (long)(g_gp_rip - ((unsigned long)base + 29)),
             g_gp_addr, g_gp_rsp, g_gp_flags,
             (unsigned long)((g_gp_flags >> 14) & 1), rv,
             (unsigned long long)if0, (unsigned long long)ctl[13],
             (unsigned long long)is0, (unsigned long long)ctl[14]);
        /* #GP(0) carries no faulting address on real hardware either
         * (si_addr 0 is truthful); the RIP must be the iretq, byte-exact */
        if (g_gp_count == 1 && g_gp_rip == (unsigned long)base + 29 &&
            (g_gp_flags & 0x4000))
            logf("=== r10b CHECK 3 PASS: iretq faults #GP at exactly the iretq, "
                 "NT in the delivered context, si_addr=%lx (0 = no address, as Windows)\n",
                 g_gp_addr);
        else if (!g_gp_count && ctl[13] > if0)
            logf("=== r10b CHECK 3 FAIL: insert fired but NO fault — tbz path taken (NT clear at run time?)\n");
        else if (!g_gp_count)
            logf("=== r10b CHECK 3 FAIL: no fault and no insert (NOHOOK/unpatched — status quo)\n");
        else
            logf("=== r10b CHECK 3 PARTIAL: fault seen, rip/addr/NT off expectations — see line above\n");
        /* dump the ring AS OF NOW so the iretq emission (if any) is in it —
         * the earlier append-ring.bin snapshot predates this translation */
        {
            unsigned nw2 = (unsigned)(ctl[3] & 0xffffffffull);
            if (nw2 > 0x20000) nw2 = 0x20000;
            int fd = open("/tmp/rosetta-re/r10b-ring.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                write(fd, ring, (size_t)nw2 * 4);
                close(fd);
                logf("r10b: ring as-of-now (%u words) dumped to /tmp/rosetta-re/r10b-ring.bin\n", nw2);
            }
        }
    }

    dump_jit();

    /* restore: displaced words straight back, verify. HOOKLIB mode never
     * restores — the dylib's patches are for the process lifetime, and the
     * counters below are the dylib's own. */
    if (!hooklib && !getenv("NOHOOK")) {
        int rc = cow_write(rb + (OFF_APPEND & ~0xffful), OFF_APPEND & 0xfff,
                           PRO_APPEND, 16);
        rc |= cow_write(rb + (OFF_GATHER_ENTRY & ~0xffful), OFF_GATHER_ENTRY & 0xfff,
                        PRO_GATHER, 16);
        rc |= cow_write(rb + (OFF_TI_ENTRY & ~0xffful), OFF_TI_ENTRY & 0xfff, PRO_TI, 16);
        read_bytes(rb + OFF_GATHER_ENTRY, v, sizeof v);
        if (rc || memcmp(v, PRO_GATHER, sizeof v)) {
            logf("=== restore FAILED (rc=%d readback %08x)\n", rc, v[0]);
            return 1;
        }
        read_bytes(rb + OFF_TI_ENTRY, v, sizeof v);
        if (memcmp(v, PRO_TI, sizeof v)) {
            logf("=== restore FAILED (ti readback %08x)\n", v[0]);
            return 1;
        }
        read_bytes(rb + OFF_APPEND, v, sizeof v);
        if (memcmp(v, PRO_APPEND, sizeof v)) {
            logf("=== restore FAILED (append readback %08x)\n", v[0]);
            return 1;
        }
        logf("restored, originals verified\n");
        if (cv_patched) {
            int rc2 = cow_write(rt + (OFF_COMPOSE & ~0xffful), OFF_COMPOSE & 0xfff,
                                PRO_COMPOSE, 16);
            rc2 |= cow_write(rt + (OFF_DECOMPOSE & ~0xffful), OFF_DECOMPOSE & 0xfff,
                             PRO_DECOMPOSE, 16);
            read_bytes(rt + OFF_COMPOSE, v, sizeof v);
            if (rc2 || memcmp(v, PRO_COMPOSE, sizeof v)) {
                logf("=== restore FAILED (compose rc=%d readback %08x)\n", rc2, v[0]);
                return 1;
            }
            read_bytes(rt + OFF_DECOMPOSE, v, sizeof v);
            if (memcmp(v, PRO_DECOMPOSE, sizeof v)) {
                logf("=== restore FAILED (decompose readback %08x)\n", v[0]);
                return 1;
            }
            logf("converters restored, originals verified\n");
        }
    }

    check_fn cu = fresh_check(1);
    if (cu) {
        if (hooklib) {
            /* the hook stays live under the dylib: NT must still be 1 here */
            unsigned long ru = cu();
            logf("control check (hooklib, hooks LIVE): NT=%lu (1 expected — nothing restored)\n",
                 (ru >> 14) & 1);
        } else {
            uint64_t g2 = *(uint64_t *)(dp + CTR_GATHER_OFF);
            uint64_t t2 = *(uint64_t *)(dp + CTR_TI_OFF);
            unsigned long ru = cu();
            uint64_t g3 = *(uint64_t *)(dp + CTR_GATHER_OFF);
            uint64_t t3 = *(uint64_t *)(dp + CTR_TI_OFF);
            logf("control check (restored): NT=%lu; gather %llu->%llu ti %llu->%llu (frozen expected)\n",
                 (ru >> 14) & 1, (unsigned long long)g2, (unsigned long long)g3,
                 (unsigned long long)t2, (unsigned long long)t3);
        }
    }
    logf("=== ntstub done\n");
    return 0;
}
