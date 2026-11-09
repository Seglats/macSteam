/* hooklib.c — the MacFinals Rosetta NT correction, injectable (r11).
 *
 * Integration step 1 of the r11 plan (NOTES.md): everything ntstub.c proved
 * in-process, as a dylib a launcher can put in DYLD_INSERT_LIBRARIES so it
 * reaches every translated process of a game session — Steam's children,
 * wineserver's children and the game itself — without touching a single
 * game file or the Rosetta install on disk (in-memory, in-process COW only,
 * the sequence proven crash-safe since r5/cowtest).
 *
 * What it installs (identical machinery to ntstub, one code path less):
 *   append hook  — libRosettaRuntime 0x18a0 entry -> appendstub.S stub ->
 *                  payload ntfix_append: the r8 pushf/popf fixups and the
 *                  r10b iretq #GP insert, both riding x17 bit 5 as NT.
 *   converters   — /usr/libexec/rosetta/runtime compose/decompose -> payload
 *                  replacements (r9: contexts carry NT, resumes keep it).
 * ntstub's gather/ti counter hooks are diagnostics and are NOT installed —
 * the append hook alone carries every fixup.
 *
 * Differences from ntstub, all deliberate:
 *   - constructor, not main: installs at dyld time, before game code.
 *   - fail-open ALWAYS: any unexpected byte/UUID/shape -> log + return.
 *     It never exits the process it lives in — the worst case is exactly
 *     the unpatched status quo.
 *   - no restore: patches stay for process lifetime.
 *   - the converter image is demand-mapped (first exception can come after
 *     us): a short-lived poll thread retries it, then leaves.
 *   - refuses (fail-open) unless DISABLE_AOT is set: cached AOT translations
 *     bypass the append hook and would leave the process HALF-fixed — worse
 *     than consistently unpatched. The launcher sets DISABLE_AOT together
 *     with the insertion; ntstub's precedent.
 *
 * Universal build on purpose: the arm64 slice's constructor is a no-op, so a
 * native child that inherits the variable is unaffected (a single-arch
 * x86_64 dylib would abort arm64 children at dyld). Under Rosetta the x86_64
 * slice runs translated — the same execution mode ntstub proved.
 *
 *   clang -arch arm64 -arch x86_64 -O2 -dynamiclib -o hooklib.dylib hooklib.c
 *   DYLD_INSERT_LIBRARIES=$PWD/hooklib.dylib DISABLE_AOT=1 \
 *     HOOKLIB=1 R10B_RESUME=1 ./ntstub        # all four checks, via the dylib
 *
 * Log: /tmp/rosetta-re/hook-<pid>.log. Counters (ctl layout as payload_arm64.c
 * documents) dump again from the atexit handler on clean exits.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <libproc.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <libkern/OSCacheControl.h>

#ifndef MAP_TRANSLATED_ALLOW_EXECUTE
#define MAP_TRANSLATED_ALLOW_EXECUTE 0x2000
#endif

/* This build's fingerprints (fail-closed gates) — kept in lockstep with
 * ntstub.c; both must change together when Rosetta updates. */
static const uint8_t WANT_UUID[16] = {
    0xa1, 0xda, 0x2b, 0x2a, 0x3d, 0x68, 0x33, 0xcb,
    0xa6, 0x31, 0xfa, 0x77, 0x2d, 0xcd, 0xd1, 0x67};
static const uint8_t RUNTIME_UUID[16] = {
    0xe9, 0x33, 0xbc, 0x84, 0xae, 0x96, 0x32, 0x5a,
    0xa3, 0x0e, 0x90, 0x38, 0x0d, 0xd2, 0x74, 0xeb};

#define OFF_APPEND   0x18a0    /* append-one-word helper, libRosettaRuntime */
#define OFF_COMPOSE  0x198b8   /* /usr/libexec/rosetta/runtime             */
#define OFF_DECOMPOSE 0x1992c

/* anchors: the words the patches displace (restore data is the same bytes) */
static const uint32_t PRO_APPEND[4]    = {0xa9be4ff4u, 0xa9017bfdu, 0x910043fdu, 0xaa0103f4u};
static const uint32_t PRO_COMPOSE[4]   = {0xd342fc28u, 0xd341fc29u, 0x3100041fu, 0x5280504au};
static const uint32_t PRO_DECOMPOSE[4] = {0xd342fc08u, 0xd344fc09u, 0xd34bfc0au, 0xd3689c0bu};

#include "payload_blob.h"      /* freestanding arm64 payload (r8/r9/r10)    */
#include "append_blob.h"       /* assembler-built append stub (r8)          */

#define RING_WORDS 0x20000u    /* payload's compiled ring contract */
#define INSLOG_WORDS 4096u     /* payload contract: insert log after the ring */

static int g_fd = -1;
static uint64_t *g_ctl;        /* [16] counters, see payload_arm64.c */
static uint32_t *g_ring;
static uint32_t *g_payload;    /* MAP_TRANSLATED_ALLOW_EXECUTE page  */
static int g_installed;        /* 1 = append hook, 2 = converters    */

static void logf(const char *fmt, ...) {
    va_list ap;
    if (g_fd >= 0) {
        va_start(ap, fmt); vdprintf(g_fd, fmt, ap); va_end(ap);
    }
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

/* ---- mach helpers (same shape as ntstub.c; that file stays the lab copy) */

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

/* Walk executable mappings for the arm64 Mach-O image with this UUID.
 * Quiet by default (game processes map hundreds of regions); pass label to
 * log the hit — diagnostics stay opt-in here, unlike ntstub. */
static mach_vm_address_t find_image(const uint8_t want[16], const char *label) {
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
        if (info.protection & VM_PROT_EXECUTE) {
            unsigned char hdr[8];
            if (read_bytes(addr, hdr, sizeof hdr)) {
                uint32_t magic = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
                uint32_t cpu = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
                if (magic == 0xFEEDFACF && cpu == 0x0100000C &&
                    uuid_matches(addr, want)) {
                    if (label) logf("%s at %016llx\n", label, (unsigned long long)addr);
                    return addr;
                }
            }
        }
        addr += size;
    }
}

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

/* Map the payload layout-preserving (TEXT sealed RX, DATA RW), entries
 * readback-verified fail-closed — as ntstub's map_payload. */
