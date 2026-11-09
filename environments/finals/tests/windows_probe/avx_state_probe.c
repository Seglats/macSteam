/* Do the AVX registers (full 256-bit YMM state) survive everything Wine does to a thread?
 *
 * THE FINALS crashes with ROSETTA_ADVERTISE_AVX=1 (null read at 0x8 when spawning into the
 * tutorial, 2026-11-02) and runs without it. Before blaming Rosetta's AVX translation, check
 * the Wine side: exception dispatch, NtContinue, Get/SetThreadContext with CONTEXT_XSTATE and
 * thread suspension must all preserve (or faithfully report and apply) the YMM upper halves.
 * Same .exe on MacFinals and Proton; any difference is ours or CrossOver's.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/avx_state_probe.c \
 *     -o runtime/build/probe/avx-state-probe.exe
 *
 * Needs AVX advertised (ROSETTA_ADVERTISE_AVX=1 on the Mac). Only inline asm uses AVX, so the
 * file is built without -mavx and refuses to run the tests when CPUID says no AVX.
 */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <intrin.h>

#ifndef CONTEXT_XSTATE
#define CONTEXT_XSTATE (CONTEXT_AMD64 | 0x40)
#endif
#define FEAT_AVX 2
#define MASK_AVX 4ULL

typedef DWORD64 (WINAPI *GetEnabledXStateFeatures_t)(void);
typedef BOOL (WINAPI *InitializeContext_t)(void *, DWORD, CONTEXT **, DWORD *);
typedef BOOL (WINAPI *SetXStateFeaturesMask_t)(CONTEXT *, DWORD64);
typedef void *(WINAPI *LocateXStateFeature_t)(CONTEXT *, DWORD, DWORD *);
static GetEnabledXStateFeatures_t pGetEnabledXStateFeatures;
static InitializeContext_t pInitializeContext;
static SetXStateFeaturesMask_t pSetXStateFeaturesMask;
static LocateXStateFeature_t pLocateXStateFeature;

struct regs { __attribute__((aligned(32))) unsigned char y[16][32]; };

#define L(n) "vmovdqu " #n "*32(%[i]), %%ymm" #n "\n\t"
#define S(n) "vmovdqu %%ymm" #n ", " #n "*32(%[o])\n\t"
#define LOAD16 L(0) L(1) L(2) L(3) L(4) L(5) L(6) L(7) L(8) L(9) L(10) L(11) L(12) L(13) L(14) L(15)
#define STORE16 S(0) S(1) S(2) S(3) S(4) S(5) S(6) S(7) S(8) S(9) S(10) S(11) S(12) S(13) S(14) S(15)
#define CLOB "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"

static void fill(struct regs *r, unsigned seed)
{
    for (int k = 0; k < 16; k++)
        for (int b = 0; b < 32; b++) r->y[k][b] = (unsigned char)(seed * 131 + k * 32 + b + 1);
}

/* number of registers whose low or high half differs */
static int diff(const struct regs *a, const struct regs *b, int *lo, int *hi)
{
    int n = 0;
    for (int k = 0; k < 16; k++)
    {
        int l = memcmp(a->y[k], b->y[k], 16) != 0, h = memcmp(a->y[k] + 16, b->y[k] + 16, 16) != 0;
        if (lo) *lo += l;
        if (hi) *hi += h;
        n += l || h;
    }
    return n;
}

/* ---- exceptions ---- */

static struct regs exc_in, exc_out;
static volatile int veh_mode;        /* 0 skip only; 1 also check the context and patch ymm5;
                                        2 use AVX in the handler (as vcruntime memcpy does) */
