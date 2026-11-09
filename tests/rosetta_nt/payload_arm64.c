/* payload_arm64.c — the native arm64 payload for the NT correction (r8).
 *
 * Lives on a MAP_TRANSLATED_ALLOW_EXECUTE page that ntstub.c maps it into by
 * flat-copying this dylib's segments layout-preserving (dyld refuses arm64
 * slices in a translated process — exectest t3). FREESTANDING constraints,
 * enforced by design and checked by mkpayload.py at build time: no imports,
 * no string literals, no pointer-valued statics — only PC-relative static
 * access, so no relocations are needed after the copy.
 *
 * What it does: the append-stub at Rosetta's 0x18a0 word-emission helper
 * calls ntfix_append(bufobj, word) for EVERY emitted arm64 word. The state
 * machine below recognises the pushf-gather and popf-scatter sequences
 * (ground truth: /tmp/rosetta-re/append-ring.bin, 146 words, milestone-4)
 * and inserts the NT fixups INTO THE EMITTED STREAM before the anchor word:
 *
 *   gather (pushf): insert  lsr wT,w17,#5 ; bfi wD,wT,#14,#1   before the
 *                   push `str xD,[x4,#-8]!`   (NT bit 14 <- x17 bit 5)
 *   scatter (popf): insert  lsr wA,wS,#14 ; bfi x17,xA,#5,#1   before the
 *                   `msr NZCV,xT`           (x17 bit 5 <- NT bit 14)
 *
 * x17 is Rosetta's per-thread side-flags register; bits 1-4 are its own
 * (DF/AC/ID/TF), bit 5 is unused (the TF-change check masks #0x10 only) and
 * becomes our NT slot. Fixups never touch live registers: wT is the gather's
 * own dead temp, wA the scatter's.
 *
 * Insertion is bounds-checked (cursor+12 <= limit, leaving room for the
 * anchor word) and fails OPEN: a skipped fixup just means that one pushf/popf
 * pair behaves exactly as unpatched Rosetta — status quo, never worse.
 *
 *   clang -arch arm64 -O2 -mgeneral-regs-only -dynamiclib -mmacosx-version-min=26.0 -o payload.dylib payload_arm64.c
 *   python3 mkpayload.py payload.dylib payload_blob.h
 *
 * The same source is compiled into the arm64 host unit test
 * (host_payload_test.c), which replays the recorded 146-word ring AND runs
 * the slot CAS from real threads — natively, on the shipping hardware.
 */

typedef unsigned int u32;
typedef unsigned long u64;

/* --- encodings: every constant verified against `clang -arch arm64` + otool
 *     (fixverify.S, 2026-10-31) and derived mechanically from the recorded
 *     ring words (host_payload_test asserts the composed results). Compose:
 *     FIXED | (Rn<<5) | Rd, where FIXED = verified word & ~0x3ff. --- */
#define LSR_W(rd, rn, imm) (0x53007c00u | ((u32)(imm) << 16) | ((u32)(rn) << 5) | (u32)(rd))
#define LSR_W17_5(rd)      (0x53057e20u | (u32)(rd))                     /* lsr wT, w17, #5 (Rn=17 baked in) */
#define BFI_W14(rd, rn)    (0x33120000u | ((u32)(rn) << 5) | (u32)(rd))  /* bfi wD, wT, #14, #1 */
#define BFI_X17_5(rn)      (0xb37b0000u | ((u32)(rn) << 5) | 17u)        /* bfi x17, xA, #5, #1 */

/* --- match skeletons: real emitted words with Rd (4:0) and Rn (9:5) cleared,
 *     so dynamic register allocation cannot break the match --- */
#define SK(w)   ((w) & 0xfffffc00u)
#define RN(w)   (((w) >> 5) & 31u)
#define RD(w)   ((w) & 31u)

#define MOVZ_0x202(w) (((w) & 0xffffffe0u) == 0x52804040u)  /* movz wD,#0x202  gather head */
#define MOVZ_0(w)     (((w) & 0xffffffe0u) == 0x52800000u)  /* movz wT,#0      scatter head */
#define MSR_NZCV(w)   (((w) & 0xffffffe0u) == 0xd51b4200u)  /* msr NZCV,xT     scatter anchor */
#define PUSH_STR(w)   (((w) & 0xffffffe0u) == 0xf81f8c80u)  /* str xD,[x4,#-8]! gather anchor (Rn=4 included) */
/* scatter head, in order: lsr#7 bfi#31 lsr#6 bfi#30 lsr#11 bfi#28 */
static const u32 SCAT_HEAD[6] = {0x53077c00u, 0x33010000u, 0x53067c00u,
                                 0x33020000u, 0x530b7c00u, 0x33040000u};
#define LSR17_4(w)  (SK(w) == 0x53047c00u && RN(w) == 17u)  /* lsr wT,w17,#4  gather tail */
#define BFI8(w)     (SK(w) == 0x33180000u)                  /* bfi wD,wT,#8,#1 gather tail */

/* --- r10b: the IRETQ escape -------------------------------------------------
 *
 * Ground truth: jitdump run 2026-10-31, jit-3.bin @0x1b90-0x1bb8 (words
 * reproduced by clang+otool in /tmp/rosetta-re/fixverify.S). The emitted
 * iretq builds its frame, materialises the RETURN TARGET's descriptor in x1
 * (adrp+add), pops the frame into registers and bl's the shared thunk — with
 * NO test of the current NT (the bug). We insert FOUR words before the bl:
 *
 *   tbz  x17, #5, bl      ; NT clear -> straight to the bl (bit set falls in)
 *   sub  x22, x1, #2      ; desc(iretq) = desc(target) - delta
 *   movz x23, #0x91       ; Rosetta's #GP fault code (the cli convention)
 *   brk  #0x3             ; raise — exception server reports the x86 RIP
 *
 * x17 is read-only here; x22/x23 are written only on the fault path where
 * they are dead. The delta is 2 in OUR trigger (iretq immediately followed
 * by its target label); other x86 shapes need their own delta — fail-open
 * only skips the insert, never mis-faults. Encodings verified in
 * fixverify.S: 36280091 d1000836 d2801237 d4200060.
 *
 * desc(target) materialisation comes in TWO shapes (both seen live):
 *   adrp x1,<page>; add x1,x1,#imm          (jitdump process, jit-3 @0x1b9c)
 *   movz x1,#..,lsl#..; movk x1,#.. (x0-3)  (ntstub r10b ring @1277: d2c00021
 *                                            f2a12ac1 f29403e1 = mov/movk/movk)
 * The chain accepts either; anything else disarms (fail-open, counted). */
#define IRET_LDRH_CS(w)  (((w) & 0xffffffe0u) == 0x7942ee40u)  /* ldrh wT,[x18,#0x176] CS   */
#define IRET_STR_PRE(w)  (((w) & 0xffffffe0u) == 0xf81f8c80u)  /* str xT,[x4,#-8]!  push    */
#define IRET_ADRP_X1(w)  (((w) & 0x9f00001fu) == 0x90000001u)  /* adrp x1,<any page>        */
#define IRET_ADD_X1(w)   (((w) & 0xffc003ffu) == 0x91000021u)  /* add x1,x1,#imm            */
#define IRET_MOVZ_X1(w)  (((w) & 0xff80001fu) == 0xd2800001u)  /* movz x1,#imm,lsl#hw (hw free) */
#define IRET_MOVK_X1(w)  (((w) & 0xff80001fu) == 0xf2800001u)  /* movk x1,#imm,lsl#hw (hw free) */
#define IRET_LDP_POST(w) (((w) & 0xffff83e0u) == 0xa8c10080u)  /* ldp xT,xT2,[x4],#0x10     */
#define IRET_LDP_PLN(w)  (((w) & 0xffff83e0u) == 0xa9400080u)  /* ldp xT,xT2,[x4]           */
#define IS_BL(w)         (((w) & 0xfc000000u) == 0x94000000u)  /* bl <thunk>                */
static const u32 IRET_INSERT[4] = {0x36280091u, 0xd1000836u, 0xd2801237u, 0xd4200060u};

