/* host_payload_test.c — replay the recorded r7 emission trace (146 words,
 * /tmp/rosetta-re/append-ring.bin) through the payload's matcher on the host,
 * x86 side, before any arm64 deployment. Gates an ntstub run on:
 *
 *   1. compose helpers produce the four assembler-verified fixup words
 *      (fixverify.S, clang -arch arm64 + otool, 2026-10-31);
 *   2. exactly 3 gather + 2 scatter fixups fire at the right anchors
 *      (before each push str / msr NZCV), 0 skipped;
 *   3. the reconstructed stream (our inserts + the anchor words, mirroring
 *      what the patched helper emits) has the fixups exactly where designed.
 *
 *   clang -arch arm64 -O1 -Wall -o host_payload_test host_payload_test.c && ./host_payload_test
 * (arm64 native: step 4 executes the ORIGINAL converter code from the
 * runtime image in an anon RX page, so the host must be Apple Silicon)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/mman.h>

/* the payload source verbatim — same matcher as ships in the dylib */
#include "payload_arm64.c"

#define NW 146
static uint32_t ring[NW];
static uint32_t stream[512];   /* reconstructed: inserts + anchor words */
static uint32_t stream_n;
static uint64_t fake[512];     /* fake JIT buffer, addressed by base+cursor */
static uint64_t fake2[512];    /* r10b: fresh bufobj — the recorded stream has
                                * the pushf gather anchor (one-shot disarm)
 * immediately before the iret CS load, so the slot is IDLE at arm time */
static uint64_t bo[3];         /* {base, cursor, limit} */
static uint64_t bo2[3];
static uint64_t ctl[32];
/* payload record ring — the payload bounds writes by its compiled RING_WORDS
 * (0x20000), NOT by the caller's array: the ring must match that contract or
 * the payload runs off the end (this test proved it the hard way, 2026-11-01:
 * a 210-word rec let the threaded replay stomp ctl itself). */
static uint32_t rec[0x20000 + 0x1000];  /* ring + r13 inslog */

/* r10b tails (step 5; also used by the r11 thread/recycle tests):
 * A adrp+add desc, B movz/movk desc — the chain must accept either. */
static const uint32_t IRET_TAIL_A[11] = {
    0x7942ee56u,  /* ldrh w22,[x18,#0x176]   CS load (arm)  */
    0x2a1603e1u,  /* mov  w1,w22                             */
    0xf81f8c81u,  /* str  x1,[x4,#-8]!        CS push        */
    0xf0ffffc1u,  /* adrp x1,-5               desc page      */
    0x912c4021u,  /* add  x1,x1,#0xb10        desc(target)   */
    0xf81f8c81u,  /* str  x1,[x4,#-8]!        RIP push       */
    0xa8c15897u,  /* ldp  x23,x22,[x4],#0x10  pop            */
    0x53003ed6u,  /* uxth w22,w22                            */
    0x52800039u,  /* mov  w25,#1                              */
    0xa940609au,  /* ldp  x26,x24,[x4]        pop            */
    0x94000010u,  /* bl   <thunk>             ANCHOR         */
};
static const uint32_t IRET_TAIL_B[12] = {
    0x7942ee56u,  /* ldrh w22,[x18,#0x176]   CS load (arm)  */
    0x2a1603e1u,  /* mov  w1,w22                             */
    0xf81f8c81u,  /* str  x1,[x4,#-8]!        CS push        */
    0xd2c00021u,  /* movz x1,#1,lsl#32        desc(target),  */
    0xf2a12ac1u,  /* movk x1,#0x956,lsl#16    64-bit imm     */
    0xf29403e1u,  /* movk x1,#0xa01f          in 3 words     */
    0xf81f8c81u,  /* str  x1,[x4,#-8]!        RIP push       */
    0xa8c15897u,  /* ldp  x23,x22,[x4],#0x10  pop            */
    0x53003ed6u,  /* uxth w22,w22                            */
    0x52800039u,  /* mov  w25,#1                              */
    0xa940609au,  /* ldp  x26,x24,[x4]        pop            */
    0x94000000u,  /* bl   <thunk>             ANCHOR         */
};

static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