static volatile LONG veh_hits, veh_noxstate, veh_ctx_lo, veh_ctx_hi;
static const unsigned char patch5[16] = { 0xa5, 0xa5, 0x5a, 0x5a, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT) c->Rip += 1;           /* int3: Rip points at it */
    else if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->ExceptionInformation[1] == 0)
        c->Rip += 3;                                           /* mov (%rax),%rax */
    else if (code == EXCEPTION_ILLEGAL_INSTRUCTION) c->Rip += 2; /* ud2 */
    else return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&veh_hits);
    if (veh_mode == 1)
    {
        DWORD len = 0;
        unsigned char *up = (c->ContextFlags & CONTEXT_XSTATE) == CONTEXT_XSTATE
            ? pLocateXStateFeature(c, FEAT_AVX, &len) : NULL;
        if (!up || len < 256) InterlockedIncrement(&veh_noxstate);
        for (int k = 0; k < 16; k++)
        {
            if (memcmp(&c->FltSave.XmmRegisters[k], exc_in.y[k], 16)) InterlockedIncrement(&veh_ctx_lo);
            if (up && len >= 256 && memcmp(up + 16 * k, exc_in.y[k] + 16, 16)) InterlockedIncrement(&veh_ctx_hi);
        }
        if (up && len >= 256) memcpy(up + 16 * 5, patch5, 16);
    }
    if (veh_mode == 2)  /* clobber every YMM register; continuing must restore them from the context */
        __asm__ volatile("vpcmpeqb %%ymm0, %%ymm0, %%ymm0\n\t" ".irp n,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n\t"
                         "vmovdqa %%ymm0, %%ymm\\n\n\t.endr" ::: CLOB);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void raise_int3(void)
{ __asm__ volatile(LOAD16 "int3\n\t" STORE16 :: [i]"r"(&exc_in), [o]"r"(&exc_out) : CLOB); }
static void raise_av(void)
{ __asm__ volatile(LOAD16 "xorl %%eax, %%eax\n\t.byte 0x48,0x8b,0x00\n\t" STORE16
                   :: [i]"r"(&exc_in), [o]"r"(&exc_out) : CLOB, "rax"); }
static void raise_ud2(void)
{ __asm__ volatile(LOAD16 "ud2\n\t" STORE16 :: [i]"r"(&exc_in), [o]"r"(&exc_out) : CLOB); }

static void test_exceptions(void)
{
    static const struct { const char *name; void (*fn)(void); } kinds[] =
        { { "int3", raise_int3 }, { "null read", raise_av }, { "ud2", raise_ud2 } };
    for (int t = 0; t < 3; t++)
    {
        int bad = 0, lo = 0, hi = 0;
        veh_mode = 0; veh_hits = 0;
        for (int n = 0; n < 2000; n++)
        {
            fill(&exc_in, n); memset(&exc_out, 0, sizeof(exc_out));
            kinds[t].fn();
            bad += diff(&exc_in, &exc_out, &lo, &hi) != 0;
        }
        printf("exception %-9s x2000: handler ran %ld, YMM changed after continue in %d runs (low %d, high %d regs)\n",
               kinds[t].name, veh_hits, bad, lo, hi);
    }
    /* what the handler sees, and whether a change it makes to the context is applied */
    veh_mode = 1; veh_hits = veh_noxstate = veh_ctx_lo = veh_ctx_hi = 0;
    int applied = 0, other = 0;
    for (int n = 0; n < 200; n++)
    {
        fill(&exc_in, n + 7); memset(&exc_out, 0, sizeof(exc_out));
        raise_int3();
        struct regs want = exc_in;
        if (!veh_noxstate) memcpy(want.y[5] + 16, patch5, 16);
        applied += !memcmp(exc_out.y[5] + 16, patch5, 16);
        other += diff(&want, &exc_out, NULL, NULL) != 0;
    }
    printf("exception context x200: no AVX state in context %ld, xmm wrong %ld, ymm-high wrong %ld, "
           "ymm5-high edit applied %d/200, registers not as expected after continue %d\n",
           veh_noxstate, veh_ctx_lo, veh_ctx_hi, applied, other);
    /* a handler that uses AVX itself */
    veh_mode = 2; veh_hits = 0;
    for (int t = 0; t < 3; t++)
    {
        int bad = 0, lo = 0, hi = 0, per[16] = { 0 };
        for (int n = 0; n < 500; n++)
        {
            fill(&exc_in, n + 3); memset(&exc_out, 0, sizeof(exc_out));
            kinds[t].fn();
            bad += diff(&exc_in, &exc_out, &lo, &hi) != 0;
            for (int k = 0; k < 16; k++) per[k] += memcmp(exc_in.y[k] + 16, exc_out.y[k] + 16, 16) != 0;
        }
        printf("exception %-9s x500, handler uses AVX: YMM not restored after continue in %d runs (low %d, high %d regs); high wrong in ymm",
               kinds[t].name, bad, lo, hi);
        for (int k = 0; k < 16; k++) if (per[k]) printf(" %d", k);
        printf("\n");
    }
    veh_mode = 0;
}

/* ---- Get/SetThreadContext with CONTEXT_XSTATE on a suspended thread ---- */

struct parked { struct regs in, out; volatile LONG ready, go; };