/* --- r19: the direct-syscall fast path --------------------------------------
 *
 * Ground truth: ring capture 2026-11-07 (probe run under macfinals-39) — see
 * R19_SYSCALL_FASTPATH.md. A direct x86 `syscall` with an invalid class (the
 * game: rax=0xffffffff and Windows SSNs, ~32k/s) takes the full Rosetta
 * exception round trip (~16-18 us). CORRECTED MODEL: the per-page thunk
 * (entry page+0x200, class dispatch page+0xb10, brk #5 trap page+0xeb8) is
 * installed by Rosetta via a path that NEVER flows through the append
 * stream — 0 brk#5/svc words in ~900k captured words across 7 processes —
 * so no append-time matcher can see it. What DOES flow through the stream
 * is the PER-SITE emission for each translated `syscall` instruction
 * (32 hits in the probe ring, all byte-stable):
 *
 *   ldrb w22,[x22]      394002d6  ; compat-mode byte check (internal)
 *   tst  w22,#1         720002df
 *   <hint>              d500401f
 *   b.eq                54000001  ; placeholder, relocated to the compat path
 *   movz x28,#A,lsl#32  d2c00..c  ; x86 RIP carrier (A,B fixed per image,
 *   movk x28,#B,lsl#16  f2a00..c  ; C varies per site)
 *   movk x28,#C         f280..c
 *   bl   #0             94000000  ; placeholder, relocated to the thunk
 *
 * (The R19 design note's "mov w0,<eax>" was the GUEST's own preceding mov,
 * not template — there is no w0 materialisation.) The chain anchors on the
 * movz x28,lsl#32 carrier (Rd=28 + lsl#32 only ever appears here; x28 is
 * Rosetta-internal) and inserts the divert in front of the bl — the exact
 * insert-before-bl shape the production iret chain uses (the bl's later
 * relocation is unaffected, proven by iret):
 *
 *   (v6 geometry — r21, 2026-11-08, capsite3 arena dumps)
 *
 * GROUND TRUTH #2: a Rosetta fixup pass writes a 26-bit immediate at
 * stream offset X — the PRE-insert cursor, the position where the emitter
 * recorded the bl's fixup — as a read-modify-write that preserves the
 * word's top 6 bits. Two different words sat at X in two dumps: adr
 * (100000db) became 13ffe81e, nop (d503201f) became d7ffe7de — both
 * UNDEFINED encodings, both exactly "26-bit patch over top-6-preserved
 * original". Whatever occupies X becomes garbage; executing it is the
 * r20-era translator assert (EXC_BAD_INSTRUCTION -> ExceptionServer.cpp,
 * exit 132). The x26 immediates survived untouched in 24/24 dumped copies.
 * So v6 NEVER EXECUTES X (X = cursor when the bl arrives):
 *
 *   X-4   b #+8          14000002  behind-cursor overwrite of the last
 *                               site word (read-back verified first) —
 *                               jumps the fall-through from X-4 to X+4
 *   X+0   bl #0          94000000  SACRIFICIAL: absorbs the fixup patch
 *                               (26-bit RMW over a real bl = a valid
 *                               relocated bl); dead on the divert path,
 *                               IS the bail target
 *   X+4   <replayed site word>     verbatim: restores the x28 carrier
 *                               (and guest-rax state if that was last)
 *   X+8   movz x26,#stub
 *   X+12  movk x26,#stub,16
 *   X+16  movk x26,#stub,32   no pc-relative word anywhere (replication
 *   X+20  movk x26,#stub,48   preserves immediates, 24/24 dumps)
 *   X+24  blr x26        d63f0340  divert; lr = X+28
 *   X+28  b #-28         17fffff9  bail: back to X = the REAL relocated bl
 *   X+32  (the emitter's own bl lands here: unrelocated, dead code)
 *
 * Fall-through enters the b at X-4 and skips X. A stream copy rebuilt from
 * emitter intent drops the whole edit (pure stock, fail-closed); a copy
 * that preserves our words never executes X either. Bail = br x30 lands on
 * the b at X+28 -> X, whose patch made it the exact relocated bl stock
 * would have had, with lr = X+4 exactly as stock — byte-identical slowpath
 * entry, no brk#5, no exception server.
 *
 * The class is a RUNTIME value at the site (no w0 materialisation), so the
 * divert lands at every direct-syscall site including Wine's own
 * class-1/2/3 sites; the stub's guard turns those back in ~10 instructions.
 *
 * The stub (ntfix_macfast) first replicates the thunk entry's save (x0-x15
 * -> ctx+0x10..0x8f, NZCV -> +0x90, x17 -> +0x98, LR -> +0x8; at the site
 * the ctx slots hold stale values), then performs EXACTLY Wine's
 * sigsys_handler semantics: frame->rcx = RIP_sig (x28, set by the site's
 * movz/movk carrier), frame->rip = +0xb, frame->eflags = compose(parked
 * NZCV, parked x17), restore_flags per TF, RCX_sig = frame, R11_sig =
 * eflags, then resumes at Wine's __wine_syscall_dispatcher_prolog_end
 * (published by a Wine patch at 0x7ffe1008, read once natively by the
 * driver into ctl[35]) via the same runtime entry the thunk's own return
 * uses: table[0](ctx, lr, rip) installs the TB entry at ctx+0x8; restore
 * regs; ret into it. Guards (g_ctl, target published and canonical, class
 * 0/7, TEB self-pointer, frame != 0, instrumentation_callback == 0) run
 * BEFORE the save and touch only Rosetta-internal x22-x25 — bail = br x30
 * re-executes the untouched bl with the original machine state. */
#define IS_SYSC_MOVZ_X28(w)   (((w) & 0xffe0001fu) == 0xd2c0001cu) /* movz x28,#,lsl32 */
#define IS_SYSC_MOVK16_X28(w) (((w) & 0xffe0001fu) == 0xf2a0001cu) /* movk x28,#,lsl16 */
#define IS_SYSC_MOVK0_X28(w)  (((w) & 0xffe0001fu) == 0xf280001cu) /* movk x28,# */
/* ctl: [32] inserts  [33] DISARM (driver sets 1)  [34] chain misses
 *      [35] published prolog_end value (driver reads 0x7ffe1008 natively)
 *      [36] stub ENGAGED count (entry installed, dispatcher entered) */
#define CTL_SYSC_INS    32u
#define CTL_SYSC_DISARM 33u
#define CTL_SYSC_MISS   34u
#define CTL_SYSC_TARGET 35u
#define CTL_SYSC_HIT    36u
/* r19 diagnostics: stub entries (inline atomic — pre-save, x22-x25 only) and
 * clean bails at 5 (counted post-dispatch, guest state parked in ctx). Early
 * guard bails at 99 = entries - hits - bail5s. */
#define CTL_SYSC_ENTERED 37u
#define CTL_SYSC_BAIL5   39u
/* r21b base census, v2: Wine cycles ~275k distinct bufobj bases through the
 * append path (first-8-exact was useless), but they cluster in a handful of
 * arenas. [48..71] = 8 x (2MB bucket = base>>21, appends, inserts);
 * [47] = appends in buckets beyond the 8th. [72] = rotating index,
 * [73..80] = the LAST 8 raw bases that took a v6 insert — where chains
 * physically land, to compare against where executed code lives. */
#define CTL_BASE_OVF  47u
#define CTL_BASE_TAB  48u
#define CTL_INS_IDX   72u
#define CTL_INS_BASES 73u
#define CTL_LAST_X    46u   /* r21b: last chain's physical address (set in insert8) */

void ntfix_macfast(void);  /* naked stub, defined at the end of the file */

enum { ST_IDLE = 0, ST_SCAT_HEAD, ST_SCAT_MSR, ST_SCAT_TAIL, ST_GATH_ARMED, ST_GATH_TAIL, ST_GATH_PUSH,
       ST_IRET_CS, ST_IRET_DESC, ST_IRET_ADD, ST_IRET_MOVK, ST_IRET_PUSH2,
       ST_IRET_POP1, ST_IRET_POP2, ST_IRET_BL,
       ST_SYSC_MOVZ, ST_SYSC_MOVK };

/* r13: the table used to be 16 slots claimed for life by the first 16
 * buffer objects seen. A Wine process has gone through all 16 before its
 * first game instruction, so every later word was dropped (probe 2026-11-01:
 * 151350 of 155046 words, "nosl"), and only the odd pushf/popf/iret that
 * happened to land in a still-owned buffer was ever fixed. Slots now carry a
 * lock and are recycled: a buffer needs a slot only while a chain is in
 * flight, so one is claimed only on a chain-head word, and any IDLE slot may
 * be taken over. The lock is held for the whole step, so a takeover can never
 * hand one stream's half-built chain (and its registers) to another. */
#define NSLOT 64
struct slot {
    u32 lock;    /* 0 free, 1 held — taken for every step on this slot */
    u32 state, phase;
    u32 scan;    /* r14: scatter words matched while a gather is armed; 0 = none */
    u32 S, A;    /* scatter: source flags reg, dead lsr temp */
    u32 D, T;    /* gather: compose reg, dead lsr temp */
    u32 budget;  /* words left before giving up (fail-open to IDLE) */
    u32 sysc_last; /* r21: last site-family word before the bl (the replay) */
    u64 key;     /* bufobj pointer, written only under the lock */
    u64 last;    /* last cursor seen: a backwards step = buffer reused */
};

static struct slot slots[NSLOT];

/* r11: game processes translate on several threads at once, so the ctl
 * counters must be atomic — a plain ++ is load/add/store and a
 * multi-threaded JIT loses half of them, which the host test proved the
 * hard way (800 inserts counted as 389). See ctl_inc below the ctl
 * declaration. */
static int try_lock(u32 *l) {
#if defined(__aarch64__)
    u32 old, ok = 1;
    __asm__ volatile("  ldaxr %w0, [%2]\n"
                     "  cbnz  %w0, 1f\n"
                     "  stxr  %w1, %w3, [%2]\n"
                     "  b     2f\n"
                     "1: clrex\n"
                     "2:"
                     : "=&r"(old), "+&r"(ok)
                     : "r"(l), "r"(1u)
                     : "cc", "memory");
    return old == 0 && ok == 0;
#else
    if (*l) return 0;                /* single-threaded builds (none ship) */
    *l = 1;
    return 1;
#endif
}
static void unlock(u32 *l) {
#if defined(__aarch64__)
    __asm__ volatile("stlr wzr, [%0]" : : "r"(l) : "memory");
#else
    *l = 0;
#endif
}
static void lock(u32 *l) { while (!try_lock(l)) { } }
static u32 *g_ring;      /* observability: every word through the helper */
static u32 g_ring_at;
static u64 *g_ctl;       /* see ctl layout below; self-set on first call */
#define RING_WORDS 0x20000u

/* ctl block, shared with ntstub.c (plain RW page, both sides read/write):
 *   [0] gather fixups   [1] scatter fixups  [2] skipped (bounds)  [3] ring_at
 *   [4..11] last 8 fixup events: (kind << 32) | ring index of the anchor
 *   [12] thread ctx (x18), written by the append stub itself (r10a)
 *   [13] iret fixups    [14] iret misses (chain budget exhausted or bounds)
 *   [15] iret DISARM (x86 side sets 1 to suppress the r10b insert — control)
 * The payload never calls out — everything the x86 side needs to observe is
 * here, because direct translated-x86 -> native-arm64 calls are unproven
 * (exectest t1/t2 SIGILL); all crossings ride the append hook. */
static void note_event(u32 kind) {
    u64 e = ((u64)kind << 32) | (g_ring_at ? g_ring_at - 1 : 0);
    u32 i;
    for (i = 11; i > 4; i--) g_ctl[i] = g_ctl[i - 1];
    g_ctl[4] = e;
}

/* atomic counter bump (ldaxr/stxr — works on every arm64 Apple ships; the
 * host unit test drives it from real threads) */
static void ctl_inc(u64 i) {
#if defined(__aarch64__)
    u64 *p = &g_ctl[i], tmp; u32 ok;
    if (!g_ctl) return;
    __asm__ volatile("1: ldaxr %0, [%2]\n"
                     "  add  %0, %0, #1\n"
                     "  stlxr %w1, %0, [%2]\n"
                     "  cbnz %w1, 1b"
                     : "=&r"(tmp), "=&r"(ok)
                     : "r"(p)
                     : "cc", "memory");
#else
    if (g_ctl) g_ctl[i] += 1;
#endif
}