static uint32_t *map_payload(void) {
    uint32_t *p = mmap(NULL, PAYLOAD_SPAN, PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE | MAP_TRANSLATED_ALLOW_EXECUTE, -1, 0);
    if (p == MAP_FAILED) { logf("payload mmap FAILED errno=%d\n", errno); return NULL; }
    memcpy(p, payload_text, PAYLOAD_TEXT_WORDS * 4);
    sys_dcache_flush(p, PAYLOAD_TEXT_WORDS * 4);
    sys_icache_invalidate(p, PAYLOAD_TEXT_WORDS * 4);
    if (mach_vm_protect(mach_task_self(), (mach_vm_address_t)p, 0x4000, FALSE,
                        VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
        logf("payload TEXT R|X FAILED\n");
        return NULL;
    }
    if (p[P_NTFIX_APPEND_OFF / 4] != P_NTFIX_APPEND_W0 ||
        p[P_NTFIX_COMPOSE_OFF / 4] != P_NTFIX_COMPOSE_W0 ||
        p[P_NTFIX_DECOMPOSE_OFF / 4] != P_NTFIX_DECOMPOSE_W0 ||
        p[P_NTFIX_MACFAST_OFF / 4] != P_NTFIX_MACFAST_W0 ||
        p[P_NTFIX_TRAPFAST_OFF / 4] != P_NTFIX_TRAPFAST_W0) {
        logf("fail-closed: payload entries %08x/%08x/%08x macfast %08x "
             "trapfast %08x (want %08x/%08x/%08x %08x %08x)\n",
             p[P_NTFIX_APPEND_OFF / 4], p[P_NTFIX_COMPOSE_OFF / 4],
             p[P_NTFIX_DECOMPOSE_OFF / 4], p[P_NTFIX_MACFAST_OFF / 4],
             p[P_NTFIX_TRAPFAST_OFF / 4],
             P_NTFIX_APPEND_W0, P_NTFIX_COMPOSE_W0, P_NTFIX_DECOMPOSE_W0,
             P_NTFIX_MACFAST_W0, P_NTFIX_TRAPFAST_W0);
        return NULL;
    }
    return p;
}

static int patch_abs_jump(mach_vm_address_t rb, uintptr_t entry_off, void *stub) {
    uint32_t jump[4] = {0x58000049u, 0xd61f0120u, 0, 0};  /* ldr x9,#8; br x9 */
    *(uint64_t *)&jump[2] = (uint64_t)stub;
    int rc = cow_write(rb + (entry_off & ~0xffful), entry_off & 0xfff, jump, 16);
    uint32_t got[4];
    read_bytes(rb + entry_off, got, sizeof got);
    if (rc || memcmp(got, jump, sizeof got)) {
        logf("install FAILED @%llx (rc=%d readback %08x)\n",
             (unsigned long long)entry_off, rc, got[0]);
        return 1;
    }
    return 0;
}

/* ---- r20: the finished-thunk syscall fast path ---------------------------
 *
 * The r19 stream insert never fired (the append stream is transformed before
 * emission — inserted words become garbage). r20 patches the FINISHED 16K
 * thunk region instead, where every word is verified before anything is
 * written. Layout (three byte-identical thunks, 2026-11-07):
 *   +0x200 strict entry save block, ends orr x18,#bit63 (+0x22c) then
 *         b -> class dispatch (+0x230)
 *   +0x2b10 dispatch: class 1/2/3 -> Mach/BSD/machdep; ANYTHING else ->
 *   +0x2b30 b +0x2eb8  — the ONLY branch to the trap in the region
 *   +0x2eb8 brk #5; b +0x400  — the trap (KEPT INTACT: every bail path
 *          re-enters it, so fail-closed = exactly today's behaviour)
 *   +0x2ec0 12 words of 0xfffffc90 dead padding
 * The driver writes a trampoline into the padding, then swaps the ONE word
 * at +0x2b30 to branch to it. One aligned word swap: concurrent threads see
 * either the stock branch or the trampoline — never a torn state.
 *
 * Everything patched here is anonymous JIT memory (never file-backed; the
 * 2026-10-31 freeze was file-backed Rosetta pages, a different thing). The
 * thunk dispatch page is HOT in the game (~32k syscalls/s), so the write
 * keeps the page executable throughout: mach_vm_write (kernel-mediated, no
 * protection change) first, then a COW window that retains X, and only as a
 * last resort the cold RW|COPY window. Every write is readback-verified. */
#define TH_ENTRY_OFF    0x200u
#define TH_TAG_OFF      0x22cu
#define TH_DEF_OFF      0x2b30u
#define TH_TRAP_OFF     0x2eb8u
#define TH_TRAMP_OFF    0x2ec0u
#define TH_REGION       0x4000u
static const uint32_t TH_ENTRY_SIG[3] = {0xa9010640u, 0xa9020e42u, 0xa9031644u};
#define TH_W_TAG        0xb2410252u   /* orr x18,x18,#bit63       */
#define TH_W_ENTB       0x14000a38u   /* b -> +0x2b10 dispatch    */
#define TH_W_DEF        0x140000e2u   /* b -> trap (stock)        */
#define TH_W_DEF_NEW    0x140000fcu   /* b -> +0x2ec0 trampoline  */
#define TH_W_TRAP0      0xd42000a0u   /* brk #5                   */
#define TH_W_TRAP1      0x17ffff51u   /* b +0x400                 */
#define TH_PAD_W        0xfffffc90u
#define TH_PAD_N        12u
#define TH_MAX_THUNKS   4096u

static uint32_t g_trampoline[6];        /* adr x27,#-8; movz/movk x26; br x26 */
static mach_vm_address_t g_thunk_done[TH_MAX_THUNKS];
static unsigned g_thunk_n, g_thunks_patched, g_thunks_badwrite, g_thunks_skipcfg;
static int g_trampoline_built;

static unsigned g_thunk_scan_gen;       /* rescan guard: bump = rescan all */

/* Build the trampoline for this process's payload address once. The movz/movk
 * formulas are the insert6 ones (assembler-verified 2026-11-07, as is
 * adr x27,#-8 = 0x10ffffdb). */
static void build_trampoline(void) {
    uint64_t addr = (uint64_t)(g_payload + P_NTFIX_TRAPFAST_OFF / 4);
    g_trampoline[0] = 0x10ffffdbu;                                       /* adr  x27,#-8 */
    g_trampoline[1] = 0xd2800000u | (((uint32_t)addr          & 0xffffu) << 5) | 26u;
    g_trampoline[2] = 0xf2a00000u | ((((uint32_t)(addr >> 16)) & 0xffffu) << 5) | 26u;
    g_trampoline[3] = 0xf2c00000u | ((((uint32_t)(addr >> 32)) & 0xffffu) << 5) | 26u;
    g_trampoline[4] = 0xf2e00000u | ((((uint64_t)addr >> 48)  & 0xffffu) << 5) | 26u;
    g_trampoline[5] = 0xd61f0340u;                                       /* br   x26     */
    g_trampoline_built = 1;
}

/* A hot-page write that never strips execute: kernel-mediated first, then a
 * COW window retaining X. Returns 0 on success. */
static int hot_write(mach_vm_address_t at, const void *data, unsigned len) {
    if (mach_vm_write(mach_task_self(), at, (mach_vm_address_t)(uintptr_t)data,
                      len) == KERN_SUCCESS) {
        sys_icache_invalidate((void *)(uintptr_t)at, len);
        return 0;
    }
    mach_vm_address_t page = at & ~0xfffull;
    unsigned orig = prot_of(page);
    mach_vm_size_t win = clamp_to_region(page, 0x1000);
    if (win) {
        kern_return_t kr = mach_vm_protect(mach_task_self(), page, win, FALSE,
                                           VM_PROT_READ | VM_PROT_WRITE |
                                           VM_PROT_EXECUTE | VM_PROT_COPY);
        if (kr == KERN_SUCCESS) {
            memcpy((void *)(uintptr_t)at, data, len);
            sys_icache_invalidate((void *)(uintptr_t)page, win);
            kr = mach_vm_protect(mach_task_self(), page, win, FALSE, orig);
            if (kr == KERN_SUCCESS) return 0;
            logf("hot_write %llx: X|COPY restore kr=%d\n",
                 (unsigned long long)at, kr);
            return 1;
        }
        logf("hot_write %llx: X|COPY kr=%d — falling back to cold RW|COPY\n",
             (unsigned long long)at, kr);
    }
    return cow_write(page, (unsigned)(at - page), data, len);
}

/* Full 16K signature check. Returns 1 for a patchable thunk, 2 for one this
 * process already patched, 0 for no match (fail-closed). */
static int thunk_verify(mach_vm_address_t w) {
    uint32_t v[TH_PAD_N];
    if (w & (TH_REGION - 1)) return 0;                    /* 16K-aligned only  */
    if (!read_bytes(w + TH_ENTRY_OFF, v, sizeof TH_ENTRY_SIG) ||
        memcmp(v, TH_ENTRY_SIG, sizeof TH_ENTRY_SIG)) return 0;
    if (!read_bytes(w + TH_TAG_OFF, v, 8) || v[0] != TH_W_TAG || v[1] != TH_W_ENTB)
        return 0;
    if (!read_bytes(w + TH_DEF_OFF, v, 4) ||
        (v[0] != TH_W_DEF && v[0] != TH_W_DEF_NEW)) return 0;
    int already = v[0] == TH_W_DEF_NEW;
    if (!read_bytes(w + TH_TRAP_OFF, v, 8) || v[0] != TH_W_TRAP0 || v[1] != TH_W_TRAP1)
        return 0;
    if (!read_bytes(w + TH_TRAMP_OFF, v, TH_PAD_N * 4)) return 0;
    for (unsigned i = 0; i < TH_PAD_N; i++) if (v[i] != TH_PAD_W) return 0;
    return already ? 2 : 1;
}

static int thunk_known(mach_vm_address_t w) {
    for (unsigned i = 0; i < g_thunk_n; i++) if (g_thunk_done[i] == w) return 1;
    return 0;
}

static void patch_one_thunk(mach_vm_address_t w) {
    uint32_t chk[6];
    if (g_thunk_n >= TH_MAX_THUNKS) { g_thunks_skipcfg++; return; }
    /* trampoline first: nothing branches to it until the redirect lands */
    if (hot_write(w + TH_TRAMP_OFF, g_trampoline, sizeof g_trampoline)) {
        g_thunks_badwrite++;
        logf("thunk %llx: trampoline write FAILED — left stock\n",
             (unsigned long long)w);
        return;
    }
    if (!read_bytes(w + TH_TRAMP_OFF, chk, sizeof g_trampoline) ||
        memcmp(chk, g_trampoline, sizeof g_trampoline)) {
        g_thunks_badwrite++;
        logf("thunk %llx: trampoline readback MISMATCH — left stock\n",
             (unsigned long long)w);
        return;
    }
    uint32_t redirect = TH_W_DEF_NEW;
    if (hot_write(w + TH_DEF_OFF, &redirect, 4) ||
        !read_bytes(w + TH_DEF_OFF, chk, 4) || chk[0] != TH_W_DEF_NEW) {
        g_thunks_badwrite++;
        logf("thunk %llx: redirect FAILED — trampoline parked, stock flow "
             "intact\n", (unsigned long long)w);
        return;
    }
    g_thunk_done[g_thunk_n++] = w;
    g_thunks_patched++;
    logf("thunk %llx patched: syscall fastpath live\n", (unsigned long long)w);
}

/* Walk every anonymous exec-capable region and patch every matching thunk. */
static void scan_patch_thunks(void) {
    if (!g_payload || !g_trampoline_built) return;
    if (g_ctl && g_ctl[33]) return;              /* MACFINALS_NT_OFF=syscall */
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    static unsigned char buf[0x40000];           /* 256K scan chunk          */
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        mach_vm_address_t next = addr + size;
        if (!info.is_submap && size <= (mach_vm_size_t)512 * 1024 * 1024 &&
            (info.protection & VM_PROT_EXECUTE ||
             info.max_protection & VM_PROT_EXECUTE)) {
            /* file-backed regions have names; Rosetta's JIT does not */
            char nm[64];
            nm[0] = 0;
            proc_regionfilename(getpid(), addr, nm, sizeof nm);
            if (!nm[0]) {
                for (mach_vm_address_t off = 0; off + 8 <= size; off += sizeof buf) {
                    mach_vm_size_t chunk = size - off < sizeof buf ? size - off
                                                                   : sizeof buf;
                    mach_vm_size_t got = 0;
                    if (mach_vm_read_overwrite(mach_task_self(), addr + off, chunk,
                                               (mach_vm_address_t)buf, &got) != KERN_SUCCESS
                        || got != chunk)
                        break;
                    /* the trap pair is 8 bytes; thunk base is 16K-aligned */
                    for (mach_vm_size_t i = 0; i + 8 <= chunk; i += 4) {
                        uint32_t a, b;
                        memcpy(&a, buf + i, 4);
                        memcpy(&b, buf + i + 4, 4);
                        if (a != TH_W_TRAP0 || b != TH_W_TRAP1) continue;
                        mach_vm_address_t t = addr + off + i;   /* &trap */
                        mach_vm_address_t w = t - TH_TRAP_OFF;  /* base  */
                        if ((w & (TH_REGION - 1)) != (TH_TRAP_OFF & (TH_REGION - 1)))
                            continue;      /* base must be 16K-aligned    */
                        if (thunk_known(w)) continue;
                        int rc = thunk_verify(w);
                        if (rc == 1) patch_one_thunk(w);
                        else if (rc == 2 && g_thunk_n < TH_MAX_THUNKS)
                            g_thunk_done[g_thunk_n++] = w;
                        else if (rc == 0) g_thunks_skipcfg++;   /* near miss */
                    }
                }
            }
        }
        if (info.is_submap) depth++;
        addr = next;
        if (!size) break;
    }
}

