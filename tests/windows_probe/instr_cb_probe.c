/* instr_cb_probe.c — direct invalid-class syscalls with a process
 * instrumentation callback installed (r21f, 2026-11-09).
 *
 * THE FINALS installs a ProcessInstrumentationCallback; the r21e fast path
 * bailed every such entry to stock (guard 7). r21f instead ORs
 * RESTORE_FLAGS_INSTRUMENTATION into frame->restore_flags exactly like
 * sigsys_handler (signal_x86_64.c:2954). This probe compares stock and fast
 * path on what the callback observes:
 *   - callback invocations per site call (Windows: exactly 1 per syscall
 *     return to user mode)
 *   - r10 handed to the callback (the return address)
 *   - rax (status) and the two markers around the syscall
 * Nothing is printed between the counter reads of one call.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/instr_cb_probe.c \
 *     -o runtime/build/probe/instr-cb-probe.exe -lntdll
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef LONG NTSTATUS_T;
NTSTATUS_T NTAPI NtSetInformationProcess(HANDLE, ULONG, PVOID, ULONG);

volatile LONG64 g_cb;
volatile ULONG64 g_r10;
void cb_thunk(void);
void site1(unsigned long long *sink);
extern char site1_after_syscall[];

/* The callback: count, record r10, return to r10. Flags preserved. */
__asm__(".globl cb_thunk\n"
        "cb_thunk:\n"
        "    pushfq\n"
        "    lock incq g_cb(%rip)\n"
        "    movq %r10, g_r10(%rip)\n"
        "    popfq\n"
        "    jmp *%r10\n");

/* The proven ais site shape; markers ride rdi (dispatcher_return restores
 * it). Win64 ABI: the sink arrives in rcx and rdi is callee-saved — save
 * it, load the sink, restore before ret. A label right after the syscall
 * instruction gives the Windows-expected callback r10. */
__asm__(".globl site1\n"
        "site1:\n"
        "    pushq  %rdi\n"
        "    movq   %rcx, %rdi\n"
        "    movabs $0x1122334455667788, %r8\n"
        "    movq   %r8, (%rdi)\n"
        "    movq   %rdx, 24(%rdi)\n"
        "    movl   $0xffffffff, %eax\n"
        "    syscall\n"
        ".globl site1_after_syscall\n"
        "site1_after_syscall:\n"
        "    movabs $0x8877665544332211, %r8\n"
        "    movq   %r8, 8(%rdi)\n"
        "    movq   %rax, 16(%rdi)\n"
        "    movq   %rdx, 32(%rdi)\n"
        "    popq   %rdi\n"
        "    ret\n");

struct instr_info { ULONG Version; ULONG Reserved; PVOID Callback; };

static NTSTATUS_T set_cb(void *cb) {
    struct instr_info info = {0, 0, cb};
    return NtSetInformationProcess(GetCurrentProcess(), 40 /* ProcessInstrumentationCallback */,
                                   &info, sizeof info);
}

int main(void) {
    unsigned long long sink[5];
    LONG64 d[8]; ULONG64 r[8], rax[8], ma[8], mb[8];
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("site1 %p  after-syscall %p\n", (void *)site1, (void *)site1_after_syscall);

    /* control without a callback */
    memset(sink, 0, sizeof sink);
    site1(sink);
    printf("no-cb: rax %016llx A %016llx B %016llx\n", sink[2], sink[0], sink[1]);

    NTSTATUS_T st = set_cb((void *)cb_thunk);
    printf("set callback: status %08lx  cb count so far %lld\n", (unsigned long)st, (long long)g_cb);

    /* warm (spaced) so the chain is live, as in the game */
    for (int i = 0; i < 30; i++) { site1(sink); Sleep(20); }

    /* single calls, nothing in between the counter reads */
    for (int i = 0; i < 8; i++) {
        memset(sink, 0, sizeof sink);
        LONG64 c0 = g_cb;
        site1(sink);
        LONG64 c1 = g_cb;
        d[i] = c1 - c0; r[i] = g_r10; rax[i] = sink[2]; ma[i] = sink[0]; mb[i] = sink[1];
        Sleep(20);
    }
    for (int i = 0; i < 8; i++)
        printf("call %d: callbacks %lld  r10 %llx (after-syscall%+lld)  rax %016llx  A %s B %s\n",
               i, (long long)d[i], r[i], (long long)(r[i] - (ULONG64)site1_after_syscall), rax[i],
               ma[i] == 0x1122334455667788ull ? "ok" : "BAD", mb[i] == 0x8877665544332211ull ? "ok" : "BAD");

    /* timed */
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        LONG64 c0 = g_cb;
        QueryPerformanceCounter(&a);
        for (int i = 0; i < 200000; i++) site1(sink);
        QueryPerformanceCounter(&b);
        LONG64 c1 = g_cb;
        double ns = (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart / 200000.0;
        printf("TIMED: 200000 calls, %.0f ns/call, callbacks %lld, rax %016llx\n",
               ns, (long long)(c1 - c0), sink[2]);
    }

    st = set_cb(NULL);
    printf("cleared callback: status %08lx\n", (unsigned long)st);
    Sleep(800);
    printf("done\n");
    return 0;
}