/* r21b base census v2 — see the CTL_BASE_TAB comment. base_note runs on
 * EVERY append: an 8-slot scan of already-hot ctl memory, nothing else
 * touches those slots. Deliberately NOT atomic: a lost increment under a
 * race costs a miscount, never a wrong bucket. */
static void base_note(u64 base) {
    u64 bucket = base >> 21, *tab;
    int i;
    if (!g_ctl) return;
    tab = &g_ctl[CTL_BASE_TAB];
    for (i = 0; i < 8; i++) {
        if (tab[i * 3] == bucket) { tab[i * 3 + 1]++; return; }
        if (tab[i * 3] == 0) { tab[i * 3] = bucket; tab[i * 3 + 1] = 1; return; }
    }
    g_ctl[CTL_BASE_OVF]++;
}
/* insert tally + last-8 insert-base ring for a base already bucketed */
static void base_tally_insert(u64 base) {
    u64 bucket = base >> 21, *tab;
    int i;
    if (!g_ctl) return;
    tab = &g_ctl[CTL_BASE_TAB];
    for (i = 0; i < 8; i++)
        if (tab[i * 3] == bucket) { tab[i * 3 + 2]++; break; }
    i = (int)(g_ctl[CTL_INS_IDX]++ & 7u);
    g_ctl[CTL_INS_BASES + i] = base;
}

/* atomic ring slot claim. r13: the ring WRAPS (index masked, RING_WORDS is
 * a power of two), so it always holds the most recent words — the probe's
 * own translations come last, after Wine's startup has filled 128K words.
 * The mask is what keeps r12's fix: no index ever reaches past the end. */
static u32 ring_claim(void) {
#if defined(__aarch64__)
    u32 old, nxt, ok;
    __asm__ volatile("1: ldaxr %w0, [%3]\n"
                     "  add  %w1, %w0, #1\n"
                     "  stlxr %w2, %w1, [%3]\n"
                     "  cbnz %w2, 1b"
                     : "=&r"(old), "=&r"(nxt), "=&r"(ok)
                     : "r"(&g_ring_at)
                     : "cc", "memory");
    return old & (RING_WORDS - 1u);
#else
    return g_ring_at++ & (RING_WORDS - 1u);
#endif
}

static void ctl_inc(u64 i);

/* Returns this buffer's slot LOCKED, or 0. Lookup reads keys unlocked as a
 * hint and confirms under the lock. With no slot for the key, a new one is
 * taken only when `head` (a word that can start a chain): any IDLE one,
 * never-used slots included. A cursor that went BACKWARDS means a recycled
 * buffer: its chain from the previous life is dropped before it can
 * mis-fire. None idle -> 0: the caller only observes (fail-open). */
/* r13 insert log: at every insert the payload snapshots the emission
 * context (26 words before the anchor + the anchor + register roles) into a
 * side buffer after the ring, CAS-claimed and wrapping. A crashing process
 * never reaches atexit, so the x86 side flushes this continuously; the last
 * record before death is the failing site. Record = 32 words:
 *   [0] 'INSL' magic  [1] kind (1 gather, 2 scatter, 3 iret)
 *   [2] g_ring_at snapshot  [3] A | S<<8 | D<<16 | T<<24
 *   [4..29] 26 emitted words before the anchor  [30] anchor word  [31] 0 */
#define INSLOG_WORDS 4096u
#define INSLOG_REC   32u
#define INSLOG_N     (INSLOG_WORDS / INSLOG_REC)
#define INSLOG_MAGIC 0x494e534cu
static u32 inslog_at;
static void inslog(u32 kind, u64 *bo, u32 w, u32 a, u32 sr) {
    u32 *base = g_ring + RING_WORDS;
    u32 idx, nxt, ok, i;
    u64 cur;
    if (!g_ring) return;
    __asm__ volatile("1: ldaxr %w0, [%3]\n"
                     "  add  %w1, %w0, #1\n"
                     "  stlxr %w2, %w1, [%3]\n"
                     "  cbnz %w2, 1b"
                     : "=&r"(idx), "=&r"(nxt), "=&r"(ok)
                     : "r"(&inslog_at)
                     : "cc", "memory");
    u32 *rec = base + (idx % INSLOG_N) * INSLOG_REC;
    rec[0] = INSLOG_MAGIC; rec[1] = kind;
    rec[2] = g_ring_at;    rec[3] = (a & 31u) | ((sr & 31u) << 8);
    cur = bo[1];
    if (cur >= 26u * 4u) {
        u32 *src = (u32 *)(u64)bo[0];
        for (i = 0; i < 26u; i++) rec[4 + i] = src[cur / 4u - 26u + i];
    } else for (i = 0; i < 26u; i++) rec[4 + i] = 0;
    rec[30] = w; rec[31] = 0;
}

static struct slot *slot_for(u64 key, u64 cursor, int head) {
    u32 i;
    for (i = 0; i < NSLOT; i++) {
        struct slot *s = &slots[i];
        if (s->key != key) continue;
        lock(&s->lock);
        if (s->key == key) {
            if (s->last > cursor) s->state = ST_IDLE;
            s->last = cursor;
            return s;
        }
        unlock(&s->lock);
    }
    if (!head) return 0;
    for (i = 0; i < NSLOT; i++) {
        struct slot *s = &slots[i];
        if (s->state != ST_IDLE || !try_lock(&s->lock)) continue;
        if (s->state == ST_IDLE) {
            ctl_inc(17);
            s->key = key; s->last = cursor;
            return s;
        }
        unlock(&s->lock);
    }
    return 0;
}

/* bufobj = {base@+0, cursor@+8 (byte offset), limit@+0x10}; the original
 * helper stores the anchor word at base[cursor] and advances by 4 after we
 * return, so our two words go down first and the cursor bumps by 8. */
static int insert2(u64 *bo, u32 w0, u32 w1) {
    u64 cur = bo[1], lim = bo[2];
    u32 *p;
    if (cur + 12 > lim) return 0;
    p = (u32 *)(u64)(bo[0] + cur);
    p[0] = w0;
    p[1] = w1;
    bo[1] = cur + 8;
    return 1;
}

/* same contract for the 4-word iret insert (r10b): anchor still to come */
static int insert4(u64 *bo, const u32 *w4) {
    u64 cur = bo[1], lim = bo[2];
    u32 *p;
    if (cur + 20 > lim) return 0;
    p = (u32 *)(u64)(bo[0] + cur);
    p[0] = w4[0];
    p[1] = w4[1];
    p[2] = w4[2];
    p[3] = w4[3];
    bo[1] = cur + 16;
    return 1;
}

/* r19: the 3-word divert + 8-byte stub literal (5 words); anchor (brk #5)
 * still to come at cur+20 — which is exactly what the adr targets. */
/* r21 v6: the divert that never touches the poisoned slot. Ground truth
 * (capsite3 dumps, 2026-11-08): a Rosetta fixup pass writes a 26-bit
 * immediate at the stream position where the emitter recorded the bl —
 * the PRE-insert cursor X — read-modify-write (top 6 bits preserved).
 * Every earlier shape put our first word exactly at X, first in the
 * fall-through path: the patch turned it into an UNDEFINED encoding, and
 * executing that copy was the on-and-off translator assert. The fix is
 * geometry, not encoding: X gets a SACRIFICIAL bl #0 (post-patch it is a
 * correctly relocated bl, and dead either way), the entry `b` at X-4
 * (behind-cursor overwrite of the last site word, read-back verified)
 * jumps OVER X, the replayed site word restores what the b displaced,
 * and the blr's lr lands on a `b` back to X — the bail path re-runs the
 * real relocated bl, the exact stock slowpath. Copies rebuilt from
 * emitter intent drop all of it (pure stock, fail-closed); copies that
 * keep our words never execute X. The movz/movk x26 immediates were
 * untouched in every dumped copy (24/24). */
static int insert8(u64 *bo, u64 addr, u32 lastw) {
    u64 cur = bo[1], lim = bo[2];
    u32 *p, *prev;
    if (cur + 36 > lim) return 0;   /* the insert + the bl itself */
    prev = (u32 *)(u64)(bo[0] + cur - 4);
    if (*prev != lastw) return 0;   /* not the site word: never touch it */
    prev[0] = 0x14000002u;                                  /* b #+8 (to X+4) */
    p = (u32 *)(u64)(bo[0] + cur);
    p[0] = 0x94000000u;                                       /* sacrificial bl #0 (absorbs the X fixup) */
    p[1] = lastw;                                             /* replayed site word (restores x28/x0) */
    p[2] = 0xd2800000u | (((u32)addr           & 0xffffu) << 5) | 26u; /* movz x26,#   */
    p[3] = 0xf2a00000u | ((((u32)(addr >> 16)) & 0xffffu) << 5) | 26u; /* movk x26,#,16*/
    p[4] = 0xf2c00000u | ((((u32)(addr >> 32)) & 0xffffu) << 5) | 26u; /* movk x26,#,32*/
    p[5] = 0xf2e00000u | ((((u64)addr >> 48)  & 0xffffu) << 5) | 26u; /* movk x26,#,48*/
    p[6] = 0xd63f0340u;                                       /* blr x26 (lr = the bail b) */
    p[7] = 0x17fffff9u;                                       /* b #-28 (back to the real bl) */
    bo[1] = cur + 32;
    if (g_ctl) g_ctl[46] = (u64)p;   /* r21b: the PHYSICAL chain address (X) */
    return 1;
}

void ntfix_init(u32 *ring, u32 ring_words, u64 *ctl) {
    (void)ring_words;
    g_ring = ring;
    g_ring_at = 0;
    g_ctl = ctl;
}

/* the append stub calls this with x0 = bufobj, w1 = the word being appended,
 * x2 = ctl block, x3 = ring; it runs BEFORE the original helper stores the
 * word. ctl/ring arrive as stub literals and are adopted on first call —
 * no init call from the translated side is ever needed. */
static void step(struct slot *s, u64 *bo, u32 w);