/* Thunks appear as new code translates (game code, late-loaded modules), so
 * the scan repeats on a slow cadence. Each pass reads candidate regions once;
 * patched thunks are remembered and skipped. */
static void *thunk_poll(void *arg) {
    (void)arg;
    struct timespec ts = {5, 0};
    for (;;) {
        nanosleep(&ts, NULL);
        scan_patch_thunks();
    }
    return NULL;
}


/* ---- the installs ------------------------------------------------------- */

/* append hook: data page (ctl + ring), payload page, stub page, one jump.
 * ctl/ring reach the payload as stub literals — no translated->native call
 * exists yet, so NOTHING here calls into the payload (exectest t1/t2). */
static int install_append(mach_vm_address_t rb) {
    uint32_t v[4];
    read_bytes(rb + OFF_APPEND, v, sizeof v);
    if (!memcmp(v, PRO_APPEND, sizeof v)) ;          /* expected: install  */
    else if (v[0] == 0x58000049u) {                   /* our jump word      */
        logf("append entry already hooked — skipping (second load?)\n");
        return 0;
    } else {
        logf("fail-closed: append prologue %08x %08x %08x %08x unexpected\n",
             v[0], v[1], v[2], v[3]);
        return 1;
    }

    char *dp = mmap(NULL, 0x1000 + (size_t)RING_WORDS * 4 + (size_t)INSLOG_WORDS * 4, PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE, -1, 0);
    if (dp == MAP_FAILED) { logf("data page FAILED errno=%d\n", errno); return 1; }
    g_ctl = (uint64_t *)dp;                       /* ctl block, 32 slots */
    /* MACFINALS_NT_OFF=gather,scatter,iret: bisect controls, read by the
     * payload when a chain arms (ctl[28]/[29]/[15]) */
    {
        /* the launcher builds Wine's environment from an allow-list, so the
         * bisect controls also come from a file (diagnostics only) */
        static char fbuf[64];
        const char *off = getenv("MACFINALS_NT_OFF");
        if (!off) {
            int cf = open("/tmp/rosetta-re/nt-off", O_RDONLY);
            if (cf >= 0) {
                ssize_t n = read(cf, fbuf, sizeof fbuf - 1);
                close(cf);
                if (n > 0) { fbuf[n] = 0; off = fbuf; }
            }
        }
        if (off) {
            /* exact comma-separated tokens: "scatter" must not match the
             * front of "scatterzero" */
            char toks[64];
            snprintf(toks, sizeof toks, "%s", off);
            int zero = 0, nop = 0, tail = 0, logonly = 0, matchonly = 0, burn = 0;
            for (char *t = strtok(toks, ", \t\n"); t; t = strtok(NULL, ", \t\n")) {
                if (!strcmp(t, "gather")) g_ctl[28] = 1;
                else if (!strcmp(t, "scatter")) g_ctl[29] = 1;
                else if (!strcmp(t, "iret")) g_ctl[15] = 1;
                else if (!strcmp(t, "syscall")) g_ctl[33] = 1;
                else if (!strcmp(t, "scatterzero")) zero = 1;
                else if (!strcmp(t, "scatternop")) nop = 1;
                else if (!strcmp(t, "scattertail")) tail = 1;
                else if (!strcmp(t, "scatterlog")) logonly = 1;
                else if (!strcmp(t, "scattermatch")) matchonly = 1;
                else if (!strcmp(t, "burn100")) burn = 1;
                else if (!strcmp(t, "burn1000")) burn = 2;
            }
            g_ctl[30] = zero;
            g_ctl[31] = nop ? 1 : (tail ? 2 : (logonly ? 3 : (matchonly ? 4 : (burn ? 4 + burn : 0))));
            logf("MACFINALS_NT_OFF=%s: gather %s scatter %s%s%s%s%s%s iret %s "
                 "syscall-fastpath %s\n", off,
                 g_ctl[28] ? "off" : "on", g_ctl[29] ? "off" : "on",
                 zero ? " (zero-mode: NT delivered as 0)" : "",
                 nop ? " (nop-mode: no x17 write)" : "",
                 tail ? " (tail-mode: insert after msr)" : "",
                 logonly ? " (logonly: no insert2)" : "",
                 matchonly ? " (matchonly: no side effects)" : "",
                 burn == 1 ? " (burn 100/word, all chains still as set)" : "",
                 burn == 2 ? " (burn 1000/word, all chains still as set)" : "",
                 g_ctl[15] ? "off" : "on",
                 g_ctl[33] ? "off" : "on");
        }
    }
    g_ring = (uint32_t *)(dp + 0x1000);           /* ring, RING_WORDS    */

    g_payload = map_payload();
    if (!g_payload) return 1;

    uint32_t *sp = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE | MAP_TRANSLATED_ALLOW_EXECUTE, -1, 0);
    if (sp == MAP_FAILED) { logf("stub page FAILED errno=%d\n", errno); return 1; }
    memcpy(sp, stub_blob, STUB_LEN * 4);
    {
        uint32_t *s = sp;
        uint64_t want[4] = {0xDEAD000000000001ull, 0xDEAD000000000003ull,
                            0xDEAD000000000004ull, 0xDEAD000000000002ull};
        uint64_t vals[4] = {(uint64_t)(g_payload + P_NTFIX_APPEND_OFF / 4),
                            (uint64_t)g_ctl, (uint64_t)g_ring,
                            rb + OFF_APPEND + 16};
        unsigned offs[4] = {PCALL_OFF, PCTL_OFF, PRING_OFF, PBACK_OFF};
        for (int i = 0; i < 4; i++) {
            if (*(uint64_t *)&s[offs[i]] != want[i]) {
                logf("fail-closed: append stub slot %d (%016llx) unexpected\n",
                     i, (unsigned long long)*(uint64_t *)&s[offs[i]]);
                return 1;
            }
            *(uint64_t *)&s[offs[i]] = vals[i];
        }
    }
    sys_dcache_flush(sp, 0x4000);
    sys_icache_invalidate(sp, 0x4000);
    if (mach_vm_protect(mach_task_self(), (mach_vm_address_t)sp, 0x4000, FALSE,
                        VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
        logf("stub page R|X FAILED\n");
        return 1;
    }
    if (patch_abs_jump(rb, OFF_APPEND, sp)) return 1;
    logf("append hook installed at libRosettaRuntime %016llx+0x18a0 "
         "(payload %p ctl %p ring %p)\n",
         (unsigned long long)rb, (void *)g_payload, (void *)g_ctl, (void *)g_ring);
    return 0;
}

