/* What segment selectors and GS/TEB/TLS relationships does a 64-bit process see?
 * Windows x64 user mode has cs=0x33, ss=ds=es=gs=0x2b, fs=0x53, and gs points at the
 * TEB. Linux happens to use the same cs/ss, so Proton matches Windows by construction;
 * macOS uses cs=0x2b ss=0x23, and Wine's exception contexts on MacFinals report those.
 * This prints every value both live (mov from the register) and as seen in an
 * exception context and a GetThreadContext, on the main thread and a second thread,
 * so the same .exe gives a three-way table on MacFinals, real Windows and Proton.
 *
 * Build: x86_64-w64-mingw32-clang -O1 -Wall segment_probe.c -o segment-probe.exe
 * Run the same .exe on MacFinals, real Windows and Proton and compare the output. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <stdio.h>

struct sel { unsigned short cs, ds, es, fs, gs, ss; };

static void live_sel(struct sel *s) {
    __asm__ volatile("mov %%cs,%0" : "=r"(s->cs));
    __asm__ volatile("mov %%ds,%0" : "=r"(s->ds));
    __asm__ volatile("mov %%es,%0" : "=r"(s->es));
    __asm__ volatile("mov %%fs,%0" : "=r"(s->fs));
    __asm__ volatile("mov %%gs,%0" : "=r"(s->gs));
    __asm__ volatile("mov %%ss,%0" : "=r"(s->ss));
}

static void print_sel(const char *what, const struct sel *s) {
    printf("%-34s cs=%04x ds=%04x es=%04x fs=%04x gs=%04x ss=%04x\n",
           what, s->cs, s->ds, s->es, s->fs, s->gs, s->ss);
}

static void print_ctx(const char *what, const CONTEXT *c) {
    printf("%-34s cs=%04x ds=%04x es=%04x fs=%04x gs=%04x ss=%04x eflags=%08lx\n",
           what, c->SegCs, c->SegDs, c->SegEs, c->SegFs, c->SegGs, c->SegSs,
           (unsigned long)c->EFlags);
}

static CONTEXT vctx;
static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    vctx = *ep->ContextRecord;
    ep->ContextRecord->Rip += 1;  /* past the int3 */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void gs_teb(const char *who) {
    void *teb_self, *peb, *tls;
    __asm__ volatile("mov %%gs:0x30,%0" : "=r"(teb_self));
    __asm__ volatile("mov %%gs:0x60,%0" : "=r"(peb));
    __asm__ volatile("mov %%gs:0x58,%0" : "=r"(tls));
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    printf("%-34s gs:[30]==TEB %s  gs:[60]==PEB %s  gs:[58]==TEB+58 %s  StackBase>rsp %s\n", who,
           teb_self == (void *)tib ? "yes" : "NO",
           peb == *(void **)((char *)tib + 0x60) ? "yes" : "NO",
           tls == *(void **)((char *)tib + 0x58) ? "yes" : "NO",
           (char *)tib->StackBase > (char *)&teb_self && (char *)tib->StackLimit < (char *)&teb_self ? "yes" : "NO");
}

/* Is gs:[off] the TEB at every offset, or only at the fields Wine mirrors? Reads each
 * qword of the first 0x1800 bytes both ways and lists the offsets that differ. */
static void gs_scan(const char *who) {
    unsigned char *teb = (unsigned char *)NtCurrentTeb();
    int diff = 0, first = -1;
    SetLastError(0x1234);
    for (unsigned off = 0; off < 0x1800; off += 8) {
        unsigned long long g, t = *(unsigned long long *)(teb + off);
        __asm__ volatile("mov %%gs:(%1),%0" : "=r"(g) : "r"((unsigned long long)off));
        if (g != t) { if (diff < 12) printf("    %s gs:[%04x]=%016llx  TEB+%04x=%016llx\n", who, off, g, off, t);
                      if (first < 0) first = off; diff++; }
    }
    printf("%-34s %d of %d qwords differ between gs:[off] and TEB+off\n", who, diff, 0x1800 / 8);
}

static DWORD WINAPI second(void *arg) {
    (void)arg;
    struct sel s; live_sel(&s);
    print_sel("thread 2: live mov", &s);
    gs_teb("thread 2: gs/TEB");
    gs_scan("thread 2: gs scan");
    Sleep(INFINITE);
    return 0;
}

int main(void) {
    struct sel s;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("windows x64 reference              cs=0033 ds=002b es=002b fs=0053 gs=002b ss=002b\n");

    live_sel(&s);
    print_sel("main: live mov", &s);
    gs_teb("main: gs/TEB");
    gs_scan("main: gs scan");

    AddVectoredExceptionHandler(1, veh);
    __asm__ volatile("int3");
    print_ctx("main: exception context", &vctx);

    CONTEXT c = {0};
    c.ContextFlags = CONTEXT_FULL | CONTEXT_SEGMENTS;
    RtlCaptureContext(&c);
    print_ctx("main: RtlCaptureContext", &c);

    HANDLE t = CreateThread(NULL, 0, second, NULL, 0, NULL);
    Sleep(300);
    SuspendThread(t);
    c.ContextFlags = CONTEXT_FULL | CONTEXT_SEGMENTS;
    if (GetThreadContext(t, &c)) print_ctx("thread 2: GetThreadContext", &c);
    else printf("thread 2: GetThreadContext failed %lu\n", GetLastError());

    /* a far return through the live cs must come back here: does the selector the
     * process reads actually work as a code selector? */
    unsigned long long ok = 0;
    __asm__ volatile(
        "lea 1f(%%rip), %%rax\n"
        "mov %%cs, %%rcx\n"
        "push %%rcx\n"
        "push %%rax\n"
        "lretq\n"
        "1: mov $1, %0\n"
        : "=r"(ok) : : "rax", "rcx", "memory");
    printf("%-34s %s\n", "far return via live cs", ok ? "returned" : "NO");
    return 0;
}