void ntfix_append(u64 *bo, u32 w, u64 *ctl, u32 *ring) {
    struct slot *s;
    if (!g_ctl && ctl) { g_ctl = ctl; g_ring = ring; g_ring_at = 0; }
    base_note(bo[0]);   /* r21b census: which buffers this path emits into */

    /* r13 timing experiment: burn cycles in the hook with every chain
     * disarmed. If Steam still crashes, the crash is translation-latency
     * sensitivity, not anything the matcher writes. */
    if (g_ctl && (g_ctl[31] == 5 || g_ctl[31] == 6)) {
        static volatile u32 burn_sink;
        u32 n = g_ctl[31] == 5 ? 100u : 1000u, i;
        for (i = 0; i < n; i++) burn_sink += i;
    }

    /* r12: the index is claimed atomically. A plain check-then-increment let
     * two JIT threads both pass `< RING_WORDS` on the last slot, and the
     * loser wrote one word past the ring — which is the payload's first
     * instruction (the payload maps directly after ctl+ring, R-X): the
     * translator then faulted inside Rosetta forever. CEF wedged at 160% CPU
     * with no window (bisect 2026-11-01: null dylib boots, append-only
     * hook does not). The counter saturates instead of wrapping. */
    if (g_ring) {
        u32 at = ring_claim();
        g_ring[at] = w;
    }
    if (g_ctl) g_ctl[3] = g_ring_at;   /* total words seen (ring keeps the last RING_WORDS) */
    /* r13 diagnostics: anchors as seen in the raw stream, matcher or not */
    if (MOVZ_0x202(w)) ctl_inc(18);
    else if (MSR_NZCV(w)) ctl_inc(19);
    else if (IRET_LDRH_CS(w)) ctl_inc(20);
    {
        /* r21 (2026-11-08): the sysc arm is BACK, re-anchored on fresh
         * capture ground truth (capsite carrier, ring 76367). The chain in
         * the stream is movn/movz w0 (the guest rax) + movz/movk x28 +3
         * words + bl #0; the r19-era ldrb/tst/hint/b.eq prefix does NOT
         * appear at file-backed sites, so the anchor is the movz x28,lsl#32
         * carrier alone. The r19 insert fired but never engaged (entered 0)
         * — leading hypothesis: every examined host had ctl[35]=0 (no Wine
         * target), so the stub's own guards bailed; this re-arm tests that
         * in a controlled carrier: entered>0 with no Wine = the divert
         * landed and executed, bailing fail-closed to the stock bl. */
        int head = MOVZ_0x202(w) || MOVZ_0(w) || IRET_LDRH_CS(w) ||
                   (IS_SYSC_MOVZ_X28(w) && !(g_ctl && g_ctl[33]));
        s = slot_for((u64)bo, bo[1], head);
        if (!s) { if (head) ctl_inc(16); return; }  /* no slot: observed, never matched */
    }
    step(s, bo, w);
    unlock(&s->lock);
}

/* r13: an iret chain that disarms is logged (inslog kind 4, stage in the A
 * field) so the missed template shape can be read back from a process that
 * exits without running atexit. Kind 5 is a gather that armed and timed out. */
static void iret_miss(struct slot *s, u64 *bo, u32 w) {
    inslog(4, bo, w, s->state, 0);
    ctl_inc(14);
    s->state = ST_IDLE;
}

/* r14: heads are also what literals in guest code emit (`mov r,0x202`, `mov r,0`), so a chain
 * that gives up must look at the word it gave up on: it may be the real head. */
static void idle_word(struct slot *s, u32 w) {
    s->state = ST_IDLE;
    if (MOVZ_0x202(w) && !(g_ctl && g_ctl[28])) { s->D = RD(w); s->state = ST_GATH_ARMED; s->budget = 48; s->scan = 0; }
    else if (MOVZ_0(w) && !(g_ctl && g_ctl[29])) { s->state = ST_SCAT_HEAD; s->phase = 0; }
    else if (IRET_LDRH_CS(w) && !(g_ctl && g_ctl[15])) {
        s->state = ST_IRET_CS; s->budget = 4;
    }
    /* r21: the syscall-site arm — see the head-predicate note above */
    else if (IS_SYSC_MOVZ_X28(w) && !(g_ctl && g_ctl[33])) {
        s->state = ST_SYSC_MOVZ; s->budget = 6;
        s->sysc_last = w;   /* no movk follows: this is the X-4 replay word */
    }
}

/* r14: an armed gather may be a literal, so a real popf has to get through. It only takes the
 * chain over once its whole six-word head has matched, which no gather contains. */
static int scatter_took_over(struct slot *s, u32 w) {
    if (g_ctl && g_ctl[29]) return 0;
    if (s->scan && SK(w) == SCAT_HEAD[s->scan - 1]) {
        if (s->scan == 1) { s->S = RN(w); s->A = RD(w); }
        if (++s->scan == 7) { s->state = ST_SCAT_MSR; s->budget = 16; ctl_inc(21); return 1; }
        return 0;
    }
    s->scan = MOVZ_0(w) ? 1 : 0;
    return 0;
}

static void step(struct slot *s, u64 *bo, u32 w) {
    switch (s->state) {
    case ST_IDLE:
        idle_word(s, w);
        break;

    case ST_SCAT_HEAD:
        if (SK(w) != SCAT_HEAD[s->phase]) {
            if (s->phase) ctl_inc(22);
            idle_word(s, w); break;
        }
        if (s->phase == 0) { s->S = RN(w); s->A = RD(w); }
        if (++s->phase == 6) { s->state = ST_SCAT_MSR; s->budget = 16; ctl_inc(21); }
        break;

    case ST_SCAT_MSR:
        if (MSR_NZCV(w)) {
            /* ctl[30]: diagnostic zero-mode — same two words, same cursor
             * bump, same registers written (w24, x17 bit 5) but the bfi
             * source is xzr, so NT is always delivered as 0. Splits "the
             * insert mechanics corrupt translation" from "NT state breaks
             * the program". r13. */
            u32 w1 = (g_ctl && g_ctl[30]) ? 0xb37b03f1u : BFI_X17_5(s->A);
            if (g_ctl && g_ctl[31] == 1) w1 = 0xd503201fu;  /* NOP: no x17 write */
            if (g_ctl && g_ctl[31] == 2) {
                /* TAIL: leave the msr exactly where Rosetta put it; insert
                 * before the next word (the rebuild's first lsr). NT lands
                 * after the msr but before any reader of x17. */
                s->state = ST_SCAT_TAIL;
                break;
            }
            if (g_ctl && g_ctl[31] == 3) {
                /* LOGONLY: match, log, count — never touch the JIT buffer.
                 * Isolates matcher side effects from insert2. */
                inslog(2, bo, w, s->A, s->S);
                note_event(2);
                ctl_inc(1);
                s->state = ST_IDLE;
                break;
            }
            if (g_ctl && g_ctl[31] == 4) {
                /* MATCH-ONLY: chain arms, head matches, msr matches, and
                 * then NOTHING happens — no insert, no log, no counter.
                 * Isolates the state-machine path from its side effects. */
                s->state = ST_IDLE;
                break;
            }
            inslog(2, bo, w, s->A, s->S);
            if (insert2(bo, LSR_W(s->A, s->S, 14), w1)) {
                ctl_inc(1);
                note_event(2);
            } else ctl_inc(2);
            s->state = ST_IDLE;
        } else if (!--s->budget) { ctl_inc(26); idle_word(s, w); }
        break;

    case ST_SCAT_TAIL:
        inslog(2, bo, w, s->A, s->S);
        if (insert2(bo, LSR_W(s->A, s->S, 14), BFI_X17_5(s->A))) {
            ctl_inc(1);
            note_event(2);
        } else ctl_inc(2);
        s->state = ST_IDLE;
        break;

    case ST_GATH_ARMED:
        if (LSR17_4(w)) { s->T = RD(w); s->state = ST_GATH_TAIL; s->budget = 8; ctl_inc(23); }
        else if (MOVZ_0x202(w)) { s->D = RD(w); s->budget = 48; s->scan = 0; }  /* the last head is the real one */
        else if (scatter_took_over(s, w)) break;
        else if (!--s->budget) { inslog(5, bo, w, s->D, 0); ctl_inc(27); idle_word(s, w); }
        break;

    case ST_GATH_TAIL:
        if (BFI8(w) && RD(w) == s->D) { s->state = ST_GATH_PUSH; s->budget = 4; ctl_inc(24); }
        else if (!--s->budget) idle_word(s, w);
        break;

    case ST_GATH_PUSH:
        if (PUSH_STR(w) && RD(w) == s->D) {
            inslog(1, bo, w, s->D, s->T);
            if (insert2(bo, LSR_W17_5(s->T), BFI_W14(s->D, s->T))) {
                ctl_inc(0);
                note_event(1);
            } else ctl_inc(2);
            s->state = ST_IDLE;  /* the push str is one-shot: armed or not, done */
        } else { ctl_inc(25); idle_word(s, w); }
        break;

    /* --- r10b iret chain: each stage matches within a tight budget and any
     * miss disarms (fail-open — unpatched Rosetta behaviour, never worse,
 * counted in ctl[14]). Arm: CS load. Then CS push, desc(target) built into
 * x1 (adrp+add OR movz+movk), RIP push, the two frame pops, bl -> insert 4
 * words before the bl. */
    case ST_IRET_CS:
        if (IRET_STR_PRE(w)) { s->state = ST_IRET_DESC; s->budget = 6; }
        else if (!--s->budget) iret_miss(s, bo, w);
        break;
    case ST_IRET_DESC:
        if (IRET_ADRP_X1(w)) { s->state = ST_IRET_ADD; s->budget = 2; }
        else if (IRET_MOVZ_X1(w)) { s->D = RD(w); s->state = ST_IRET_MOVK; s->budget = 5; }
        else if (!--s->budget) iret_miss(s, bo, w);
        break;
    case ST_IRET_ADD:
        if (IRET_ADD_X1(w)) { s->D = 1; s->state = ST_IRET_PUSH2; s->budget = 2; }
        else if (!--s->budget) iret_miss(s, bo, w);
        break;
    case ST_IRET_MOVK:
        if (IRET_MOVK_X1(w)) { /* stay: 0-3 movks depending on the address */ }
        else if (IRET_STR_PRE(w) && RD(w) == s->D) { s->state = ST_IRET_POP1; s->budget = 2; }
        else if (!--s->budget) iret_miss(s, bo, w);
        break;
    case ST_IRET_PUSH2:
        if (IRET_STR_PRE(w) && RD(w) == s->D) { s->state = ST_IRET_POP1; s->budget = 2; }
        else if (!--s->budget) iret_miss(s, bo, w);
        break;
    case ST_IRET_POP1:
        if (IRET_LDP_POST(w)) { s->state = ST_IRET_POP2; s->budget = 4; }
        else if (!--s->budget) iret_miss(s, bo, w);
        break;
    case ST_IRET_POP2:
        if (IRET_LDP_PLN(w)) { s->state = ST_IRET_BL; s->budget = 2; }
        else if (!--s->budget) iret_miss(s, bo, w);
        break;
    case ST_IRET_BL:
        if (IS_BL(w)) {
            inslog(3, bo, w, 0, 0);
            if (insert4(bo, IRET_INSERT)) { ctl_inc(13); note_event(3); }
            else ctl_inc(14);
        } else { iret_miss(s, bo, w); break; }  /* r13: was a silent miss */
        s->state = ST_IDLE;  /* one-shot: bl seen or budget gone, done */
        break;

    /* --- r19/r21 syscall-site chain: the per-site emission for a translated
     * `syscall` (movz x28,lsl#32 carrier, movk/movk address words, bl to the
     * thunk — see the block comment above). Address words are family-matched
     * (the emitter may skip zero halves), the bl ends it; the divert goes in
     * front of the bl and the chain is done (one emission per site). r21:
     * sysc_last remembers the newest site word so insert8 can verify and
     * replay the one sitting at X-4. */
    case ST_SYSC_MOVZ:
        if (IS_SYSC_MOVK16_X28(w) || IS_SYSC_MOVK0_X28(w)) { s->state = ST_SYSC_MOVK; s->budget = 3; s->sysc_last = w; }
        else if (IS_BL(w)) {
            u64 stub = (u64)&ntfix_macfast;
            inslog(6, bo, w, 0, 0);
            if (insert8(bo, stub, s->sysc_last)) {
                ctl_inc(CTL_SYSC_INS); note_event(6); base_tally_insert(bo[0]);
            } else ctl_inc(CTL_SYSC_MISS);
            s->state = ST_IDLE;
        }
        else if (!--s->budget) { ctl_inc(CTL_SYSC_MISS); idle_word(s, w); }
        break;
    case ST_SYSC_MOVK:
        if (IS_BL(w)) {
            u64 stub = (u64)&ntfix_macfast;
            inslog(6, bo, w, 0, 0);
            if (insert8(bo, stub, s->sysc_last)) {
                ctl_inc(CTL_SYSC_INS); note_event(6); base_tally_insert(bo[0]);
            } else ctl_inc(CTL_SYSC_MISS);
            s->state = ST_IDLE;
        }
        else if (IS_SYSC_MOVK16_X28(w) || IS_SYSC_MOVK0_X28(w)) { s->sysc_last = w; /* stay: address words */ }
        else if (!--s->budget) { ctl_inc(CTL_SYSC_MISS); idle_word(s, w); }
        break;

    default:
        s->state = ST_IDLE;
    }
}

