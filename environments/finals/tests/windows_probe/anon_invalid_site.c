/* anon_invalid_site.c — capsite5's question, asked inside a Wine process.
 *
 * Outside Wine, anon-RWX copies of a marked invalid-class (rax=-1) syscall
 * site execute their v6 chain fine (capsite5: entered 4). Inside Wine NOTHING
 * engages (entered 0 everywhere). If an anon copy inside Wine DOES engage,
 * the switch is image/backing classification; if not, the switch is
 * process-level (volume/time/strategy).
 *
 * mingw gcc here has no __try/__except — and an invalid-SSN syscall in Wine
 * may raise and kill us — so: unbuffered output, ANON calls first (each
 * announced before it runs), file-backed warm call LAST. Counters survive
 * via the hook's ctl-<pid>.bin (81 slots) even if the process dies.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/anon_invalid_site.c \
 *     -o runtime/build/probe/anon-invalid-site.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void site1(unsigned long long *sink) {
    /* ais4: markers ride RDI (in dispatcher_return's ALWAYS-restored set,
     * frame->0x28) so they survive both the stock SIGSYS path and the chain.
     * ais3's rdx version crashed: Wine's emulated raw syscall returns with
     * rdx/r8/r9/r10 clobbered (restore_flags=0 — stock behaviour, see NOTES
     * r21c), so nothing after the syscall may lean on them. We instead
     * CAPTURE rdx before/after and rax after — documenting the clobber and
     * the returned status without depending on either. */
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

int main(void) {
    unsigned long long sink[5] = {0, 0, 0, 0, 0};
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ais7: confirm the two-hop TEB hypothesis from the Windows side —
     * the stub's guard-5b dump reads TPIDRRO(native pthread TEB)+0x30 =
     * 0x7ffc0000; if %gs:0x30 (the real TEB) equals that, the stub can
     * resolve TEB = [TPIDRRO+0x30] and the guard stops bailing. Also print
     * the syscall_frame pointer (TEB+0x378) the dispatcher needs. */
    {
        unsigned long long teb, frame;
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));
        frame = *(volatile unsigned long long *)(teb + 0x378);
        printf("TEB %llx  [TEB+378] frame %llx\n", teb, frame);
    }

    /* ais5: call the file-backed site IMMEDIATELY — main entry may precede
     * the driver's first poll tick (~100 ms), i.e. the window where a PE
     * chain provably executed (ais4: entered 1, guard 2 = pre-publish).
     * Then repeat both sites for ~2 s: hook polls (300 ms) bracket WHEN
     * stub entries happen — only early (warmup/tiering model) or always. */
    for (int rep = 0; rep < 3; rep++) {
        memset(sink, 0, sizeof sink);
        printf("PRE rep%d ... ", rep);
        site1(&sink[0]);
        printf("rax %016llx entered?\n", sink[2]);
    }

    void *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE);
    if (!page) { printf("VirtualAlloc failed %lu\n", GetLastError()); return 1; }
    memcpy(page, (const void *)site1, 64);
    void (*anon_site1)(unsigned long long *) =
        (void (*)(unsigned long long *))page;
    printf("anon copy at %p (src %p)\n", page, (const void *)site1);

    for (int rep = 0; rep < 40; rep++) {
        memset(sink, 0, sizeof sink);
        anon_site1(&sink[0]);
        unsigned long long anon_rax = sink[2];
        memset(sink, 0, sizeof sink);
        site1(&sink[0]);
        printf("iter %2d: anon rax %016llx warm rax %016llx\n",
               rep, anon_rax, sink[2]);
        Sleep(50);
    }
    /* ais6: HOT phase — back-to-back, no Sleep. r21b's direct-syscall-probe
     * (hot loop) showed entered 0 while these spaced calls entered every
     * iteration. Tier-up model: Rosetta re-emits hot TBs via the unhooked
     * path, dropping our chain. Prediction: entered grows ~N more times,
     * then stops for the rest of the hot loops (~360 ms each). */
    printf("hot warm begin\n");
    for (int i = 0; i < 20000; i++) site1(&sink[0]);
    printf("hot warm end\n");
    printf("hot anon begin\n");
    for (int i = 0; i < 20000; i++) anon_site1(&sink[0]);
    printf("hot anon end\n");
    /* ais12: THE measurement — same site shape that engages (ais11: hits
     * 20039), timed. Stock cost is 16-19 us (R17); the fast path should
     * land near Proton's ~2 us. rax printed after to prove it still
     * returns c000001c through the dispatcher. */
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        memset(sink, 0, sizeof sink);
        QueryPerformanceCounter(&a);
        for (int i = 0; i < 200000; i++) site1(&sink[0]);
        QueryPerformanceCounter(&b);
        double ns = (double)(b.QuadPart - a.QuadPart) * 1e9
                    / (double)f.QuadPart / 200000.0;
        printf("TIMED site1: 200000 calls, %.0f ns/call, rax %016llx\n",
               ns, sink[2]);
    }
    Sleep(800);   /* let the ring writer flush */
    printf("done\n");
    return 0;
}
