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

enum { ST_IDLE = 0, ST_SCAT_HEAD, ST_SCAT_MSR, ST_SCAT_TAIL, ST_GATH_ARMED, ST_GATH_TAIL, ST_GATH_PUSH,
       ST_IRET_CS, ST_IRET_DESC, ST_IRET_ADD, ST_IRET_MOVK, ST_IRET_PUSH2,
       ST_IRET_POP1, ST_IRET_POP2, ST_IRET_BL };

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
        int head = MOVZ_0x202(w) || MOVZ_0(w) || IRET_LDRH_CS(w);
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