/* --- r9: the context converters (checks 2 and 4) ---------------------------
 *
 * /usr/libexec/rosetta/runtime owns the x86-context compose/decompose
 * (ThreadContextRegisterState.cpp et al.; NOT in libRosettaRuntime's text).
 * Two tiny leaf functions there translate between the per-thread parked
 * representation — NZCV value (SF/ZF/CF/OF at 31..28, AF parked 27, PF
 * parked 26) + x17 shadow (DF/AC/ID/TF at bits 1-4) — and an x86 EFLAGS
 * word. Neither builds or reads bit 14 (NT). These are full replacements,
 * installed as 16-byte abs jumps at their entries; every bit below mirrors
 * the original disassembly (/tmp/rosetta-re/runtime.asm 0x198b8/0x1992c)
 * EXCEPT the marked NT lines. Differential-tested on the host against the
 * original machine code (host_payload_test.c). */

u64 ntfix_compose(u64 nzcv, u64 shadow) {
    /* x86 EFLAGS from parked NZCV + shadow; original at 0x198b8 */
    u64 f = (nzcv & 0x80000000ull) ? 0x282ull : 0x202ull; /* SF via cmn/csel base */
    f |= (nzcv >> 24) & 0x40ull;      /* ZF  <- NZCV bit 30 */
    f |= (nzcv >> 17) & 0x800ull;     /* OF  <- NZCV bit 28 */
    f |= (nzcv >> 23) & 0x10ull;      /* AF  <- parked bit 27 */
    f |= (nzcv >> 24) & 0x4ull;       /* PF  <- parked bit 26 */
    if (!(nzcv & 0x20000000ull)) f |= 1ull;  /* CF = !ARM C (tst+cset eq) */
    f |= ((shadow >> 1) & 1ull) << 10;       /* DF  <- shadow 1 */
    f |= ((shadow >> 2) & 1ull) << 18;       /* AC  <- shadow 2 */
    f |= ((shadow >> 3) & 1ull) << 21;       /* ID  <- shadow 3 */
    f |= ((shadow >> 4) & 1ull) << 8;        /* TF  <- shadow 4 */
    f |= ((shadow >> 5) & 1ull) << 14;       /* NT  <- shadow 5  (r9 fix) */
    return f;
}

void ntfix_decompose(u64 f, u64 *nzcv_out, u64 *shadow_out) {
    /* parked NZCV + shadow from x86 EFLAGS; original at 0x1992c */
    u64 n = (f << 24) & 0xc0000000ull;       /* SF->31, ZF->30 */
    n |= (f & 1ull) << 29;                    /* CF (inverted next line) */
    n |= ((f >> 11) & 1ull) << 28;            /* OF */
    n |= ((f >> 4) & 1ull) << 27;             /* AF parked */
    n |= ((f >> 2) & 1ull) << 26;             /* PF parked */
    n ^= 0x20000000ull;                       /* CF invert (eor) */
    *nzcv_out = n;

    u64 s = ((f >> 10) & 1ull) << 1;          /* DF  -> shadow 1 (orig: (f>>9)&2) */
    s |= ((f >> 18) & 1ull) << 2;             /* AC  -> shadow 2 (orig: (f>>16)&4) */
    s |= ((f >> 21) & 1ull) << 3;             /* ID  -> shadow 3 (orig: (f>>18)&8) */
    s |= ((f >> 8) & 1ull) << 4;              /* TF  -> shadow 4 (orig: (f>>4)&0x10) */
    s |= ((f >> 14) & 1ull) << 5;             /* NT  -> shadow 5  (r9 fix) */
    *shadow_out = s;
}

/* --- r19: ntfix_macfast, the syscall divert stub (naked arm64) ---------------
 *
 * Reached by `blr x26` at X+24 of the v6 insert in front of the SITE's bl
 * to the thunk (see the r19 block comment at the top of the file). Entry
 * state: x18 = ctx (UNTAGGED — normal translated code; every deref works
 * either way under TBI), x0 = syscall number (the guest's own rax, its
 * materialisation replayed at X+4), x28 = RIP_sig (the site's movz/movk
 * carrier, replayed at X+4), x30 = lr = X+28 — the bail `b #-28` back to
 * X, which the fixup patch has turned into the site's REAL relocated bl
 * (r21: the insert never executes X itself; the pre-v6 shapes put live
 * words there and the patch made them undefined — the translator assert).
 * The ctx shadow
 * slots do NOT hold the guest yet — the thunk's entry save never ran — so
 * the stub performs it first, replicating the thunk entry at page+0x200:
 * x0-x15 -> ctx+0x10..0x8f, NZCV -> +0x90, x17 -> +0x98, LR -> +0x8.
 *
 * Performs EXACTLY Wine's sigsys_handler semantics (signal_x86_64.c:2937,
 * verified against source, not from notes):
 *   frame->rcx = RIP_sig; frame->rip = RIP_sig + 0xb
 *   frame->eflags = EFL_sig; frame->restore_flags = 0 (DWORD)
 *   RCX_sig = frame; R11_sig = eflags        (the syscall instruction's
 *                                             architectural rcx/r11 effects)
 *   if TF: EFL_sig &= ~0x100; restore_flags |= CONTEXT_CONTROL
 *   RIP_sig = __wine_syscall_dispatcher_prolog_end_ptr
 * EFL_sig = compose(parked NZCV, parked shadow) — the same ntfix_compose the
 * exception server's own converter uses (we own it), so parity is exact by
 * construction, NT included. The CW Hack 24265 mxcsr dance forces the restored
 * MXCSR to equal the live one; our path never captures/restores FPU state, so
 * the live value persists — identical outcome, nothing to replicate.
 *
 * Resume replicates the page epilogue at page+0x2560 (0x102e85560-0x102e8560c
 * in sys-8): record pc -> ctx+0x1d0, in-runtime byte ctx+0x208, call
 * table[0](ctx, lr, x86 RIP) — runtime+0x5964 installs the TB entry at
 * ctx+0x8 — clear the byte, and if ctx+0x8 changed, the verbatim restore of
 * 0x102e855dc: parked NZCV, parked x17, all GPR slots, x30 from ctx+0x8,
 * untag x18 (no-op untagged), ret.
 * The epilogue's pending-event branch (ctx+0x1c8 -> mach_msg wait) is NOT
 * replicated: nothing raised an exception here, and a pending event stays
 * pending until the next checkpoint (a brk or epilogue — microseconds at the
 * game's 32k syscalls/s). Accepted in R19.
 *
 * Guards (side-effect-free, any failure -> br x30 = re-run the stock bl):
 *   g_ctl present; ctl[35] published and canonical user VA
 *   class 0 or 7 only (1/2/3 are real macOS syscalls — never diverted)
 *   TPIDRRO_EL0 -> TEB, [TEB+0x30]==TEB, frame=[TEB+0x378] != 0
 *   [TEB+0x330] slot canonical (bail otherwise). Since r21f an installed
 *   callback is NOT a bail: like sigsys_handler we OR RESTORE_FLAGS_
 *   INSTRUMENTATION into frame->restore_flags (THE FINALS installs one).
 *   Since r21e: TF live (x17 bit4 or ctx+0x444) → bail to stock.
 * The guards touch only x22-x25 (Rosetta-internal; the site template itself
 * clobbers x22). x30 does NOT survive the Rosetta runtime call (and the
 * restore block re-loads it from ctx+0x8), so it is parked at [sp,#-16]
 * before the save: the guard bail branches while lr is still pristine, the
 * late bail pops it into x27 AFTER the restore's x30 load and br x27. Bail
 * safety: before the save it
 * is exact by construction; after it, the no-entry case runs the full
 * restore block FIRST (un-doing every live clobber — compose and the
 * runtime call own x0/x1/x19+) and then branches to the bl with the
 * original machine state, leaving only idempotent frame writes and the ctx
 * register edits the stock handler would make anyway. */
