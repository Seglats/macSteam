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
        p[P_NTFIX_DECOMPOSE_OFF / 4] != P_NTFIX_DECOMPOSE_W0) {
        logf("fail-closed: payload entries %08x/%08x/%08x unexpected "
             "(want %08x/%08x/%08x)\n",
             p[P_NTFIX_APPEND_OFF / 4], p[P_NTFIX_COMPOSE_OFF / 4],
             p[P_NTFIX_DECOMPOSE_OFF / 4],
             P_NTFIX_APPEND_W0, P_NTFIX_COMPOSE_W0, P_NTFIX_DECOMPOSE_W0);
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
            logf("MACFINALS_NT_OFF=%s: gather %s scatter %s%s%s%s%s%s iret %s\n", off,
                 g_ctl[28] ? "off" : "on", g_ctl[29] ? "off" : "on",
                 zero ? " (zero-mode: NT delivered as 0)" : "",
                 nop ? " (nop-mode: no x17 write)" : "",
                 tail ? " (tail-mode: insert after msr)" : "",
                 logonly ? " (logonly: no insert2)" : "",
                 matchonly ? " (matchonly: no side effects)" : "",
                 burn == 1 ? " (burn 100/word, all chains still as set)" : "",
                 burn == 2 ? " (burn 1000/word, all chains still as set)" : "",
                 g_ctl[15] ? "off" : "on");
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

/* the converter image is demand-mapped: if it was not there at constructor
 * time, look again for a bounded while, then leave (fail-open either way).
 * Fast ticks first — the image appears with the FIRST exception and the
 * decisive window (the anti-cheat's probe) is early — then settle. */
static void *converter_poll(void *arg) {
    (void)arg;
    for (int i = 0; !(g_installed & 2); i++) {
        if (i >= 250) {   /* 100 fast ticks (10s) + 150 slow ones (5min) */
            logf("converter poll: image never appeared — contexts will carry "
                 "NT only after install\n");
            return NULL;
        }
        struct timespec ts = {i < 100 ? 0 : 2, i < 100 ? 100000000L : 0};
        nanosleep(&ts, NULL);
        int rc = install_converters();
        if (rc == 0) { g_installed |= 2; logf("converter poll: installed on tick %d\n", i); return NULL; }
        if (rc == 2) { logf("converter poll: fail-closed mismatch, giving up\n"); return NULL; }
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
            uint32_t hdr[4] = {0x43544c50u /* 'PTLC' LE = 'CTLP' bytes */, (uint32_t)i, 0, 32};
            (void)!write(fc, hdr, sizeof hdr);
            (void)!write(fc, g_ctl, 32 * sizeof(uint64_t));
        }
        if ((i & 7) == 0) dump_counters("poll");
    }
    return NULL;
}

static int ring_capture_on(void) {
    if (getenv("MACFINALS_HOOK_RING")) return 1;
    return access("/tmp/rosetta-re/nt-ring", F_OK) == 0;
}

static void atexit_dump(void) { dump_counters("at exit"); }

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
    if (rc == 0) g_installed |= 2;
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
    atexit(atexit_dump);
    dump_counters("at install");
}

#else  /* arm64 slice: a no-op, so DYLD_INSERT into native children is safe */

__attribute__((constructor))
static void macfinals_hook_init_arm64(void) { }

#endif