/* converter replacements (r9). 0 = installed, 1 = image not mapped yet
 * (retry later), 2 = fail-closed mismatch. */
static int install_converters(void) {
    uint32_t v[4];
    mach_vm_address_t rt = find_image(RUNTIME_UUID, NULL);
    if (!rt) return 1;
    read_bytes(rt + OFF_COMPOSE, v, sizeof v);
    if (memcmp(v, PRO_COMPOSE, sizeof v)) {
        logf("fail-closed: compose prologue %08x %08x %08x %08x unexpected\n",
             v[0], v[1], v[2], v[3]);
        return 2;
    }
    read_bytes(rt + OFF_DECOMPOSE, v, sizeof v);
    if (memcmp(v, PRO_DECOMPOSE, sizeof v)) {
        logf("fail-closed: decompose prologue %08x %08x %08x %08x unexpected\n",
             v[0], v[1], v[2], v[3]);
        return 2;
    }
    if (patch_abs_jump(rt, OFF_COMPOSE, g_payload + P_NTFIX_COMPOSE_OFF / 4) ||
        patch_abs_jump(rt, OFF_DECOMPOSE, g_payload + P_NTFIX_DECOMPOSE_OFF / 4))
        return 2;
    logf("converters installed at rosetta/runtime %016llx "
         "(compose+decompose, readback verified)\n", (unsigned long long)rt);
    return 0;
}