/* arm64-callable ctl_inc for the naked stub (ctl_inc itself is static and
 * may be inlined away; this one keeps a stable symbol). x0 = ctl index. */
void ntfix_ctl_hit(u64 i) { ctl_inc(i); }

__attribute__((naked)) void ntfix_macfast(void)
{
    __asm__ volatile(
        /* guards: BEFORE the save, guest-visible x0-x15 and NZCV are live —
         * only Rosetta-internal x22-x25 are touched (never restored by any
         * exit; the site template itself clobbers x22). */
        "   adrp x22, _g_ctl@PAGE\n"
        "   ldr  x22, [x22, _g_ctl@PAGEOFF]\n"
        "   cbz  x22, 99f\n"
        "   add  x24, x22, #0x128\n"          /* &ctl[37] (ldaxr: no imm) */
        "97: ldaxr x23, [x24]\n"
        "   add  x23, x23, #1\n"
        "   stxr  w25, x23, [x24]\n"
        "   cbnz w25, 97b\n"
        /* r21c: per-guard bail histogram — one byte per guard in ctl[40]
         * (+0x140): b0 no-target b1 non-canonical b2 class b3 TPIDRRO-null
         * b4 TEB-self b5 no-frame b6 instrumentation-cb. Non-atomic byte
         * counts (races only smear totals); x22-x25 only, all dead on the
         * bail paths, g_ctl re-derived where x22 no longer holds it. */
        "   ldr  x25, [x22, #0x118]\n"        /* x25 = ctl[35] target  */
        "   cbnz x25, 11f\n"
        "   ldrb w23, [x22, #0x140]\n"
        "   add  w23, w23, #1\n"
        "   strb w23, [x22, #0x140]\n"
        "   b    99f\n"
        "11: lsr  x24, x25, #47\n"
        "   cbz  x24, 12f\n"                  /* canonical user VA     */
        "   ldrb w23, [x22, #0x141]\n"
        "   add  w23, w23, #1\n"
        "   strb w23, [x22, #0x141]\n"
        "   b    99f\n"
        "12: and  w22, w0, #0x07000000\n"
        "   cbz  w22, 14f\n"
        "   movz w24, #0x0700, lsl #16\n"
        "   cmp  w22, w24\n"
        "   b.eq 14f\n"                       /* only class 0 / 7      */
        "   ldrb w23, [x22, #0x142]\n"
        "   add  w23, w23, #1\n"
        "   strb w23, [x22, #0x142]\n"
        "   b    99f\n"
        "14: mrs  x22, TPIDRRO_EL0\n"
        "   bic  x22, x22, #7\n"
        "   cbnz x22, 15f\n"
        "   adrp x24, _g_ctl@PAGE\n"
        "   ldr  x24, [x24, _g_ctl@PAGEOFF]\n"
        "   ldrb w23, [x24, #0x143]\n"
        "   add  w23, w23, #1\n"
        "   strb w23, [x24, #0x143]\n"
        "   b    99f\n"
        /* r21c/ais6: TPIDRRO in our setup is the native PTHREAD TEB
         * (mac_gsbase_switching=0 — %gs truth comes from exec regions, the
         * cthread self never moves to the TEB), so the old single-hop
         * [TPIDRRO+0x30]==TPIDRRO check bailed on EVERY entry (20039/20043
         * incl. hot loops — the byte wrapped 78x). Measured:
         * [pthread+0x30] = 0x7ffc0000 = the Windows TEB (page-aligned,
         * top-down from 0x7ffe0000; confirmed against %gs:0x30 by the
         * probe). Two-hop resolution, fail-closed: candidate validated
         * (nonzero, canonical, page-aligned) BEFORE its deref, then its
         * self-pointer must match. */
        "15: ldr  x23, [x22, #0x30]\n"
        "   cmp  x23, x22\n"
        "   b.eq 16f\n"                       /* TPIDRRO IS the TEB     */
        "   cbz  x23, 19f\n"                  /* candidate TEB checks   */
        "   lsr  x24, x23, #47\n"
        "   cbnz x24, 19f\n"                  /* canonical user VA      */
        "   tst  x23, #0xfff\n"
        "   b.ne 19f\n"                       /* TEBs are page-aligned  */
        "   ldr  x24, [x23, #0x30]\n"
        "   cmp  x24, x23\n"
        "   b.ne 19f\n"                       /* [TEB+0x30]==TEB        */
        "   mov  x22, x23\n"                  /* x22 = Windows TEB      */
        "   b    16f\n"
        /* TEB resolution failed — dump the pointers for the driver log
         * (ctl[41] raw TPIDRRO, [42] bic'd base, [43] [base+0x30],
         * [44] [base+0x378]) and count in ctl[45] (full word: ais6's byte
         * lane wrapped at 255). x22-x24 only, x25 (live target) kept. */
        "19: mrs  x24, TPIDRRO_EL0\n"
        "   adrp x23, _g_ctl@PAGE\n"
        "   ldr  x23, [x23, _g_ctl@PAGEOFF]\n"
        "   str  x24, [x23, #0x148]\n"        /* ctl[41] raw TPIDRRO    */
        "   str  x22, [x23, #0x150]\n"        /* ctl[42] bic'd base     */
        "   ldr  x24, [x22, #0x30]\n"
        "   str  x24, [x23, #0x158]\n"        /* ctl[43] [base+0x30]    */
        "   ldr  x24, [x22, #0x378]\n"
        "   str  x24, [x23, #0x160]\n"        /* ctl[44] [base+0x378]   */
        "   ldr  x24, [x23, #0x168]\n"        /* ctl[45] fail count     */
        "   add  x24, x24, #1\n"
        "   str  x24, [x23, #0x168]\n"
        "   ldrb w24, [x23, #0x144]\n"
        "   add  w24, w24, #1\n"
        "   strb w24, [x23, #0x144]\n"
        "   b    99f\n"
        "16: ldr  x23, [x22, #0x378]\n"       /* x23 = syscall_frame   */
        "   cbnz x23, 17f\n"
        "   adrp x24, _g_ctl@PAGE\n"
        "   ldr  x24, [x24, _g_ctl@PAGEOFF]\n"
        "   ldrb w23, [x24, #0x145]\n"
        "   add  w23, w23, #1\n"
        "   strb w23, [x24, #0x145]\n"
        "   b    99f\n"
        /* ais7: [TEB+0x330] is NEVER null under Wine — signal_start_thread
         * stores &instrumentation_callback there (signal_x86_64.c:3212) and
         * the dispatcher DEREFERENCES it twice ("movq 0x330(%r13),%r10;
         * movq (%r10),%r10; test %r10,%r10; jz" :1943-1946): the slot is a
         * pointer to the global; a callback is installed only when *slot
         * != 0. The old single-load guard bailed on EVERY entry (ais7:
         * guards byte6 == entered, 6/14/22/29). Same fail-closed shape as
         * the two-hop TEB guard: canonical-check the slot before the deref;
         * x22 (TEB) is dead past this point, borrow it. */
        /* r21f (2026-11-09): an installed callback is no longer a bail.
         * THE FINALS installs one (second game run on -42: entered 4.7M,
         * hits 137, every other entry bailed here — guards byte6 wrapping
         * every poll). sigsys_handler does NOT decline it either: it sets
         * frame->restore_flags |= RESTORE_FLAGS_INSTRUMENTATION (0x10000,
         * signal_x86_64.c:2954) and the dispatcher's return path enters the
         * callback (re-reading it via [TEB+0x330], r10 = return address,
         * :3613-3629). So: carry the callback VALUE in x24 to the
         * restore_flags write below (0 = none). x24 is dead on the main path
         * otherwise and callee-saved across bl _ntfix_compose. The
         * non-canonical-slot bail (20) stays fail-closed. */
        "17: ldr  x24, [x22, #0x330]\n"
        "   cbz  x24, 18f\n"                  /* no slot: x24 = 0 = none */
        "   lsr  x22, x24, #47\n"
        "   cbnz x22, 20f\n"                  /* non-canonical slot    */
        "   ldr  x24, [x24]\n"                /* x24 = the callback    */
        "18:\n"
        /* r21e TF guard (game crash 2026-11-08 19:20: STATUS_SINGLE_STEP,
         * ExceptionAddress = published target+4 = the SECOND dispatcher
         * instruction — a step trap after one executed instruction, so TF
         * was live at our dispatcher entry). table[0]'s 0x59c4 TB lookup
         * takes the ctx TF byte as a key; a step-mode entry TB traps at
         * exactly +4. Probe (tf_syscall_probe): stock sigsys handles TF+
         * syscall CORRECTLY (post-syscall step at the GUEST rip, rax
         * intact) for every arming mode we can produce, and the fast path
         * matched it byte-for-byte there — but the game's entry stepped.
         * TF-live sites are rare (anti-debug windows) and safe on stock:
         * DECLINE them. Live x17 bit4 (TF) or ctx+0x444 (runtime TF byte)
         * → bail. Count ctl[40] byte7 (0x147). x17 is still LIVE here
         * (the park to ctx+0x98 happens below).
         * REGISTER CONTRACT (r21e-2, the hard way): x23 is the SYSCALL FRAME
         * (label 16: [TEB+0x378]), LIVE across label 18 — the frame build
         * below stores through it (str x28,[x23,#0x10] = frame->rcx …). The
         * first build read the TF byte into w23 and Wine spun: every
         * fast-path entry stored frame fields through a garbage pointer (ais
         * probe stuck at VirtualAlloc, ~170 re-translations/s; capsite
         * passed VACUOUSLY — its 11 entries all bail at guard 2, target
         * unpublished, so label 18 never ran). r21f: x24 now carries the
         * instrumentation callback to the restore_flags write, so the TF
         * byte goes into w22 — dead from guard 7 until `mrs x22, NZCV` in
         * the park. The bail block may clobber x23/x24 like every other
         * guard's bail. None of tbnz/ldrb/cbz touch NZCV — the park below
         * saves entry flags. */
        "   tbnz x17, #4, 22f\n"              /* live shadow TF set     */
        "   ldrb w22, [x18, #0x444]\n"        /* runtime TF byte        */
        "   cbz  w22, 23f\n"
        "22: adrp x23, _g_ctl@PAGE\n"
        "   ldr  x23, [x23, _g_ctl@PAGEOFF]\n"
        "   ldrb w24, [x23, #0x147]\n"
        "   add  w24, w24, #1\n"
        "   strb w24, [x23, #0x147]\n"
        "   b    99f\n"
        "23:\n"
        /* park the bail target: lr does not survive the table[0] call (nor
         * the restore's ctx+0x8 load); sp is ours until the resume */
        "   str  x30, [sp, #-16]!\n"
        /* the thunk entry's save (page+0x200): park guest state into ctx */
        "   stp  x0, x1, [x18, #0x10]\n"
        "   stp  x2, x3, [x18, #0x20]\n"
        "   stp  x4, x5, [x18, #0x30]\n"
        "   stp  x6, x7, [x18, #0x40]\n"
        "   stp  x8, x9, [x18, #0x50]\n"
        "   stp  x10, x11, [x18, #0x60]\n"
        "   stp  x12, x13, [x18, #0x70]\n"
        "   stp  x14, x15, [x18, #0x80]\n"
        "   mrs  x22, NZCV\n"
        "   str  x22, [x18, #0x90]\n"
        "   str  x17, [x18, #0x98]\n"
        "   str  x30, [x18, #0x8]\n"
        /* frame + context writes, sigsys_handler parity */
        "   str  x28, [x23, #0x10]\n"         /* frame->rcx = RIP_sig  */
        "   add  x13, x28, #0xb\n"
        "   str  x13, [x23, #0x70]\n"         /* frame->rip = +0xb     */
        "   ldr  x0, [x18, #0x90]\n"          /* parked NZCV           */
        "   ldr  x1, [x18, #0x98]\n"          /* parked shadow         */
        "   bl   _ntfix_compose\n"
        "   str  x0, [x23, #0x80]\n"          /* frame->eflags         */
        "   str  x23, [x18, #0x18]\n"         /* RCX slot = frame      */
        "   str  x0, [x18, #0x68]\n"          /* R11 slot = eflags     */
        /* restore_flags, sigsys_handler order: = 0; |= INSTRUMENTATION if a
         * callback is installed (r21f, x24 from guard 7); |= CONTEXT_CONTROL
         * if TF */
        "   mov  w13, #0\n"
        "   cbz  x24, 3f\n"
        "   movz w13, #1, lsl #16\n"          /* RESTORE_FLAGS_INSTRUMENTATION */
        "3: tbz  x0, #8, 2f\n"                /* TF?                   */
        "   orr  w13, w13, #1\n"              /* CONTEXT_CONTROL       */
        "   ldr  x14, [x18, #0x98]\n"
        "   bic  x14, x14, #0x10\n"           /* EFL_sig &= ~0x100     */
        "   str  x14, [x18, #0x98]\n"
        "2: str  w13, [x23, #0xb4]\n"         /* frame->restore_flags  */
        /* resolve + resume: the page-epilogue call, verbatim shape */
        "   ldr  x14, [x18, #0x8]\n"          /* baseline              */
        "   adr  x29, .\n"
        "   str  x29, [x18, #0x1d0]\n"        /* recorded pc           */
        "   mov  w29, #1\n"
        "   strb w29, [x18, #0x208]\n"        /* in-runtime byte       */
        /* ais8: table[0]'s install is CONDITIONAL on the pending word —
         * 0x5908 swp's ctx+0x1c8 to 0, returns the OLD value, and 0x5964
         * takes the install path (0x59c4 → TB lookup → str ctx+0x8) only
         * when bit9 is set in it. The epilogue's callers always have bit9
         * queued; our calls had w=0 → no install → bail5 on every entry.
         * Queue the continuation ourselves — fail-closed: anything already
         * pending (word != 0) belongs to the stock flow, bail. bit0 stays
         * clear: that path parks rip at ctx+0x1d8 and calls 0xebc4
         * (exception-server notify), which is not our case. */
        "   ldr  w13, [x18, #0x1c8]\n"        /* pending FLAGS — 32-bit!  */
        "   cbz  w13, 21f\n"                  /* 0x1cc is a SEPARATE      */
        "   adrp x23, _g_ctl@PAGE\n"          /* payload field; a 64-bit  */
        "   ldr  x23, [x23, _g_ctl@PAGEOFF]\n"/* access reads/writes both */
        "   str  x13, [x23, #0x130]\n"        /* (ais10: busy-bails were  */
        "   b    5f\n"                        /* the payload, flags were  */
        "21: mov  w13, #0x200\n"              /* 0 — zero-extended dump)  */
        "   str  w13, [x18, #0x1c8]\n"        /* bit9: RIP continuation   */
        "   adr  x1, 3f\n"                    /* lr = after the blr    */
        "   mov  x2, x25\n"                   /* x86 RIP = prolog_end  */
        "   ldr  x29, [x18]\n"
        "   ldr  x29, [x29]\n"                /* table[0]              */
        "   mov  x0, x18\n"
        "   blr  x29\n"
        "3: strb wzr, [x18, #0x208]\n"
        "   ldr  x15, [x18, #0x8]\n"
        "   cmp  x15, x14\n"
        "   b.eq 5f\n"                        /* no entry: clean bail  */
        "   adrp x22, _g_ctl@PAGE\n"          /* count the engage      */
        "   ldr  x22, [x22, _g_ctl@PAGEOFF]\n"
        "   cbz  x22, 4f\n"
        "   mov  x0, #36\n"
        "   bl   _ntfix_ctl_hit\n"
        "4:\n"
        /* restore block 0x102e855dc, verbatim; x30 = the installed entry */
        "   ldp  x29, x17, [x18, #0x90]\n"
        "   msr  NZCV, x29\n"
        "   ldp  x14, x15, [x18, #0x80]\n"
        "   ldp  x12, x13, [x18, #0x70]\n"
        "   ldp  x10, x11, [x18, #0x60]\n"
        "   ldp  x8, x9, [x18, #0x50]\n"
        "   ldp  x6, x7, [x18, #0x40]\n"
        "   ldp  x4, x5, [x18, #0x30]\n"
        "   ldp  x2, x3, [x18, #0x20]\n"
        "   ldp  x0, x1, [x18, #0x10]\n"
        "   ldr  x30, [x18, #0x8]\n"
        "   ldr  x27, [sp], #16\n"            /* drop parked target    */
        "   and  x18, x18, #0x7fffffffffffffff\n"
        "   ret\n"
        /* clean bail: the same restore un-does every live clobber (compose
         * and the runtime call own x0/x1/x19+), then the site's bl re-runs
         * with the original machine state — stock thunk path. */
        "5: adrp x22, _g_ctl@PAGE\n"
        "   ldr  x22, [x22, _g_ctl@PAGEOFF]\n"
        "   cbz  x22, 6f\n"
        "   mov  x0, #39\n"                   /* count the late bail    */
        "   bl   _ntfix_ctl_hit\n"
        "6: ldp  x29, x17, [x18, #0x90]\n"
        "   msr  NZCV, x29\n"
        "   ldp  x14, x15, [x18, #0x80]\n"
        "   ldp  x12, x13, [x18, #0x70]\n"
        "   ldp  x10, x11, [x18, #0x60]\n"
        "   ldp  x8, x9, [x18, #0x50]\n"
        "   ldp  x6, x7, [x18, #0x40]\n"
        "   ldp  x4, x5, [x18, #0x30]\n"
        "   ldp  x2, x3, [x18, #0x20]\n"
        "   ldp  x0, x1, [x18, #0x10]\n"
        "   ldr  x30, [x18, #0x8]\n"
        "   ldr  x27, [sp], #16\n"            /* bail target back      */
        "   and  x18, x18, #0x7fffffffffffffff\n"
        "   br   x27\n"
        /* guard-7 fail (ais7): slot null-canonical check failed or a real
         * instrumentation callback is installed — histogram byte 6 and the
         * stock trap. Reached only by branch; the pass path is at 18. */
        "20: adrp x23, _g_ctl@PAGE\n"
        "   ldr  x23, [x23, _g_ctl@PAGEOFF]\n"
        "   ldrb w24, [x23, #0x146]\n"
        "   add  w24, w24, #1\n"
        "   strb w24, [x23, #0x146]\n"
        "   b    99f\n"
        "99: br   x30\n"                      /* guards: lr untouched  */
    );
}