/* r11 threaded replay: 200 buffer lives through one bufobj (step 6) */
struct thr_rep { uint64_t bo[3]; volatile int *go; };
static void *thr_worker(void *arg) {
    struct thr_rep *r = arg;
    uintptr_t fired = 0;
    while (!*r->go) ;
    for (int rep = 0; rep < 200; rep++) {
        r->bo[1] = 0;                       /* fresh buffer life */
        for (int i = 0; i < 12; i++) {
            uint64_t before = r->bo[1];
            ntfix_append(r->bo, IRET_TAIL_B[i], ctl, rec);
            while (before < r->bo[1]) before += 4;   /* our inserts */
            *(uint32_t *)((char *)r->bo[0] + r->bo[1]) = IRET_TAIL_B[i];
            r->bo[1] += 4;
        }
        if (r->bo[1] == 0x40) fired++;      /* 12 anchors + 4 insert words */
    }
    return (void *)fired;
}

int main(void) {
    FILE *f = fopen("/tmp/rosetta-re/append-ring.bin", "rb");
    if (!f) return fail("append-ring.bin missing (rerun r7 ntstub)");
    if (fread(ring, 4, NW, f) != NW) return fail("short ring");
    fclose(f);

    /* 1. compose helpers == assembler-verified words */
    {
        uint32_t a = LSR_W(24, 22, 14), b = BFI_X17_5(24);
        uint32_t c = LSR_W17_5(23), d = BFI_W14(22, 23);
        if (a != 0x530e7ed8u) return fail("LSR_W(24,22,14) != 530e7ed8");
        if (b != 0xb37b0311u) return fail("BFI_X17_5(24) != b37b0311");
        if (c != 0x53057e37u) return fail("LSR_W17_5(23) != 53057e37");
        if (d != 0x331202f6u) return fail("BFI_W14(22,23) != 331202f6");
        printf("compose helpers: verified words reproduced\n");
    }

    /* 2. replay through the payload, simulating the original helper after
     * each ntfix_append call: anchor word stored at base[cursor], +4. The
     * inserts the payload laid down at the cursor go into the stream first. */
    bo[0] = (uint64_t)fake;
    bo[1] = 0;
    bo[2] = sizeof fake;
    ntfix_init(rec, 0, ctl);
    for (int i = 0; i < NW; i++) {
        uint64_t before = bo[1];
        ntfix_append(bo, ring[i], ctl, rec);
        while (before < bo[1]) {        /* our inserted words */
            stream[stream_n++] = *(uint32_t *)((char *)fake + before);
            before += 4;
        }
        *(uint32_t *)((char *)fake + bo[1]) = ring[i];   /* original store */
        bo[1] += 4;
        stream[stream_n++] = ring[i];
    }
    printf("replay: gather fixups %llu, scatter fixups %llu, skipped %llu, stream %u words\n",
           (unsigned long long)ctl[0], (unsigned long long)ctl[1],
           (unsigned long long)ctl[2], stream_n);
    if (ctl[0] != 3) return fail("expected 3 gather fixups");
    if (ctl[1] != 2) return fail("expected 2 scatter fixups");
    if (ctl[2] != 0) return fail("expected 0 skips");

    /* 3. fixups sit exactly before their anchors, with the right encodings */
    {
        int g = 0, s = 0;
        for (uint32_t i = 0; i + 3 < stream_n; i++) {
            /* gather: lsr wT,w17,#5 ; bfi wD,wT,#14 ; push-str */
            if ((stream[i] & 0xfffffc00u) == 0x53057c00u && RN(stream[i]) == 17 &&
                (stream[i + 1] & 0xfffffc00u) == 0x33120000u &&
                PUSH_STR(stream[i + 2]) && RD(stream[i + 2]) == RD(stream[i + 1])) {
                g++;
                printf("gather fixup @stream %u: %08x %08x | push %08x\n",
                       i, stream[i], stream[i + 1], stream[i + 2]);
            }
            /* scatter: lsr wA,wS,#14 ; bfi x17,xA,#5 ; msr NZCV (A = the lsr's Rd) */
            if ((stream[i] & 0xfffffc00u) == 0x530e7c00u &&
                stream[i + 1] == BFI_X17_5(RD(stream[i])) &&
                MSR_NZCV(stream[i + 2])) {
                s++;
                printf("scatter fixup @stream %u: %08x %08x | msr %08x\n",
                       i, stream[i], stream[i + 1], stream[i + 2]);
            }
        }
        if (g != 3) return fail("stream: gather fixups not at push anchors");
        if (s != 2) return fail("stream: scatter fixups not at msr anchors");
    }

    printf("PASS: matcher fires 3+2 fixups at the verified anchors\n");

    /* 4. r9: differential-test the converter replacements against the
     * ORIGINAL machine code copied out of /usr/libexec/rosetta/runtime
     * (compose @0x198b8, decompose @0x1992c). The originals run from an
     * executable anon copy; equality required on every bit except NT. */
    {
        FILE *f = fopen("/tmp/rosetta-re/runtime.img", "rb");
        if (!f) return fail("runtime.img missing (cp /usr/libexec/rosetta/runtime)");
        static unsigned char img[0x30000];
        if (fread(img, 1, sizeof img, f) != sizeof img) return fail("short runtime.img");
        fclose(f);

        void *code = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
        if (code == MAP_FAILED) return fail("anon page for originals");
        memcpy(code, img + 0x198b8, 0x1a0);   /* both functions fit */
        if (mprotect(code, 0x1000, PROT_READ | PROT_EXEC) != 0)
            return fail("mprotect originals RX");

        uint64_t (*orig_compose)(uint64_t, uint64_t) = (void *)((char *)code + 0x0);
        void (*orig_decompose)(uint64_t, uint64_t *, uint64_t *) =
            (void *)((char *)code + (0x1992c - 0x198b8));

        /* vectors: edges + parked/NZCV bit combos + deterministic pseudo-random */
        uint64_t vec_n[24] = {0, 0xffffffffull, 0x7fffffffull, 0x80000000ull,
                              0x20000000ull, 0xdfffffffull, 0x5fffffffull,
                              0x080000000ull >> 1, 0x40000000ull, 0x10000000ull,
                              0x18000000ull, 0x00000002ull};
        uint64_t vec_s[16] = {0, 1, 2, 3, 4, 5, 6, 0x1full, 0x20ull, 0x2full,
                              0x30ull, 0x3full};
        unsigned rng = 0x12345678u;
        for (int i = 12; i < 24; i++) { rng = rng * 1103515245u + 12345u; vec_n[i] = ((uint64_t)rng << 7) ^ rng; }
        for (int i = 12; i < 16; i++) { rng = rng * 1103515245u + 12345u; vec_s[i] = (rng >> 9) & 0x3f; }

        int cmp_ok = 1, dec_ok = 1, nt_absent = 1, nt_fix_ok = 1;
        for (int i = 0; i < 24; i++) {
            for (int j = 0; j < 16; j++) {
                uint64_t nz = vec_n[i], sh = vec_s[j];
                uint64_t want = orig_compose(nz, sh);
                uint64_t got = ntfix_compose(nz, sh);
                /* originals never build bit 14 — the bug we are fixing */
                if (want & 0x4000ull) nt_absent = 0;
                /* with shadow bit 5 clear, replacement must be identical */
                if (!(sh & 0x20ull) && want != got) {
                    printf("compose diff nz=%016llx sh=%016llx want=%08llx got=%08llx\n",
                           (unsigned long long)nz, (unsigned long long)sh,
                           (unsigned long long)want, (unsigned long long)got);
                    cmp_ok = 0;
                }
                /* with bit 5 set, replacement adds exactly bit 14 */
                uint64_t expect = (sh & 0x20ull) ? (want | 0x4000ull) : want;
                if (got != expect) nt_fix_ok = 0;

                /* decompose round: pick flags the original can produce + NT bit */
                uint64_t fl = want | ((sh & 0x20ull) ? 0x4000ull : 0);
                uint64_t on = 0, os = 0, rn = 0, rs = 0;
                orig_decompose(fl, &on, &os);
                ntfix_decompose(fl, &rn, &rs);
                if (!(fl & 0x4000ull)) {
                    if (on != rn || os != rs) {
                        printf("decompose diff fl=%08llx on=%08llx rn=%08llx os=%llx rs=%llx\n",
                               (unsigned long long)fl, (unsigned long long)on,
                               (unsigned long long)rn, (unsigned long long)os,
                               (unsigned long long)rs);
                        dec_ok = 0;
                    }
                    if (os & 0x20ull) nt_absent = 0;   /* original never sets 5 */
                } else {
                    /* ours must equal the original plus exactly shadow bit 5
                     * (the original ignores bit 14 completely) */
                    if (on != rn || rs != (os | 0x20ull)) {
                        printf("decompose-NT diff fl=%08llx on=%08llx rn=%08llx os=%llx rs=%llx\n",
                               (unsigned long long)fl, (unsigned long long)on,
                               (unsigned long long)rn, (unsigned long long)os,
                               (unsigned long long)rs);
                        dec_ok = 0;
                    }
                }
            }
        }
        (void)nt_absent;
        if (!cmp_ok) return fail("ntfix_compose diverges from original");
        if (!nt_fix_ok) return fail("ntfix_compose NT bit wrong");
        if (!dec_ok) return fail("ntfix_decompose diverges from original");
        printf("converters: differential vs original code PASS "
               "(bit 14 absent in originals, added exactly by ours)\n");
    }

    /* 5. r10b: replay BOTH recorded iretq emission tails and require exactly
     * one insert each: the 4 verified words IMMEDIATELY before the thunk bl,
     * nothing else disturbed. Tail A (jitdump, jit-3 @0x1b90) materialises
     * desc(target) with adrp+add; tail B (ntstub ring @1274, 2026-10-31 run)
     * with movz+movk x2 — the same chain must accept either. Tails are at
     * file scope (the r11 tests reuse them). */
    {
        for (int t = 0; t < 2; t++) {
            const uint32_t *tail = t ? IRET_TAIL_B : IRET_TAIL_A;
            uint32_t ntail = t ? 12 : 11;
            uint64_t c13 = ctl[13], c14 = ctl[14];
            uint64_t g0 = ctl[0], g1 = ctl[1], g2 = ctl[2];
            uint32_t start = stream_n;
            bo2[0] = (uint64_t)fake2;
            bo2[1] = 0;
            bo2[2] = sizeof fake2;
            for (uint32_t i = 0; i < ntail; i++) {
                uint64_t before = bo2[1];
                ntfix_append(bo2, tail[i], ctl, rec);
                while (before < bo2[1]) {
                    stream[stream_n++] = *(uint32_t *)((char *)fake2 + before);
                    before += 4;
                }
                *(uint32_t *)((char *)fake2 + bo2[1]) = tail[i];
                bo2[1] += 4;
                stream[stream_n++] = tail[i];
            }
            printf("iret replay %s: fixups %llu->%llu, misses %llu->%llu\n",
                   t ? "B(movz)" : "A(adrp)",
                   (unsigned long long)c13, (unsigned long long)ctl[13],
                   (unsigned long long)c14, (unsigned long long)ctl[14]);
            if (ctl[13] != c13 + 1) return fail("expected exactly 1 more iret fixup");
            if (ctl[14] != c14) return fail("iret insert missed (chain/bounds)");
            if (ctl[0] != g0 || ctl[1] != g1 || ctl[2] != g2)
                return fail("iret replay disturbed the r8 counters");

            uint32_t n = stream_n - start;
            if (n != ntail + 4) return fail("iret stream word count wrong");
            if (!IS_BL(stream[start + n - 1])) return fail("last word is not the bl");
            for (int k = 0; k < 4; k++)
                if (stream[start + n - 5 + k] != IRET_INSERT[k]) {
                    printf("word %d: %08x != %08x\n", k,
                           stream[start + n - 5 + k], IRET_INSERT[k]);
                    return fail("iret insert words wrong or misplaced");
                }
            printf("  insert: %08x %08x %08x %08x | bl %08x\n",
                   stream[start + n - 5], stream[start + n - 4],
                   stream[start + n - 3], stream[start + n - 2], stream[start + n - 1]);
        }
        printf("PASS: both iret tails fire once, 4 verified words before the bl\n");
    }

    /* 6. r11 multi-thread: four real threads, four bufobjs, tail B replayed
     * 200x each with a cursor reset between reps (buffer reuse every time —
     * the game recycles JIT buffers and translates on several threads). Every
     * rep must fire exactly one insert, no misses, no cross-stream inserts:
     * threads must never share a matcher chain (CAS adoption) and a recycled
     * buffer address must not resume its previous chain (cursor regression). */
    {
        enum { NT = 4 };
        static struct thr_rep reps[NT];
        static uint64_t fakes[NT][512];
        memset(reps, 0, sizeof reps);
        volatile int go = 0;
        for (int t = 0; t < NT; t++) {
            reps[t].bo[0] = (uint64_t)fakes[t];
            reps[t].bo[1] = 0;
            reps[t].bo[2] = sizeof fakes[t];
            reps[t].go = &go;
        }
        uint64_t c13 = ctl[13], c14 = ctl[14];
        pthread_t th[NT];
        uintptr_t fired[NT] = {0};
        for (int t = 0; t < NT; t++) pthread_create(&th[t], NULL, thr_worker, &reps[t]);
        go = 1;
        for (int t = 0; t < NT; t++) pthread_join(th[t], (void **)&fired[t]);
        printf("threads: per-worker inserts %llu/%llu/%llu/%llu (want 200 each), "
               "ctl fixups %llu->%llu (want +%d), misses %llu->%llu (want +0)\n",
               (unsigned long long)fired[0], (unsigned long long)fired[1],
               (unsigned long long)fired[2], (unsigned long long)fired[3],
               (unsigned long long)c13, (unsigned long long)ctl[13], NT * 200,
               (unsigned long long)c14, (unsigned long long)ctl[14]);
        if (ctl[13] != c13 + NT * 200) return fail("threaded replay lost inserts (CAS/reset)");
        if (ctl[14] != c14) return fail("threaded replay missed chains");
    }

    /* 7. r11 stale-chain kill: half a tail through a buffer (chain armed at
     * ST_IRET_MOVK), buffer recycled (cursor restarts), then the tail's
     * REMAINDER — which would complete the chain and insert if the stale
     * state survived the reuse. It must not; a full fresh tail afterwards
     * must fire exactly once. */
    {
        bo2[0] = (uint64_t)fake2; bo2[1] = 0; bo2[2] = sizeof fake2;
        for (int i = 0; i < 4; i++) {          /* CS load, mov, CS push, movz */
            uint64_t before = bo2[1];
            ntfix_append(bo2, IRET_TAIL_B[i], ctl, rec);
            while (before < bo2[1]) before += 4;
            *(uint32_t *)((char *)fake2 + bo2[1]) = IRET_TAIL_B[i];
            bo2[1] += 4;
        }
        bo2[1] = 0;                            /* recycled buffer, same address */
        uint64_t c13 = ctl[13], c14 = ctl[14];
        for (int i = 4; i < 12; i++) {         /* would COMPLETE a stale chain */
            uint64_t before = bo2[1];
            ntfix_append(bo2, IRET_TAIL_B[i], ctl, rec);
            while (before < bo2[1]) before += 4;
            *(uint32_t *)((char *)fake2 + bo2[1]) = IRET_TAIL_B[i];
            bo2[1] += 4;
        }
        uint64_t after_completion = ctl[13];   /* read BEFORE the fresh tail */
        for (int i = 0; i < 12; i++) {         /* fresh full tail on it */
            uint64_t before = bo2[1];
            ntfix_append(bo2, IRET_TAIL_B[i], ctl, rec);
            while (before < bo2[1]) before += 4;
            *(uint32_t *)((char *)fake2 + bo2[1]) = IRET_TAIL_B[i];
            bo2[1] += 4;
        }
        printf("stale-chain: completion %s, fresh tail fired %s\n",
               after_completion == c13 ? "silent (correct)" : "INSERTED (BUG)",
               ctl[13] == after_completion + 1 ? "once (correct)" : "wrong");
        if (ctl[13] != c13 + 1) return fail("fresh tail after recycle did not fire exactly once");
        if (ctl[14] != c14) return fail("recycle path counted a miss");
        printf("PASS: CAS adoption + buffer-reuse reset hold up\n");
    }

    /* 8. r13 slot recycling: 1000 distinct buffer objects in turn, one full
     * tail each — far more buffers than slots. The old table kept the first
     * 16 for life and dropped everything after (Wine probe: 151350 of 155046
     * words); every one of these must fire. */
    {
        enum { NB = 1000 };
        static uint64_t bufs[NB][3];
        static uint64_t mem[64];
        uint64_t c13 = ctl[13], c14 = ctl[14], c16 = ctl[16];
        for (int b = 0; b < NB; b++) {
            uint64_t *bb = bufs[b];
            bb[0] = (uint64_t)mem; bb[1] = 0; bb[2] = sizeof mem;
            for (int i = 0; i < 12; i++) {
                ntfix_append(bb, IRET_TAIL_B[i], ctl, rec);
                *(uint32_t *)((char *)mem + bb[1]) = IRET_TAIL_B[i];
                bb[1] += 4;
            }
        }
        printf("recycling: %d buffers, iret fixups %llu->%llu (want +%d), misses +%llu, "
               "dropped heads +%llu\n", NB, (unsigned long long)c13,
               (unsigned long long)ctl[13], NB, (unsigned long long)(ctl[14] - c14),
               (unsigned long long)(ctl[16] - c16));
        if (ctl[13] != c13 + NB) return fail("slots not recycled: inserts lost past the table size");
        if (ctl[14] != c14 || ctl[16] != c16) return fail("recycling path missed or dropped chains");
        printf("PASS: slots recycle across more buffers than the table holds\n");
    }

    /* 9. r14: a literal in the guest code emits the same word as a chain head: `mov r,0x202`
     * gives movz wD,#0x202 and `mov r,0` gives movz wT,#0. Neither may cost the real pushf or
     * popf right after it (Wine probe nt_patterns_probe: 9 of 37 arrangements lost NT). */
    {
        enum { D = 3, T = 5, S = 6, A = 7, FALSE_REG = 9 };
        static const uint32_t gather[] = {
            0x52804040u | D, 0xd503201fu, 0x53047c00u | (17u << 5) | T,
            0x33180000u | (T << 5) | D, 0xf81f8c80u | D};
        static const uint32_t scatter[] = {
            0x52800000u | A, 0x53077c00u | (S << 5) | A, 0x33010000u | (A << 5) | 8u,
            0x53067c00u | (S << 5) | A, 0x33020000u | (A << 5) | 8u, 0x530b7c00u | (S << 5) | A,
            0x33040000u | (A << 5) | 8u, 0xd51b4200u | 8u};
        static const uint32_t none[] = {0};
        static const uint32_t f202[] = {0x52804040u | FALSE_REG};
        static const uint32_t f0[] = {0x52800000u | FALSE_REG};
        static const uint32_t f0202[] = {0x52800000u | FALSE_REG, 0x52804040u | FALSE_REG};
        static const uint32_t f2020[] = {0x52804040u | FALSE_REG, 0x52800000u | FALSE_REG};
        static const uint32_t f202gap[] = {0x52804040u | FALSE_REG, 0xd503201fu, 0xd503201fu, 0xd503201fu};
        static const struct { const char *name; const uint32_t *prefix; int n; } prefixes[] = {
            {"no literal", none, 0}, {"mov r,0x202", f202, 1}, {"mov r,0", f0, 1},
            {"mov r,0; mov r,0x202", f0202, 2}, {"mov r,0x202; mov r,0", f2020, 2}, {"mov r,0x202; nops", f202gap, 4}};
        static uint64_t mem[64];
        for (unsigned p = 0; p < sizeof prefixes / sizeof prefixes[0]; p++) {
            for (int which = 0; which < 2; which++) {
                static uint64_t bb[3];
                const uint32_t *chain = which ? scatter : gather;
                int len = which ? 8 : 5;
                uint64_t before = ctl[which ? 1 : 0];
                bb[0] = (uint64_t)mem; bb[1] = 0; bb[2] = sizeof mem;
                for (int i = 0; i < prefixes[p].n + len; i++) {
                    uint32_t w = i < prefixes[p].n ? prefixes[p].prefix[i] : chain[i - prefixes[p].n];
                    ntfix_append(bb, w, ctl, rec);
                    *(uint32_t *)((char *)mem + bb[1]) = w;
                    bb[1] += 4;
                }
                printf("literal \"%s\" before %s: fixups +%llu\n", prefixes[p].name, which ? "popf" : "pushf",
                       (unsigned long long)(ctl[which ? 1 : 0] - before));
                if (ctl[which ? 1 : 0] != before + 1) return fail("a literal cost the real pushf/popf its fixup");
                if (which == 0) {
                    /* the fixup must use the real chain's registers, not the literal's */
                    uint32_t *words = (uint32_t *)mem;
                    int at = prefixes[p].n + len - 1;  /* lsr, then bfi, then the anchor */
                    if (words[at + 1] != (0x33120000u | (T << 5) | D)) return fail("gather fixup used the wrong registers");
                }
            }
        }
        /* a literal with no pushf after it stays harmless, and a later real chain still fires */
        {
            static uint64_t bb[3];
            uint64_t g = ctl[0], s0 = ctl[1];
            bb[0] = (uint64_t)mem; bb[1] = 0; bb[2] = sizeof mem;
            ntfix_append(bb, 0x52804040u | FALSE_REG, ctl, rec);
            *(uint32_t *)((char *)mem + bb[1]) = 0; bb[1] += 4;
            for (int i = 0; i < 60 && bb[1] + 8 < sizeof mem; i++) {
                ntfix_append(bb, 0xd503201fu, ctl, rec);
                *(uint32_t *)((char *)mem + bb[1]) = 0xd503201fu; bb[1] += 4;
            }
            if (ctl[0] != g || ctl[1] != s0) return fail("a lone literal caused an insert");
        }
        printf("PASS: literals in guest code do not cost a real pushf/popf its fixup\n");
    }
    return 0;
}
