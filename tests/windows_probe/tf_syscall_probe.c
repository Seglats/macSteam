/* tf_syscall_probe.c — reproduce the 2026-11-08 19:20 THE FINALS crash.
 *
 * The game died 2.3 s after start with the Embark crash reporter showing a
 * minidump whose exception record is:
 *     code 0x80000004 (STATUS_SINGLE_STEP), addr = published fast-path
 *     target (prolog_end) + 4, tid = main thread.
 * prolog_end+4 is the SECOND dispatcher instruction — a step trap fires
 * AFTER one executed instruction — so guest TF was live at the moment the
 * fast-path resume entered Wine's dispatcher.
 *
 * The stub's TF handling (clear parked x17 bit 4 when compose reports
 * EFLAGS.TF, frame->eflags keeps TF, restore_flags=CONTEXT_CONTROL) reads
 * as stock parity against sigsys_handler. This probe reproduces the exact
 * guest state — TF live while a direct invalid-class syscall site runs —
 * and logs every single-step address the app sees. Correct (Windows/stock)
 * behaviour: the syscall executes, the step trap fires at a GUEST address
 * (post-syscall instruction). Bug: an address in the dispatcher
 * (published target / target+4).
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/tf_syscall_probe.c \
 *     -o runtime/build/probe/tf-syscall-probe.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static unsigned long long steps[256];
static int nsteps;
static unsigned long long site1_addr, image_base;

/* site1 shape: the proven anon-invalid-site emission (ais11/ais12). */
static void site1(unsigned long long *sink) {
    __asm__ volatile(
        "movabs $0x1122334455667788, %%r8\n\t"
        "movq   %%r8, (%%rdi)\n\t"          /* marker A              */
        "movq   %%rdx, 24(%%rdi)\n\t"       /* rdx BEFORE            */
        "movl   $0xffffffff, %%eax\n\t"
        "syscall\n\t"
        "movabs $0x8877665544332211, %%r8\n\t"
        "movq   %%r8, 8(%%rdi)\n\t"         /* marker B              */
        "movq   %%rax, 16(%%rdi)\n\t"       /* rax AFTER (status)    */
        "movq   %%rdx, 32(%%rdi)\n\t"       /* rdx AFTER (clobber?)  */
        : : "D"(sink) : "r8", "rax", "rdx", "rcx", "r11", "memory");
}

static int in_image(unsigned long long a) {
    return a >= image_base && a < image_base + 0x400000;
}

/* Debugger-style stepping: ALWAYS clear TF while the exception dispatches
 * (else every RtlDispatchException instruction traps too — nested
 * exceptions, 0xC0000025, an artifact of the first probe version), then
 * re-arm it in the context just before continuing. Each guest instruction
 * traps exactly once, cleanly. */
/* v3: the handler is SILENT (record only). A printf inside a VEH during
 * exception dispatch raises a nested c0000008 in the ucrt (observed on the
 * STOCK path, which otherwise survives TF+syscall correctly) and kills the
 * process in a non-continuable spiral — a probe artifact, not Wine's. The
 * +seh trace records every dispatch anyway. */
static LONG WINAPI step_handler(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    unsigned long long a;
    if (r->ExceptionCode != (DWORD)0x80000004 /* STATUS_SINGLE_STEP */)
        return EXCEPTION_CONTINUE_SEARCH;
    a = (unsigned long long)r->ExceptionAddress;
    if (nsteps < 256) steps[nsteps] = a;
    nsteps++;
    ep->ContextRecord->EFlags &= ~0x100u;
    if (nsteps < 40 && in_image(a))
        ep->ContextRecord->EFlags |= 0x100u;   /* re-arm: keep stepping */
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void) {
    unsigned long long sink[5];
    void *veh = NULL;
    setvbuf(stdout, NULL, _IONBF, 0);

    image_base = (unsigned long long)GetModuleHandleA(NULL);
    site1_addr = (unsigned long long)(void *)&site1;
    {
        unsigned long long teb, frame;
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
        frame = *(volatile unsigned long long *)(teb + 0x378);
        printf("image %llx  site1 %llx  TEB %llx  frame %llx\n",
               image_base, site1_addr, teb, frame);
    }

    /* control: plain call, no TF */
    memset(sink, 0, sizeof sink);
    site1(&sink[0]);
    printf("plain: rax %016llx  A %016llx  B %016llx\n",
           sink[2], sink[0], sink[1]);

    /* WARM the site's chain first (r21e): a cold site never routes into the
     * stub, and HOT loops don't either — tier-up re-emits hot TBs via the
     * unhooked path and drops the chain (ais6; confirmed 2026-11-09: 200
     * back-to-back calls → entered 0). SPACED calls engage (ais's Sleep(50)
     * loop entered every iteration). The game's sites are naturally spaced,
     * as they were when it crashed. 30 spaced calls, then arm TF. */
    for (int i = 0; i < 30; i++) { site1(&sink[0]); Sleep(20); }
    printf("warm: 30 spaced calls, rax %016llx\n", sink[2]);

    /* variant A: VEH stepping */
    veh = AddVectoredExceptionHandler(1, step_handler);

    /* TF live across the site: arm TF, immediately call site1. The first
     * trap fires on the call itself; the handler keeps TF set so the walk
     * steps through site1's instructions and INTO the syscall — the game's
     * state at 19:20:39. */
    memset(sink, 0, sizeof sink);
    nsteps = 0;
    __asm__ volatile(
        "movq   %0, %%rax\n\t"
        "pushfq\n\t"
        "orq    $0x100, (%%rsp)\n\t"
        "popfq\n\t"                        /* TF live from here       */
        "call  *%%rax\n\t"
        :
        : "m"(site1_addr), "D"(&sink[0])
        : "rax", "rcx", "r11", "memory");
    printf("tf run: steps %d  rax %016llx  A %016llx  B %016llx\n",
           nsteps, sink[2], sink[0], sink[1]);
    for (int i = 0; i < nsteps && i < 256; i++)
        printf("  step[%d] %llx%s\n", i, steps[i],
               (steps[i] >= image_base && steps[i] < image_base + 0x400000)
                   ? "" : "   <== NOT in probe image");
    printf("=== tf probe done\n");
    return 0;
}