/* --- r20: ntfix_trapfast — the FINISHED-THUNK syscall divert (naked arm64) ---
 *
 * The r19 stream insert above never fired (entered 0 in-process; and the
 * ring's ldrb/tst/b.eq lead-in does not exist in the finished code — Rosetta
 * transforms the append stream before emission, so anything inserted there is
 * guesswork). The r20 approach does not touch the stream at all: the driver
 * (hooklib) patches each FINISHED 16K thunk region directly, where every word
 * is verifiable beforehand.
 *
 * Ground truth (three thunks, byte-identical across the full 16K; dumps in
 * /tmp/rosetta-re/sys-*.bin, 2026-11-07):
 *   +0x200  strict entry: stp x0,x1..x14,x15 -> ctx+0x10..0x8f; mrs NZCV;
 *           stp x29,x17 -> ctx+0x90; str x30 -> ctx+0x8; orr x18,#bit63
 *           (b2410252); b -> +0x2b10 (word 14000a38 at +0x230)
 *   +0x2b10 class dispatch: and w23,w0,#0x07000000; lsr #24; cmp 1 -> Mach,
 *           2 -> BSD, 3 -> machdep; ANYTHING ELSE (classes 0 and 7 — the
 *           invalid-class traffic Wine's sigsys_handler sees today):
 *   +0x2b30 b +0x2eb8   (word 140000e2 — the ONLY branch to the trap in the
 *           whole region)
 *   +0x2eb8 brk #0x5 (d42000a0); b +0x400 (17ffff51)   — the trap
 *   +0x2ec0 12 words of 0xfffffc90 padding (dead, nothing references it)
 *
 * The driver writes, in the region's last 4K page:
 *   +0x2ec0  adr x27,#-8        (10ffffdb; x27 = &brk#5)
 *   +0x2ec4  movz/movk x26 x4   (the same constant formulas insert8 uses,
 *                                assembler-proven)
 *   +0x2ed8  br x26             (d61f0340) -> ntfix_trapfast
 * and then swaps the single word at +0x2b30 to b +0x2ec0 (140000fc). One
 * aligned word: every concurrent thread sees either the stock branch (-> the
 * untouched trap, today's behaviour) or the trampoline — no torn state. Bail
 * at ANY point = br x27 = run the original brk #5: fail-closed by keeping
 * the trap itself intact.
 *
 * Entry state here is the state the r19 stub was ORIGINALLY designed for:
 * the strict entry's save has already run — guest x0-x15 parked at
 * ctx+0x10..0x8f, NZCV at +0x90, x17 at +0x98, the site's return at ctx+0x8,
 * x18 tagged — and dispatch left x0 = syscall number, x28 = post-syscall
 * x86 RIP (set by the site carrier, untouched since). So no save here: only
 * guards -> frame writes -> the epilogue-style dispatch -> restore.
 * Guards touch only Rosetta-internal x22-x25 (the dispatch itself already
 * clobbers w23/w16) plus the entered counter at ctl[37] (ldaxr/stxr; x18 is
 * the platform register, not usable as a scratch). ctl[33] (the
 * MACFINALS_NT_OFF=syscall kill switch) is checked LIVE so an already-patched
 * thunk can be disarmed without un-patching pages that may be executing.
 * Clean-bail path (dispatch installed no TB entry) counts ctl[39] and runs
 * the stock trap — the frame writes it already made are idempotent, the
 * context slot writes are overwritten by the stock handler. */