static DWORD WINAPI parked_thread(void *arg)
{
    struct parked *p = arg;
    __asm__ volatile(LOAD16
                     "lock incl (%[r])\n\t"
                     "1: pause\n\tcmpl $0, (%[g])\n\tje 1b\n\t"
                     STORE16
                     :: [i]"r"(&p->in), [o]"r"(&p->out), [r]"r"(&p->ready), [g]"r"(&p->go) : CLOB);
    return 0;
}

static CONTEXT *alloc_xstate_context(void **mem)
{
    DWORD len = 0; CONTEXT *c = NULL;
    pInitializeContext(NULL, CONTEXT_ALL | CONTEXT_XSTATE, NULL, &len);
    *mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len);
    if (!pInitializeContext(*mem, CONTEXT_ALL | CONTEXT_XSTATE, &c, &len)) return NULL;
    pSetXStateFeaturesMask(c, MASK_AVX);
    return c;
}

static void test_thread_context(void)
{
    int runs = 50, get_lo = 0, get_hi = 0, noav = 0, applied = 0, other = 0, failed = 0;
    for (int n = 0; n < runs; n++)
    {
        struct parked *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*p));
        fill(&p->in, 100 + n);
        HANDLE h = CreateThread(NULL, 0, parked_thread, p, 0, NULL);
        while (!p->ready) Sleep(0);
        Sleep(n % 5);
        void *mem; CONTEXT *c = alloc_xstate_context(&mem);
        unsigned char *up = NULL; DWORD len = 0;
        SuspendThread(h);
        if (!c || !GetThreadContext(h, c)) { failed++; ResumeThread(h); p->go = 1; goto done; }
        up = pLocateXStateFeature(c, FEAT_AVX, &len);
        if (!up || len < 256) noav++;
        for (int k = 0; k < 16; k++)
        {
            get_lo += memcmp(&c->FltSave.XmmRegisters[k], p->in.y[k], 16) != 0;
            get_hi += up && len >= 256 && memcmp(up + 16 * k, p->in.y[k] + 16, 16) != 0;
        }
        if (up && len >= 256) memcpy(up + 16 * 7, patch5, 16);
        if (!SetThreadContext(h, c)) failed++;
        ResumeThread(h);
        p->go = 1;
    done:
        WaitForSingleObject(h, INFINITE); CloseHandle(h);
        struct regs want = p->in;
        if (up && len >= 256) memcpy(want.y[7] + 16, patch5, 16);
        applied += !memcmp(p->out.y[7] + 16, patch5, 16);
        other += diff(&want, &p->out, NULL, NULL) != 0;
        HeapFree(GetProcessHeap(), 0, mem); HeapFree(GetProcessHeap(), 0, p);
    }
    printf("suspended thread x%d: get/set failed %d, no AVX state %d, xmm wrong %d, ymm-high wrong %d, "
           "ymm7-high edit applied %d/%d, registers not as expected after resume %d\n",
           runs, failed, noav, get_lo, get_hi, applied, runs, other);
}

/* ---- preemption: threads hold YMM values across a busy loop while being interrupted ---- */

struct spinner { struct regs in, out; unsigned seed; volatile LONG stop; LONG loops, bad, lo, hi; };

static DWORD WINAPI spinner_thread(void *arg)
{
    struct spinner *s = arg;
    for (unsigned n = 0; !s->stop; n++)
    {
        fill(&s->in, s->seed * 1000 + n);
        __asm__ volatile(LOAD16 "movl $20000, %%ecx\n\t1: decl %%ecx\n\tjnz 1b\n\t" STORE16
                         :: [i]"r"(&s->in), [o]"r"(&s->out) : CLOB, "rcx");
        int lo = 0, hi = 0;
        if (diff(&s->in, &s->out, &lo, &hi)) { s->bad++; s->lo += lo; s->hi += hi; }
        s->loops++;
    }
    return 0;
}