/* r19: the Wine patch publishes __wine_syscall_dispatcher_prolog_end at
 * 0x7ffe1008 (slot 1 of the dispatcher-pointer page signal_start_thread maps).
 * The page appears only when the FIRST Windows thread starts — after the
 * constructor — so the driver reads it natively (x86 VA == arm64 VA under
 * Rosetta) on the poll ticks, guarded by a region walk (coverage + readability;
 * see the PROT_NONE note inside) so any not-yet-readable page is a no-op, never
 * a fault. Raw value into ctl[35]; the stub re-checks
 * non-zero/canonical and bails to the stock brk #5 otherwise. */
static void refresh_fastpath_target(void) {
    if (!g_ctl || g_ctl[35]) return;
    mach_vm_address_t addr = 0x7ffe1000;
    mach_vm_size_t size = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    natural_t depth = 0;
    if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                               (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
        return;                       /* nothing at/below the page yet */
    if (addr > 0x7ffe1000 || addr + size < 0x7ffe1010) return;
    /* coverage alone is NOT enough: before Wine maps the dispatcher page, the
     * address sits inside one of Rosetta's giant loader reservations, which is
     * MAPPED but PROT_NONE. A bare read there is KERN_PROTECTION_FAILURE — this
     * exact bug crashed every probe on macfinals-40 (poll tick 0 lands ~100 ms
     * in, before Wine's first Windows thread maps the page; wine-*.ips, fault
     * 0x7ffe1008 in the translated poll thread). Require readability AND take
     * the value via mach_vm_read_overwrite, which returns an error instead of
     * faulting whatever the page's state. */
    if (!(info.protection & VM_PROT_READ)) return;
    uint64_t val = 0;
    mach_vm_size_t nread = 0;
    if (mach_vm_read_overwrite(mach_task_self(), 0x7ffe1008, sizeof val,
                               (mach_vm_address_t)&val, &nread) != KERN_SUCCESS
        || nread != sizeof val)
        return;
    if (val) {
        g_ctl[35] = val;
        logf("fastpath: prolog_end %llx published at 0x7ffe1008\n",
             (unsigned long long)val);
    }
}

/* the converter image is demand-mapped: if it was not there at constructor
 * time, look again for a bounded while, then leave (fail-open either way).
 * Fast ticks first — the image appears with the FIRST exception and the
 * decisive window (the anti-cheat's probe) is early — then settle. The same
 * loop picks up the fastpath target (later than the converters: it needs
 * Wine's first Windows thread, not just an exception). */
static void *converter_poll(void *arg) {
    (void)arg;
    for (int i = 0; ; i++) {
        if (i >= 250) {   /* 100 fast ticks (10s) + 150 slow ones (5min) */
            if (!(g_installed & 2))
                logf("converter poll: image never appeared — contexts will carry "
                     "NT only after install\n");
            if (g_ctl && !g_ctl[35])
                logf("converter poll: 0x7ffe1008 never appeared — syscall "
                     "fastpath stays disarmed\n");
            return NULL;
        }
        struct timespec ts = {i < 100 ? 0 : 2, i < 100 ? 100000000L : 0};
        nanosleep(&ts, NULL);
        if (!(g_installed & 2)) {
            int rc = install_converters();
            if (rc == 0) { g_installed |= 2; logf("converter poll: installed on tick %d\n", i); }
            else if (rc == 2) { logf("converter poll: fail-closed mismatch, giving up\n"); return NULL; }
        }
        refresh_fastpath_target();
        if ((g_installed & 2) && (!g_ctl || g_ctl[35])) return NULL;
    }
    return NULL;
}

static void dump_counters(const char *when) {
    if (!g_ctl) return;
    logf("ctl %s: gather %llu scatter %llu skip %llu ring %llu | iret fixups %llu "
         "misses %llu disarm %llu | events", when,
         (unsigned long long)g_ctl[0], (unsigned long long)g_ctl[1],
         (unsigned long long)g_ctl[2], (unsigned long long)g_ctl[3],
         (unsigned long long)g_ctl[13], (unsigned long long)g_ctl[14],
         (unsigned long long)g_ctl[15]);
    for (int i = 4; i < 12; i++)
        if (g_ctl[i]) logf(" [%d]=%llx:%llx", i - 4,
                           (unsigned long long)(g_ctl[i] >> 32),
                           (unsigned long long)(g_ctl[i] & 0xffffffffu));
    logf("\n");
    /* r13 matcher diagnostics (payload_arm64.c ctl layout [16..27]) */
    logf("diag %s: nosl %llu slots %llu | seen movz202 %llu msr %llu irecs %llu | "
         "scat head %llu broken %llu msr-out %llu | gath lsr4 %llu bfi8 %llu "
         "push-miss %llu armed-out %llu\n", when,
         (unsigned long long)g_ctl[16], (unsigned long long)g_ctl[17],
         (unsigned long long)g_ctl[18], (unsigned long long)g_ctl[19],
         (unsigned long long)g_ctl[20], (unsigned long long)g_ctl[21],
         (unsigned long long)g_ctl[22], (unsigned long long)g_ctl[26],
         (unsigned long long)g_ctl[23], (unsigned long long)g_ctl[24],
         (unsigned long long)g_ctl[25], (unsigned long long)g_ctl[27]);
    /* r19/r20 syscall fastpath (ctl [32] inserts (r19, dead) [33] disarm
     * [34] misses (r19) [35] published target [36] engaged hits [37] stub
     * entries [39] clean bails; driver counts: thunks patched / bad writes /
     * signature near-misses) */
    logf("sysc %s: thunks %u badwrite %u nearmiss %u ins %llu miss %llu hits %llu disarm %llu "
         "target %llx entered %llu bail5 %llu guards %016llx c38 %llx\n", when,
         g_thunks_patched, g_thunks_badwrite, g_thunks_skipcfg,
         (unsigned long long)g_ctl[32], (unsigned long long)g_ctl[34],
         (unsigned long long)g_ctl[36], (unsigned long long)g_ctl[33],
         (unsigned long long)g_ctl[35], (unsigned long long)g_ctl[37],
         (unsigned long long)g_ctl[39],
         /* r21c guard histogram: 8 bytes of ctl[40], low byte = guard 2
          * (no target) .. byte 6 = guard 7 (instrumentation cb), byte 7 =
          * r21e TF guard (live x17 bit4 or ctx+0x444 — see the 2026-11-08
          * game crash). hex, so read RIGHT-to-LEFT: guards bbbbbbbbbbbbbbbb
          * — rightmost pair is guard 2. non-atomic byte counts; races only
          * smear totals. */
         (unsigned long long)(g_ctl[40] & 0xffffffffffffffffull),
         /* ais10: last-writer diagnostic — busy-bail stores the pending
          * word, the post-call path stores ctx+0x8 (small mask vs JIT ptr) */
         (unsigned long long)g_ctl[38]);
    /* r21c/ais5: guard-5b pointer dump — what TPIDRRO actually holds in a
     * translated Wine thread (set only when guard 5b has fired). ctl[45]
     * (ais6+): full-word 5b fail count — the byte lane at 0x144 wrapped at
     * 255 in the hot-loop runs. */
    if (g_ctl[41] || g_ctl[42] || g_ctl[43] || g_ctl[44] || g_ctl[45])
        logf("tpidrro %s: raw %llx bic %llx [base+30] %llx [base+378] %llx fails %llu\n",
             when,
             (unsigned long long)g_ctl[41], (unsigned long long)g_ctl[42],
             (unsigned long long)g_ctl[43], (unsigned long long)g_ctl[44],
             (unsigned long long)g_ctl[45]);
    /* r21b base census v2 (payload ctl [47..80]): appends+inserts per 2MB
     * bucket, and the last 8 raw bases that took an insert */
    logf("bases %s:", when);
    for (int i = 0; i < 8; i++)
        if (g_ctl[48 + i * 3])
            logf(" [%llx<<21 app %llu ins %llu]",
                 (unsigned long long)g_ctl[48 + i * 3],
                 (unsigned long long)g_ctl[49 + i * 3],
                 (unsigned long long)g_ctl[50 + i * 3]);
    if (g_ctl[47]) logf(" (+%llu appends in further buckets)", (unsigned long long)g_ctl[47]);
    logf(" | last X %llx | last insert bases:", (unsigned long long)g_ctl[46]);
    for (int i = 0; i < 8; i++)
        if (g_ctl[73 + i])
            logf(" %llx", (unsigned long long)g_ctl[73 + i]);
    logf("\n");
}

/* Ring capture, for matcher debugging. At exit via MACFINALS_HOOK_RING=1 —
 * and, because a crashing process never reaches atexit, continuously while
 * /tmp/rosetta-re/nt-ring exists: a side thread rewrites ring-<pid>.bin every
 * 300 ms (oldest-first), so the last window before a crash survives. The
 * launcher drops unknown env vars, hence the file switch like nt-off. */
static void write_ring_fd(int fd) {
    if (fd < 0 || !g_ring || !g_ctl) return;
    uint64_t total = g_ctl[3] & 0xffffffffull;
    uint32_t start = total > RING_WORDS ? (uint32_t)(total & (RING_WORDS - 1)) : 0;
    uint32_t n = total > RING_WORDS ? RING_WORDS : (uint32_t)total;
    lseek(fd, 0, SEEK_SET);
    if (total > RING_WORDS) {
        if (write(fd, g_ring + start, (size_t)(RING_WORDS - start) * 4) < 0) return;
        if (write(fd, g_ring, (size_t)start * 4) < 0) return;
    } else if (write(fd, g_ring, (size_t)n * 4) < 0) return;
    ftruncate(fd, (off_t)n * 4);
}

static void *ring_poll(void *arg) {
    (void)arg;
    char path[64];
    snprintf(path, sizeof path, "/tmp/rosetta-re/ring-%d.bin", (int)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    snprintf(path, sizeof path, "/tmp/rosetta-re/inslog-%d.bin", (int)getpid());
    int fi = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char cpath[64];
    snprintf(cpath, sizeof cpath, "/tmp/rosetta-re/ctl-%d.bin", (int)getpid());
    int fc = open(cpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    logf("ring poll: capturing to /tmp/rosetta-re/{ring,inslog,ctl}-%d.bin (50ms early, 300ms later)\n",
         (int)getpid());
    for (int i = 1;; i++) {
        struct timespec ts = {0, (i <= 100 ? 50L : 300L) * 1000L * 1000L};
        nanosleep(&ts, NULL);
        write_ring_fd(fd);
        if (fi >= 0) (void)!write(fi, g_ring + RING_WORDS, (size_t)INSLOG_WORDS * 4);
        if (fc >= 0 && g_ctl) {
            uint32_t hdr[4] = {0x43544c50u /* 'PTLC' LE = 'CTLP' bytes */, (uint32_t)i, 0, 37};
            (void)!write(fc, hdr, sizeof hdr);
            (void)!write(fc, g_ctl, 81 * sizeof(uint64_t));
        }
        if ((i & 7) == 0) dump_counters("poll");
    }
    return NULL;
}

static int ring_capture_on(void) {
    if (getenv("MACFINALS_HOOK_RING")) return 1;
    return access("/tmp/rosetta-re/nt-ring", F_OK) == 0;
}

static void atexit_dump(void);

/* r21 diagnostic: one-shot arena scan at exit (translation quiescent) for the
 * v6 insert chain — MACFINALS_SYSC_DUMP=1. Metadata walk + page-guarded direct
 * loads only (the 2026-11-07 rule; also the r21 caution: never mid-churn).
 * Answers "did the chains reach the materialized TBs at all?" for a process
 * where entered==0 despite inserts (the Wine probe run). Wine owns SIGSEGV at
 * this point, so the recovery handler is installed around the scan and
 * restored after. */
#if defined(__x86_64__)
#include <setjmp.h>
#include <signal.h>
static sigjmp_buf sd_jmp;
static volatile sig_atomic_t sd_armed;
static struct sigaction sd_old_segv, sd_old_bus;
static void sd_recover(int sig) { (void)sig; if (sd_armed) siglongjmp(sd_jmp, 1); _exit(78); }
static int sd_read32(const uint32_t *p, uint32_t *out) {
    sd_armed = 1;
    if (sigsetjmp(sd_jmp, 1)) { sd_armed = 0; return 0; }
    *out = *(volatile const uint32_t *)p;
    sd_armed = 0;
    return 1;
}
static int sd_is_x26(uint32_t w) {   /* movz/movk x26 any shift */
    return (w & 0xffe0001fu) == 0xd2800000u + 26u ||
           (w & 0xffe0001fu) == 0xf2800000u + 26u ||
           (w & 0xffe0001fu) == 0xf2a00000u + 26u ||
           (w & 0xffe0001fu) == 0xf2c00000u + 26u ||
           (w & 0xffe0001fu) == 0xf2e00000u + 26u;
}
static void syscdump(void) {
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    unsigned regions = 0, hits = 0, stock = 0, stock_shown = 0, exec_regions = 0;
    uint64_t seen[128];
    unsigned nseen = 0;
    char path[64];
    snprintf(path, sizeof path, "/tmp/rosetta-re/syscdump-%d.log", (int)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    logf("syscdump: scanning arenas for v6 chains -> %s\n", path);
    struct sigaction rec = {.sa_sigaction = (void *)sd_recover, .sa_flags = SA_SIGINFO};
    sigemptyset(&rec.sa_mask);
    sigaction(SIGSEGV, &rec, &sd_old_segv);
    sigaction(SIGBUS, &rec, &sd_old_bus);
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        mach_vm_address_t next = addr + size;
        /* the walker wedges uninterruptibly somewhere past 0x209000000 (a
         * 128MB prot-3 arena; SIGKILL still works). Everything Rosetta
         * emits into lives below 0x210000000 — stop the walk there and
         * say so, instead of hanging the dump */
        if (addr >= 0x210000000ull) {
            if (fd >= 0) dprintf(fd, "SCAN STOP at %016llx (limit)\n", (unsigned long long)addr);
            break;
        }
        if ((info.protection | info.max_protection) & VM_PROT_EXECUTE) {
            /* region LIST: every exec-capable region, named or not, so the
             * arena map is on record even where the scan is skipped */
            char nm[256] = {0};
            proc_regionfilename(getpid(), addr, nm, sizeof nm - 1);
            if (fd >= 0)
                dprintf(fd, "REGION %016llx size %llu prot %x max %x submap %u name '%s'\n",
                        (unsigned long long)addr, (unsigned long long)size,
                        (unsigned)info.protection, (unsigned)info.max_protection,
                        (unsigned)info.is_submap, nm);
            exec_regions++;
            /* big non-exec-protection regions (exec only in max_protection)
             * are list-only: the 128MB prot-3 arena at 0x209000000 WEDGES
             * the page scan (reads block in page-in, walker uninterruptible
             * — SIGKILL works, SIGALRM does not; run6) */
            int dup = 0;
            for (unsigned i = 0; i < nseen; i++) if (seen[i] == addr) { dup = 1; break; }
            if (!dup && nseen < 128) seen[nseen++] = addr;
            /* scan deduped, unnamed OR named, non-submap, readable, sane size —
             * the walker revisits regions (2-4x multiplicities seen), so the
             * seen[] guard keeps counts honest */
            if (!dup && !info.is_submap && size <= 512ull * 1024 * 1024 &&
                (info.protection & VM_PROT_READ) &&
                ((info.protection & VM_PROT_EXECUTE) || size <= 1024 * 1024)) {
                unsigned stock_here = 0, hits_here = 0;
                regions++;
                for (unsigned long pg = 0; pg < size; pg += 0x1000) {
                    const uint32_t *pw = (const uint32_t *)(addr + pg);
                    unsigned limit = (size - pg < 0x1000) ? (unsigned)(size - pg) : 0x1000;
                    uint32_t probe;
                    if (!sd_read32(pw, &probe)) continue;
                    sd_armed = 1;
                    if (sigsetjmp(sd_jmp, 1)) { sd_armed = 0; continue; }
                    for (unsigned wo = 6; wo + 8 <= limit; wo++) {
                        /* stock site census: carrier + movk(+movk) + a bl word,
                         * chain or not — reported per region so staged vs
                         * executed copies can be told apart */
                        if ((pw[wo] & 0xffe0001fu) == 0xd2c0001cu) {
                            int k = wo + 1, mk = 0;
                            while (k < (int)limit - 1 && mk < 3 &&
                                   ((pw[k] & 0xffe0001fu) == 0xf2a0001cu ||
                                    (pw[k] & 0xffe0001fu) == 0xf280001cu)) { k++; mk++; }
                            if (k < (int)limit - 1 && (pw[k] & 0xfc000000u) == 0x94000000u) {
                                if (fd >= 0 && stock_shown < 24) {
                                    dprintf(fd, "STOCK %u at %016llx (region %016llx prot %x) words:",
                                            stock_shown, (unsigned long long)(uintptr_t)(pw + wo),
                                            (unsigned long long)addr, (unsigned)info.protection);
                                    /* int bound: q<=k-wo+1 mixed sign made the loop
                                     * dead (words never printed) — cast both sides */
                                    int last = (int)k - (int)wo + 1;
                                    for (int q = -2; q <= last; q++)
                                        dprintf(fd, " %08x", pw[wo + q]);
                                    dprintf(fd, "\n");
                                    stock_shown++;
                                }
                                stock_here++;
                            }
                        }
                        /* blr x26; b #-28 with a movz/movk x26 within 4 before */
                        if (pw[wo] == 0xd63f0340u && pw[wo+1] == 0x17fffff9u &&
                            (sd_is_x26(pw[wo-1]) || sd_is_x26(pw[wo-2]) ||
                             sd_is_x26(pw[wo-3]) || sd_is_x26(pw[wo-4]))) {
                            const uint32_t *at = pw + wo;
                            if (fd >= 0) dprintf(fd,
                                "HIT %u at %016llx (region %016llx prot %x) words:",
                                hits, (unsigned long long)(uintptr_t)(at - 24),
                                (unsigned long long)addr, (unsigned)info.protection);
                            /* X-4..: report 6 words before the blr (= X+4..X+20
                             * area) plus the pair after, and the 4 words before
                             * the replay (X-4 sacrificial zone) */
                            for (int k = -10; k <= 2; k++) {
                                if (fd >= 0) dprintf(fd, " %08x", at[k]);
                            }
                            if (fd >= 0) dprintf(fd, "\n");
                            hits++;
                            hits_here++;
                            wo += 2;
                        }
                    }
                    sd_armed = 0;
                }
                if (stock_here || hits_here) {
                    if (fd >= 0)
                        dprintf(fd, "SUMMARY region %016llx prot %x size %llu: %u chains, %u stock sites\n",
                                (unsigned long long)addr, (unsigned)info.protection,
                                (unsigned long long)size, hits_here, stock_here);
                    stock += stock_here;
                }
            }
        }
        addr = next;
    }
    sigaction(SIGSEGV, &sd_old_segv, NULL);
    sigaction(SIGBUS, &sd_old_bus, NULL);
    logf("syscdump: %u exec regions (%u scanned), %u v6 chains, %u stock sites\n",
         exec_regions, regions, hits, stock);
    if (fd >= 0) { dprintf(fd, "regions %u scanned %u chains %u stock %u\n",
                           exec_regions, regions, hits, stock); close(fd); }
}
#endif

static void atexit_dump(void) {
#if defined(__x86_64__)
    if (getenv("MACFINALS_SYSC_DUMP")) syscdump();
#endif
    dump_counters("at exit");
}

/* ---- exports: ntstub's HOOKLIB mode and diagnostics read these ---------- */
const uint64_t *macfinals_ctl(void) { return g_ctl; }
const uint32_t *macfinals_ring(void) { return g_ring; }
uint32_t macfinals_ring_words(void) { return g_ctl ? (uint32_t)(g_ctl[3] & 0xffffffffull) : 0; }
int macfinals_installed(void) { return g_installed; }

#if defined(__x86_64__)

static int translated_process(void) {
    int v = 0;
    size_t n = sizeof v;
    return sysctlbyname("sysctl.proc_translated", &v, &n, NULL, 0) == 0 && v == 1;
}

__attribute__((constructor))
static void macfinals_hook_init(int argc, const char **argv) {
    /* a native arm64 child that inherited the variable: nothing to correct */
    if (!translated_process()) return;

    mkdir("/tmp/rosetta-re", 0755);
    char path[64];
    snprintf(path, sizeof path, "/tmp/rosetta-re/hook-%d.log", (int)getpid());
    g_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    /* proc_pidpath only ever names the Wine loader; argv carries the Windows
     * image, which is what tells one process of a session from another */
    char cmd[600] = {0};
    for (int i = 0; i < argc && argv && argv[i]; i++) {
        size_t n = strlen(cmd);
        if (n + 2 >= sizeof cmd) break;
        snprintf(cmd + n, sizeof cmd - n, "%s%s", i ? " " : "", argv[i]);
    }
    logf("=== hooklib pid %d ppid %d installed=%d cmd: %s\n",
         (int)getpid(), (int)getppid(), g_installed, cmd);
    if (getenv("MACFINALS_NT_HOOK") && !strcmp(getenv("MACFINALS_NT_HOOK"), "0")) {
        logf("MACFINALS_NT_HOOK=0: disabled by request\n");
        return;
    }
    /* Steam's CEF UI (steamwebhelper.exe, steamwebhelper_real.exe and every
     * child they spawn) never executes pushf/popf/iret — all counters stay 0 —
     * but with the append hook live its browser process stalls inside
     * Rosetta's translator at 100-160% CPU and no window ever appears (bisect
     * 2026-11-01: no hook / DISABLE_AOT + empty dylib both boot in ~10 s;
     * append-only and full hook never do). Leave it untranslated-by-us. */
    if (strcasestr(cmd, "steamwebhelper")) {
        logf("skipped: Steam CEF process (append hook stalls it; it needs no NT fix)\n");
        return;
    }
    if (!getenv("DISABLE_AOT")) {
        logf("REFUSING: DISABLE_AOT is not set — cached AOT translations bypass "
             "the hook and would leave this process HALF-fixed. Set DISABLE_AOT=1 "
             "together with the injection (as ntstub requires).\n");
        return;
    }

    mach_vm_address_t rb = find_image(WANT_UUID, "libRosettaRuntime");
    if (!rb) { logf("fail-closed: no UUID-matching libRosettaRuntime —Rosetta "
                    "build changed? nothing patched\n"); return; }
    if (install_append(rb)) return;
    g_installed |= 1;
    if (ring_capture_on()) {
        pthread_t th;
        if (pthread_create(&th, NULL, ring_poll, NULL) == 0) pthread_detach(th);
        else logf("ring poll thread FAILED errno=%d\n", errno);
    }

#ifdef MACFINALS_NO_CONVERTERS
    /* bisect build: append hook only, Rosetta's own context converters */
    logf("converters skipped (MACFINALS_NO_CONVERTERS build)\n");
    atexit(atexit_dump);
    dump_counters("at install");
    return;
#endif
    int rc = install_converters();
    if (rc == 0) {
        g_installed |= 2;
        /* r19: converters were installable now, but the syscall-fastpath
         * target (0x7ffe1008) only appears with Wine's FIRST Windows
         * thread — still needs the poll, or the fastpath stays disarmed
         * in every normal process (found in the macfinals-39 probe run:
         * target 0 forever). The poll self-exits once ctl[35] is set. */
        if (g_ctl && !g_ctl[35]) {
            pthread_t th;
            if (pthread_create(&th, NULL, converter_poll, NULL) == 0)
                pthread_detach(th);
            else
                logf("converter poll thread FAILED errno=%d — syscall "
                     "fastpath stays disarmed in this process\n", errno);
        }
    }
    else if (rc == 2) { logf("converters unmatched — append hook stays live "
                             "(pushf/popf/iret NT only, contexts not)\n"); }
    else {
        /* image not mapped yet: watch for it on the side */
        pthread_t th;
        if (pthread_create(&th, NULL, converter_poll, NULL) == 0)
            pthread_detach(th);
        else
            logf("converter poll thread FAILED errno=%d — contexts stay "
                 "NT-blind in this process\n", errno);
    }
    /* r20 QUARANTINED 2026-11-07: the first in-process run of this scanner
     * PANICKED THE KERNEL — twice, deterministically (ntstub gate runs at
     * 18:20:00 and 18:21:16, two forced reboots). Panicked task = the hooked
     * process itself; faulting user frame = scan_patch_thunks+494, the
     * mach_vm_read_overwrite of a translated anonymous executable region;
     * kernel data abort far=0x14 (NULL+0x14), macOS 27.0 26A428. Panic files
     * copied to /tmp/rosetta-re/panics/. sysdump's reads never hit this, but
     * sysdump read 4K at a time, post-startup, as the only mach client; this
     * scan reads 256K chunks from dyld-constructor time while translation is
     * live and converter_poll queries in parallel. Until each mach_vm_* use
     * on translated mappings is re-proven safe (NOTES.md 2026-11-07), the
     * whole feature stays behind an explicit opt-in. */
    if (getenv("MACFINALS_THUNK_PATCH") &&
        !strcmp(getenv("MACFINALS_THUNK_PATCH"), "1")) {
        build_trampoline();
        scan_patch_thunks();
        {
            pthread_t th;
            if (pthread_create(&th, NULL, thunk_poll, NULL) == 0)
                pthread_detach(th);
            else
                logf("thunk poll thread FAILED errno=%d — late thunks stay "
                     "stock in this process\n", errno);
        }
    }
    else {
        logf("thunk patch OFF (2026-11-07 kernel panic: mach_vm_read_overwrite "
             "on translated exec regions — see NOTES.md); MACFINALS_THUNK_PATCH=1 "
             "to re-enable\n");
    }
    atexit(atexit_dump);
    dump_counters("at install");
}

#else  /* arm64 slice: a no-op, so DYLD_INSERT into native children is safe */

__attribute__((constructor))
static void macfinals_hook_init_arm64(void) { }

#endif