__attribute__((naked)) void ntfix_trapfast(void)
{
    __asm__ volatile(
        "   adrp x22, _g_ctl@PAGE\n"
        "   ldr  x22, [x22, _g_ctl@PAGEOFF]\n"
        "   cbz  x22, 99f\n"
        "   add  x24, x22, #0x128\n"          /* &ctl[37] (ldaxr: no imm) */
        "97: ldaxr x23, [x24]\n"
        "   add  x23, x23, #1\n"
        "   stxr  w25, x23, [x24]\n"
        "   cbnz w25, 97b\n"
        "   ldr  x23, [x22, #0x108]\n"        /* ctl[33] disarm          */
        "   cbnz x23, 99f\n"
        "   ldr  x25, [x22, #0x118]\n"        /* x25 = ctl[35] target    */
        "   cbz  x25, 99f\n"
        "   lsr  x24, x25, #47\n"
        "   cbnz x24, 99f\n"                  /* canonical user VA       */
        "   and  w22, w0, #0x07000000\n"
        "   cbz  w22, 1f\n"
        "   movz w24, #0x0700, lsl #16\n"
        "   cmp  w22, w24\n"
        "   b.ne 99f\n"                       /* only class 0 / 7        */
        "1: mrs  x22, TPIDRRO_EL0\n"
        "   bic  x22, x22, #7\n"
        "   cbz  x22, 99f\n"
        "   ldr  x23, [x22, #0x30]\n"
        "   cmp  x23, x22\n"
        "   b.ne 99f\n"                       /* [TEB+0x30]==TEB         */
        "   ldr  x23, [x22, #0x378]\n"        /* x23 = syscall_frame     */
        "   cbz  x23, 99f\n"
        "   ldr  x24, [x22, #0x330]\n"
        "   cbnz x24, 99f\n"                  /* instrumentation cb      */
        /* park the bail target: x27 does not survive the runtime call */
        "   str  x27, [sp, #-16]!\n"
        /* frame + context writes — sigsys_handler parity; the save already
         * ran at the entry, the ctx slots hold the guest state */
        "   str  x28, [x23, #0x10]\n"         /* frame->rcx = RIP_sig    */
        "   add  x13, x28, #0xb\n"
        "   str  x13, [x23, #0x70]\n"         /* frame->rip = +0xb       */
        "   ldr  x0, [x18, #0x90]\n"          /* parked NZCV             */
        "   ldr  x1, [x18, #0x98]\n"          /* parked shadow           */
        "   bl   _ntfix_compose\n"
        "   str  x0, [x23, #0x80]\n"          /* frame->eflags           */
        "   str  x23, [x18, #0x18]\n"         /* RCX slot = frame        */
        "   str  x0, [x18, #0x68]\n"          /* R11 slot = eflags       */
        "   mov  w13, #0\n"
        "   tbz  x0, #8, 2f\n"                /* TF?                     */
        "   mov  w13, #1\n"                   /* CONTEXT_CONTROL         */
        "   ldr  x14, [x18, #0x98]\n"
        "   bic  x14, x14, #0x10\n"           /* EFL_sig &= ~0x100       */
        "   str  x14, [x18, #0x98]\n"
        "2: str  w13, [x23, #0xb4]\n"         /* frame->restore_flags    */
        /* resolve + resume: the page-epilogue call, verbatim shape */
        "   ldr  x14, [x18, #0x8]\n"          /* baseline                */
        "   adr  x29, .\n"
        "   str  x29, [x18, #0x1d0]\n"        /* recorded pc             */
        "   mov  w29, #1\n"
        "   strb w29, [x18, #0x208]\n"        /* in-runtime byte         */
        /* ais8: queue the RIP continuation — see ntfix_macfast; without
         * bit9 in ctx+0x1c8, 0x5908's swp returns 0 and 0x5964 installs
         * nothing. 32-bit only: ctx+0x1cc is a separate payload field. */
        "   ldr  w13, [x18, #0x1c8]\n"        /* pending flags (32-bit)  */
        "   cbnz w13, 5f\n"                   /* busy → clean bail       */
        "   mov  w13, #0x200\n"
        "   str  w13, [x18, #0x1c8]\n"        /* bit9: RIP continuation  */
        "   adr  x1, 3f\n"                    /* lr = after the blr      */
        "   mov  x2, x25\n"                   /* x86 RIP = prolog_end    */
        "   ldr  x29, [x18]\n"
        "   ldr  x29, [x29]\n"                /* table[0]                */
        "   mov  x0, x18\n"
        "   blr  x29\n"
        "3: strb wzr, [x18, #0x208]\n"
        "   ldr  x15, [x18, #0x8]\n"
        "   cmp  x15, x14\n"
        "   adrp x23, _g_ctl@PAGE\n"          /* ais10: post-call ctx+0x8 */
        "   ldr  x23, [x23, _g_ctl@PAGEOFF]\n"/* at ctl[38] (last writer  */
        "   str  x15, [x23, #0x130]\n"        /* wins; busy-bail also     */
        "   b.eq 5f\n"                        /* no entry: clean bail    */
        "   adrp x22, _g_ctl@PAGE\n"          /* count the engage        */
        "   ldr  x22, [x22, _g_ctl@PAGEOFF]\n"
        "   cbz  x22, 4f\n"
        "   mov  x0, #36\n"
        "   bl   _ntfix_ctl_hit\n"
        "4:\n"
        /* restore block 0x102e855dc, verbatim; x30 = the installed entry */
        "   ldp  x29, x17, [x18, #0x90]\n"
        "   msr  NZCV, x29\n"
        "   ldp  x14, x15, [x18, #0x80]\n"
        "   ldp  x12, x13, [x18, #0x70]\n"
        "   ldp  x10, x11, [x18, #0x60]\n"
        "   ldp  x8, x9, [x18, #0x50]\n"
        "   ldp  x6, x7, [x18, #0x40]\n"
        "   ldp  x4, x5, [x18, #0x30]\n"
        "   ldp  x2, x3, [x18, #0x20]\n"
        "   ldp  x0, x1, [x18, #0x10]\n"
        "   ldr  x30, [x18, #0x8]\n"
        "   ldr  x27, [sp], #16\n"            /* drop parked target      */
        "   and  x18, x18, #0x7fffffffffffffff\n"
        "   ret\n"
        /* clean bail: restore un-does every live clobber, then the
         * ORIGINAL brk #5 runs — exactly today's stock behaviour */
        "5: adrp x22, _g_ctl@PAGE\n"
        "   ldr  x22, [x22, _g_ctl@PAGEOFF]\n"
        "   cbz  x22, 6f\n"
        "   mov  x0, #39\n"                   /* count the late bail     */
        "   bl   _ntfix_ctl_hit\n"
        "6: ldp  x29, x17, [x18, #0x90]\n"
        "   msr  NZCV, x29\n"
        "   ldp  x14, x15, [x18, #0x80]\n"
        "   ldp  x12, x13, [x18, #0x70]\n"
        "   ldp  x10, x11, [x18, #0x60]\n"
        "   ldp  x8, x9, [x18, #0x50]\n"
        "   ldp  x6, x7, [x18, #0x40]\n"
        "   ldp  x4, x5, [x18, #0x30]\n"
        "   ldp  x2, x3, [x18, #0x20]\n"
        "   ldp  x0, x1, [x18, #0x10]\n"
        "   ldr  x30, [x18, #0x8]\n"
        "   ldr  x27, [sp], #16\n"            /* bail target back        */
        "   and  x18, x18, #0x7fffffffffffffff\n"
        "   br   x27\n"                       /* the original trap       */
        "99: br   x27\n"                      /* guards: nothing done    */
    );
}