static void test_preemption(const char *name, int nthreads, int suspend, DWORD flags)
{
    struct spinner *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s) * nthreads);
    HANDLE *h = HeapAlloc(GetProcessHeap(), 0, sizeof(*h) * nthreads);
    for (int i = 0; i < nthreads; i++) { s[i].seed = i + 1; h[i] = CreateThread(NULL, 0, spinner_thread, &s[i], 0, NULL); }
    void *mem = NULL; CONTEXT *xc = (flags & 0x40) ? alloc_xstate_context(&mem) : NULL;
    CONTEXT plain; long suspends = 0;
    DWORD end = GetTickCount() + 3000;
    while (GetTickCount() < end)
    {
        if (!suspend) { Sleep(50); continue; }
        for (int i = 0; i < nthreads; i++)
        {
            SuspendThread(h[i]);
            if (flags)
            {
                CONTEXT *c = xc ? xc : &plain;
                if (!xc) c->ContextFlags = flags;
                if (GetThreadContext(h[i], c) && suspend == 2) SetThreadContext(h[i], c);
            }
            ResumeThread(h[i]);
            suspends++;
        }
    }
    long loops = 0, bad = 0, lo = 0, hi = 0;
    for (int i = 0; i < nthreads; i++) s[i].stop = 1;
    WaitForMultipleObjects(nthreads, h, TRUE, INFINITE);
    for (int i = 0; i < nthreads; i++) { loops += s[i].loops; bad += s[i].bad; lo += s[i].lo; hi += s[i].hi; CloseHandle(h[i]); }
    printf("preemption %-34s %2d threads, %6ld suspends: %ld of %ld loops lost YMM (low %ld, high %ld regs)\n",
           name, nthreads, suspends, bad, loops, lo, hi);
    if (mem) HeapFree(GetProcessHeap(), 0, mem);
    HeapFree(GetProcessHeap(), 0, s); HeapFree(GetProcessHeap(), 0, h);
}

int main(void)
{
    int r[4];
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    pGetEnabledXStateFeatures = (void *)GetProcAddress(k32, "GetEnabledXStateFeatures");
    pInitializeContext = (void *)GetProcAddress(k32, "InitializeContext");
    pSetXStateFeaturesMask = (void *)GetProcAddress(k32, "SetXStateFeaturesMask");
    pLocateXStateFeature = (void *)GetProcAddress(k32, "LocateXStateFeature");

    __cpuid(r, 1);
    int avx = (r[2] >> 28) & 1, osxsave = (r[2] >> 27) & 1, xsave = (r[2] >> 26) & 1;
    __cpuidex(r, 7, 0);
    int avx2 = (r[1] >> 5) & 1;
    unsigned long long xcr0 = 0;
    if (osxsave) { unsigned lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); xcr0 = lo | (unsigned long long)hi << 32; }
    printf("CPUID: AVX %d AVX2 %d XSAVE %d OSXSAVE %d; XCR0 %#llx\n", avx, avx2, xsave, osxsave, xcr0);
    if (osxsave)
    {
        __cpuidex(r, 0xd, 0);
        printf("CPUID 0xd.0: features %#x, XSAVE size for XCR0 %u, max %u\n", r[0], r[1], r[2]);
        __cpuidex(r, 0xd, 1);
        printf("CPUID 0xd.1: XSAVEOPT %d XSAVEC %d XGETBV1 %d XSAVES %d\n",
               r[0] & 1, (r[0] >> 1) & 1, (r[0] >> 2) & 1, (r[0] >> 3) & 1);
    }
    DWORD64 en = pGetEnabledXStateFeatures ? pGetEnabledXStateFeatures() : 0;
    unsigned long long kusd_mask = *(volatile unsigned long long *)(0x7ffe0000 + 0x3d8);
    unsigned kusd_size = *(volatile unsigned *)(0x7ffe0000 + 0x3d8 + 16)  /* EnabledFeatures, EnabledVolatileFeatures, Size */;
    printf("Windows: GetEnabledXStateFeatures %#llx; KUSER_SHARED_DATA XState mask %#llx size %u\n",
           (unsigned long long)en, kusd_mask, kusd_size);
    if (!avx || !osxsave || !(xcr0 & 6) || !(en & MASK_AVX) || !pLocateXStateFeature || !pInitializeContext)
    { printf("AVX not available to this process; nothing to test\n"); return 1; }

    AddVectoredExceptionHandler(1, veh);
    test_exceptions();
    test_thread_context();
    SYSTEM_INFO si; GetSystemInfo(&si);
    int over = si.dwNumberOfProcessors * 2;
    test_preemption("(time slicing only)", over, 0, 0);
    test_preemption("(suspend/resume)", 4, 1, 0);
    test_preemption("(suspend+GetThreadContext FULL)", 4, 1, CONTEXT_FULL);
    test_preemption("(suspend+Get/Set FULL)", 4, 2, CONTEXT_FULL);
    test_preemption("(suspend+Get/Set ALL|XSTATE)", 4, 2, CONTEXT_ALL | CONTEXT_XSTATE);
    return 0;
}
