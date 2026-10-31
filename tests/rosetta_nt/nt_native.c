/* nt_native.c — the probe's four NT-flag checks as a plain x86-64 macOS binary.
 *
 * No Wine, no bundle: this is the fastest loop for working on the Rosetta NT-flag
 * correction. Build and run:
 *   clang -arch x86_64 -O1 -o nt_native nt_native.c && ./nt_native
 * On a real x86-64 CPU every check passes. Under Rosetta today all four fail, which
 * is the divergence THE FINALS' environment check sees (tests/windows_probe/
 * macfinals_probe.c nt_flag(), where Windows and Proton both pass).
 *
 * The checks use signals where the Windows probe uses SEH: an int3 handler reads and
 * writes uc_mcontext->__ss.__rflags, which is the same x86-64 context Rosetta
 * synthesizes for Wine's signal handlers. */
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <sys/ucontext.h>

#define NT_FLAG 0x4000

static volatile int failures;
static volatile unsigned long trap_flags;   /* rflags seen by the SIGTRAP handler */
static volatile unsigned long fault_code;   /* signal that hit the iretq */
static void *volatile resume_at;
static volatile long resume_rsp_adjust;
static volatile int inject_nt;               /* handler sets NT on resume */

static void handler(int sig, siginfo_t *si, void *vctx) {
    ucontext_t *uc = vctx;
    if (sig == SIGTRAP) trap_flags = uc->uc_mcontext->__ss.__rflags;
    else fault_code = sig;
    if (inject_nt) uc->uc_mcontext->__ss.__rflags |= NT_FLAG;
    else uc->uc_mcontext->__ss.__rflags &= ~NT_FLAG; /* never resume with NT live */
    if (resume_at) {
        uc->uc_mcontext->__ss.__rip = (uint64_t)resume_at;
        uc->uc_mcontext->__ss.__rsp += resume_rsp_adjust;
    }
}

static void install_handlers(void) {
    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

static void check(int same, const char *what, const char *got, const char *real_hw) {
    printf("%-4s %-46s got %-18s real x86-64: %s\n", same ? "ok" : "DIFF", what, got, real_hw);
    if (!same) failures++;
}

extern char after_int3[], after_iret[], after_inject[];

int main(void) {
    unsigned long flags;
    char got[48];
    install_handlers();

    /* 1. POPF sets NT, PUSHF reads it back. */
    __asm__ volatile("pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\tpushfq\n\tpopq %0\n\t"
                     "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq"
                     : "=r"(flags) :: "memory", "cc");
    snprintf(got, sizeof got, "NT=%lu", (unsigned long)!!(flags & NT_FLAG));
    check(!!(flags & NT_FLAG), "POPF sets NT, PUSHF reads it back", got, "NT=1");

    /* 2. A trap raised with NT set: the delivered context must carry it. */
    trap_flags = 0; resume_at = after_int3; resume_rsp_adjust = 0;
    __asm__ volatile("pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\tint3\n\t"
                     ".globl _after_int3\n_after_int3:\n\t"
                     "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq" ::: "memory", "cc");
    snprintf(got, sizeof got, "sig rflags NT=%lu", (unsigned long)!!(trap_flags & NT_FLAG));
    check(!!(trap_flags & NT_FLAG), "signal context keeps NT", got, "NT=1");

    /* 3. IRETQ with NT set is a task return, which 64-bit mode refuses with #GP. */
    fault_code = 0; resume_at = after_iret; resume_rsp_adjust = 40;
    __asm__ volatile("pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\t"
                     "movq %%rsp,%%rax\n\tmovl %%ss,%%ecx\n\tpushq %%rcx\n\tpushq %%rax\n\tpushfq\n\t"
                     "movl %%cs,%%ecx\n\tpushq %%rcx\n\tleaq _after_iret(%%rip),%%rcx\n\tpushq %%rcx\n\tiretq\n\t"
                     ".globl _after_iret\n_after_iret:\n\t"
                     "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq" ::: "rax", "rcx", "memory", "cc");
    snprintf(got, sizeof got, "signal %lu", fault_code);
    check(fault_code == SIGSEGV, "IRETQ with NT set faults (#GP)", got, "SIGSEGV");

    /* 4. NT set in a context, then resumed: the flag must survive into the
     * running code. Wine's own iretq path carries this correctly on real hardware. */
    flags = 0; resume_at = after_inject; resume_rsp_adjust = 0; inject_nt = 1;
    __asm__ volatile("int3\n\t"
                     ".globl _after_inject\n_after_inject:\n\tpushfq\n\tpopq %0\n\t"
                     "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq" : "=r"(flags) :: "memory", "cc");
    inject_nt = 0;
    snprintf(got, sizeof got, "NT=%lu", (unsigned long)!!(flags & NT_FLAG));
    check(!!(flags & NT_FLAG), "NT injected via context survives resume", got, "NT=1");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
