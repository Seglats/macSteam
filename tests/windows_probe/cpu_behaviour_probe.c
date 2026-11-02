/* What a CPU translator is likely to get wrong, for a line-by-line diff between Rosetta and a
 * real CPU. Generic x86-64 behaviour from the architecture manuals: flags, faults, the trap
 * flag, debug registers, x87/SSE state, segment and timing instructions. Each exception case
 * runs in its own process, so one that kills the process cannot hide the others. Real CPUs
 * differ per vendor in undefined flags, so note the vendor when comparing.
 *
 * Build: x86_64-w64-mingw32-clang -O1 -Wall cpu_behaviour_probe.c -o cpu-behaviour-probe.exe
 * Run the same .exe on MacFinals and Proton and diff the output. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLAG_MASK 0x8d5u  /* OF SF ZF AF PF CF */

/* ---- flags ---- */
#define SHIFT32(name, insn) \
static unsigned name(unsigned value, unsigned char count, unsigned in, unsigned *result) { \
    unsigned long long flags; unsigned out = value; \
    __asm__ volatile("pushq %q2\n\tpopfq\n\t" insn " %%cl,%0\n\tpushfq\n\tpopq %1" \
                     : "+r"(out), "=&r"(flags) : "r"((unsigned long long)in), "c"(count) : "cc"); \
    *result = out; return (unsigned)flags & FLAG_MASK; }
#define SHIFT8(name, insn) \
static unsigned name(unsigned value, unsigned char count, unsigned in, unsigned *result) { \
    unsigned long long flags; unsigned char out = (unsigned char)value; \
    __asm__ volatile("pushq %q2\n\tpopfq\n\t" insn " %%cl,%0\n\tpushfq\n\tpopq %1" \
                     : "+q"(out), "=&r"(flags) : "r"((unsigned long long)in), "c"(count) : "cc"); \
    *result = out; return (unsigned)flags & FLAG_MASK; }
SHIFT32(shl32, "shll") SHIFT32(shr32, "shrl") SHIFT32(sar32, "sarl") SHIFT32(rol32, "roll")
SHIFT32(ror32, "rorl") SHIFT32(rcl32, "rcll") SHIFT32(rcr32, "rcrl")
SHIFT8(shl8, "shlb") SHIFT8(shr8, "shrb") SHIFT8(sar8, "sarb") SHIFT8(rol8, "rolb")
SHIFT8(ror8, "rorb") SHIFT8(rcl8, "rclb") SHIFT8(rcr8, "rcrb")

typedef unsigned (*shift_fn)(unsigned, unsigned char, unsigned, unsigned *);
static void flags_shifts(void) {
    static const struct { const char *name; shift_fn fn; int width; } ops[] = {
        {"shl32", shl32, 32}, {"shr32", shr32, 32}, {"sar32", sar32, 32}, {"rol32", rol32, 32},
        {"ror32", ror32, 32}, {"rcl32", rcl32, 32}, {"rcr32", rcr32, 32}, {"shl8", shl8, 8},
        {"shr8", shr8, 8}, {"sar8", sar8, 8}, {"rol8", rol8, 8}, {"ror8", ror8, 8},
        {"rcl8", rcl8, 8}, {"rcr8", rcr8, 8}};
    static const unsigned char counts[] = {0, 1, 2, 7, 8, 9, 31, 32, 33};
    static const unsigned values[] = {0x81, 0x80000001, 0};
    for (unsigned o = 0; o < ARRAYSIZE(ops); o++) for (unsigned v = 0; v < ARRAYSIZE(values); v++)
    for (unsigned c = 0; c < ARRAYSIZE(counts); c++) for (unsigned in = 0; in < 2; in++) {
        unsigned result, flags = ops[o].fn(values[v], counts[c], in ? 0x8d7 : 0x202, &result);
        printf("%-6s %08x cnt %2u in %s -> %08x flags %03x\n", ops[o].name, values[v], counts[c],
               in ? "all" : "0  ", result, flags);
    }
}

#define BINARY(name, body) static unsigned name(unsigned long long a, unsigned long long b, unsigned long long *r) { \
    unsigned long long flags, x = a, y = b; \
    __asm__ volatile("pushq $0x202\n\tpopfq\n\t" body "\n\tpushfq\n\tpopq %2" : "+a"(x), "+d"(y), "=&r"(flags) : : "cc"); \
    *r = x; return (unsigned)flags & FLAG_MASK; }
BINARY(mul_op, "mulq %%rdx") BINARY(imul_op, "imulq %%rdx,%%rax") BINARY(bsf_op, "bsfq %%rdx,%%rax")
BINARY(bsr_op, "bsrq %%rdx,%%rax") BINARY(bt_op, "btq %%rdx,%%rax") BINARY(bts_op, "btsq %%rdx,%%rax")
BINARY(add_op, "addq %%rdx,%%rax") BINARY(sub_op, "subq %%rdx,%%rax") BINARY(adc_op, "adcq %%rdx,%%rax")
BINARY(and_op, "andq %%rdx,%%rax") BINARY(xor_op, "xorq %%rdx,%%rax")
BINARY(inc_op, "incq %%rax") BINARY(neg_op, "negq %%rax") BINARY(shld_op, "shldq $4,%%rdx,%%rax")
BINARY(cmpb_op, "cmpb %%dl,%%al")

static void flags_binary(void) {
    typedef unsigned (*fn)(unsigned long long, unsigned long long, unsigned long long *);
    static const struct { const char *name; fn f; } ops[] = {
        {"mul", mul_op}, {"imul", imul_op}, {"bsf", bsf_op}, {"bsr", bsr_op}, {"bt", bt_op}, {"bts", bts_op},
        {"add", add_op}, {"sub", sub_op}, {"adc", adc_op}, {"and", and_op}, {"xor", xor_op}, {"inc", inc_op},
        {"neg", neg_op}, {"shld", shld_op}, {"cmpb", cmpb_op}};
    static const unsigned long long values[][2] = {
        {0, 0}, {1, 1}, {0xf, 1}, {0x7fffffffffffffffull, 1}, {0x8000000000000000ull, 2},
        {0xffffffffffffffffull, 1}, {0x123456789abcdef0ull, 0}, {0x123456789abcdef0ull, 0x47}, {0x10, 0x8}};
    for (unsigned o = 0; o < ARRAYSIZE(ops); o++) for (unsigned v = 0; v < ARRAYSIZE(values); v++) {
        unsigned long long r;
        unsigned flags = ops[o].f(values[v][0], values[v][1], &r);
        printf("%-5s %016llx %016llx -> %016llx flags %03x\n", ops[o].name, values[v][0], values[v][1], r, flags);
    }
}

/* which RFLAGS bits POPF sets and PUSHF reads back; TF (8) has its own test */
static void flags_popf(void) {
    for (int bit = 0; bit < 32; bit++) {
        unsigned long long want = 0x202ull | (1ull << bit), got, cleared;
        if (bit == 8 || bit == 9) continue;
        __asm__ volatile("pushfq\n\tpushq %1\n\tpopfq\n\tpushfq\n\tpopq %0\n\tpopfq" : "=r"(got) : "r"(want) : "cc", "memory");
        __asm__ volatile("pushfq\n\tpushq $0x202\n\tpopfq\n\tpushfq\n\tpopq %0\n\tpopfq" : "=r"(cleared) : : "cc", "memory");
        printf("popf bit %2d: %s, then cleared %016llx\n", bit, got & (1ull << bit) ? "kept" : "not kept", cleared);
    }
    {
        unsigned long long ax = 0xd700, flags, back;
        __asm__ volatile("pushq $0x202\n\tpopfq\n\tsahf\n\tpushfq\n\tpopq %1\n\tlahf" : "+a"(ax), "=&r"(flags) : : "cc");
        back = ax;
        printf("sahf d7 -> flags %03llx, lahf ah=%02llx\n", flags & 0xfff, (back >> 8) & 0xff);
    }
}

/* ---- exceptions: the bytes run from executable memory, one process per case ---- */
static const struct { const char *name; unsigned char len; unsigned char code[16]; } cases[] = {
    {"int3", 1, {0xcc}}, {"int 3 (2 bytes)", 2, {0xcd, 0x03}}, {"int1/icebp", 1, {0xf1}},
    {"int 0x2d", 2, {0xcd, 0x2d}}, {"int 0x2c", 2, {0xcd, 0x2c}}, {"int 0x29", 2, {0xcd, 0x29}},
    {"int 0x80", 2, {0xcd, 0x80}}, {"int 0x00", 2, {0xcd, 0x00}}, {"int 0x0e", 2, {0xcd, 0x0e}},
    {"ud2", 2, {0x0f, 0x0b}}, {"hlt", 1, {0xf4}}, {"cli", 1, {0xfa}}, {"sti", 1, {0xfb}},
    {"in al,dx", 1, {0xec}}, {"out dx,al", 1, {0xee}}, {"in al,0x60", 2, {0xe4, 0x60}},
    {"insb", 1, {0x6c}}, {"mov rax,cr0", 3, {0x0f, 0x20, 0xc0}}, {"mov cr0,rax", 3, {0x0f, 0x22, 0xc0}},
    {"mov rax,cr3", 3, {0x0f, 0x20, 0xd8}}, {"mov rax,dr7", 3, {0x0f, 0x21, 0xf8}},
    {"rdmsr", 2, {0x0f, 0x32}}, {"wrmsr", 2, {0x0f, 0x30}}, {"rdpmc", 2, {0x0f, 0x33}}, {"invd", 2, {0x0f, 0x08}},
    {"wbinvd", 2, {0x0f, 0x09}}, {"clts", 2, {0x0f, 0x06}}, {"lgdt", 3, {0x0f, 0x01, 0x11}}, {"lidt", 3, {0x0f, 0x01, 0x19}},
    {"ltr", 3, {0x0f, 0x00, 0xd8}}, {"lmsw", 3, {0x0f, 0x01, 0xf0}}, {"swapgs", 3, {0x0f, 0x01, 0xf8}},
    {"sysret", 2, {0x0f, 0x07}}, {"sysexit", 2, {0x0f, 0x35}}, {"nop", 1, {0x90}},
    {"divide by zero", 5, {0x31, 0xc9, 0x31, 0xd2, 0xf7}},
    {"read non-canonical", 13, {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0x80, 0x48, 0x8b, 0x00}},
    {"read address 0", 5, {0x31, 0xc0, 0x48, 0x8b, 0x00}}, {"write address 0", 5, {0x31, 0xc0, 0x48, 0x89, 0x00}},
    {"lock nop (#UD)", 2, {0xf0, 0x90}}, {"lock add reg (#UD)", 3, {0xf0, 0x01, 0xc0}},
    {"15 prefixes", 16, {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90}},
    {"16 bytes long (#GP)", 16, {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90}},
    {"invalid in 64-bit: into", 1, {0xce}}, {"invalid in 64-bit: aaa", 1, {0x37}}, {"invalid in 64-bit: pusha", 1, {0x60}},
    {"invalid in 64-bit: push es", 1, {0x06}}, {"invalid in 64-bit: les", 2, {0xc4, 0x00}},
    {"mov ds,invalid selector", 5, {0xb8, 0xff, 0x7f, 0, 0}}, {"far ret to bad cs", 1, {0xcb}},
    {"mov ss,0", 4, {0x31, 0xc0, 0x8e, 0xd0}}, {"sgdt", 5, {0x48, 0x8d, 0x01, 0x0f, 0x01}},
    {"movaps misaligned", 8, {0x48, 0x8d, 0x41, 0x01, 0x0f, 0x28, 0x00, 0x90}},
    {"vzeroupper", 3, {0xc5, 0xf8, 0x77}}, {"xgetbv", 5, {0x31, 0xc9, 0x0f, 0x01, 0xd0}},
    {"rdtscp", 3, {0x0f, 0x01, 0xf9}}, {"rdpid", 4, {0xf3, 0x0f, 0xc7, 0xf8}}, {"rdrand", 4, {0x48, 0x0f, 0xc7, 0xf0}},
    {"rdfsbase", 5, {0xf3, 0x48, 0x0f, 0xae, 0xc0}}, {"rdgsbase", 5, {0xf3, 0x48, 0x0f, 0xae, 0xc8}},
    {"lar eax,cs", 5, {0x8c, 0xc8, 0x0f, 0x02, 0xc0}}, {"lsl eax,cs", 5, {0x8c, 0xc8, 0x0f, 0x03, 0xc0}},
    {"verr cs", 5, {0x8c, 0xc8, 0x0f, 0x00, 0xe0}}, {"verw cs", 5, {0x8c, 0xc8, 0x0f, 0x00, 0xe8}},
    {"str eax", 3, {0x0f, 0x00, 0xc8}}, {"sldt eax", 3, {0x0f, 0x00, 0xc0}}, {"smsw eax", 3, {0x0f, 0x01, 0xe0}},
    {"sidt", 3, {0x0f, 0x01, 0x09}},
    {"monitor", 3, {0x0f, 0x01, 0xc8}}, {"ldmxcsr reserved bit", 3, {0x0f, 0xae, 0x11}}, {"xbegin", 6, {0xc7, 0xf8, 0, 0, 0, 0}},
};

static unsigned char *exec_page;
static void *resume_at;
static LONG CALLBACK case_handler(EXCEPTION_POINTERS *ep) {
    static int faults;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    long long at = (char *)r->ExceptionAddress - (char *)exec_page, rip = (char *)ep->ContextRecord->Rip - (char *)exec_page;
    printf(" code %08lx at %+lld rip %+lld flags %lx params %lu", r->ExceptionCode, at, rip, r->ExceptionFlags, r->NumberParameters);
    for (DWORD i = 0; i < r->NumberParameters && i < 3; i++) {
        /* an address parameter that is outside the case means little; keep small values */
        ULONG_PTR v = r->ExceptionInformation[i];
        if (v < 0x100 || v == ~(ULONG_PTR)0) printf(" %llx", (unsigned long long)v); else printf(" big");
    }
    printf(" eflags %06lx cs %02x ss %02x\n", ep->ContextRecord->EFlags & 0x3f0fd5, ep->ContextRecord->SegCs, ep->ContextRecord->SegSs);
    if (++faults >= 4) { printf(" [faults repeat]\n"); ExitProcess(3); }
    ep->ContextRecord->Rip = (ULONG_PTR)resume_at;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int run_case(int index) {
    static unsigned char scratch[64] __attribute__((aligned(16)));
    DWORD old;
    unsigned char *p = exec_page = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned len = cases[index].len;
    if (!p) return 2;
    /* rcx points at scratch data for the cases that use it */
    memcpy(p, cases[index].code, len);
    if (!strcmp(cases[index].name, "divide by zero")) { static const unsigned char c[] = {0x31, 0xc9, 0x31, 0xd2, 0xf7, 0xf1}; memcpy(p, c, sizeof c); len = sizeof c; }
    if (!strcmp(cases[index].name, "mov ds,invalid selector")) { static const unsigned char c[] = {0xb8, 0xff, 0x7f, 0, 0, 0x8e, 0xd8}; memcpy(p, c, sizeof c); len = sizeof c; }
    if (!strcmp(cases[index].name, "ldmxcsr reserved bit")) ((unsigned *)scratch)[0] = 0xffff1f80u;
    if (!strcmp(cases[index].name, "sgdt")) { static const unsigned char c[] = {0x0f, 0x01, 0x01}; memcpy(p, c, sizeof c); len = sizeof c; }
    if (!strcmp(cases[index].name, "far ret to bad cs")) { static const unsigned char c[] = {0x6a, 0x00, 0x6a, 0x00, 0x48, 0xcb}; memcpy(p, c, sizeof c); len = sizeof c; }
    /* lea rsp,[rbx-0x48]; ret: rbx is rsp before the 64-byte gap and the call, so a case that
     * changed the stack still returns */
    p[len] = 0x48; p[len + 1] = 0x8d; p[len + 2] = 0x63; p[len + 3] = 0xb8;
    p[len + 4] = 0xc3;
    resume_at = p + len;
    VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 0x1000);
    AddVectoredExceptionHandler(1, case_handler);
    printf("%-26s", cases[index].name);
    {
        register void *data __asm__("rcx") = scratch;
        register void *target __asm__("rax") = p;
        __asm__ volatile("pushq %%rbx\n\tmovq %%rsp,%%rbx\n\tsubq $64,%%rsp\n\tcallq *%1\n\tmovq %%rbx,%%rsp\n\tpopq %%rbx"
                         : "+r"(data), "+r"(target) :
                         : "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");
    }
    printf(" [end]\n");
    return 0;
}

static void all_cases(const char *self) {
    for (unsigned i = 0; i < ARRAYSIZE(cases); i++) {
        char command[MAX_PATH + 32];
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        DWORD code = 0;
        snprintf(command, sizeof command, "\"%s\" case %u", self, i);
        if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { printf("%s: could not start\n", cases[i].name); continue; }
        if (WaitForSingleObject(pi.hProcess, 8000) == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 99); printf("  case %s TIMED OUT\n", cases[i].name); }
        else { GetExitCodeProcess(pi.hProcess, &code); if (code) printf("  case %s exit code %08lx\n", cases[i].name, code); }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* ---- trap flag ---- */
static int steps;
static DWORD64 step_dr6;
static long long step_at[8];
static unsigned char *trap_code;
static LONG CALLBACK trap_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        printf("  other exception %08lx at %+lld\n", ep->ExceptionRecord->ExceptionCode,
               (char *)ep->ExceptionRecord->ExceptionAddress - (char *)trap_code);
        ep->ContextRecord->EFlags &= ~0x100;
        ep->ContextRecord->Rip++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (steps < 8) step_at[steps] = (char *)ep->ExceptionRecord->ExceptionAddress - (char *)trap_code;
    steps++;
    step_dr6 = ep->ContextRecord->Dr6;
    if (steps >= 8) ep->ContextRecord->EFlags &= ~0x100;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void trap_run(const char *name, const unsigned char *body, unsigned len) {
    /* pushf; or [rsp],0x100; popf; <body>; pushf; and [rsp],~0x100; popf; ret */
    static const unsigned char head[] = {0x9c, 0x48, 0x81, 0x0c, 0x24, 0x00, 0x01, 0, 0, 0x9d};
    static const unsigned char tail[] = {0x9c, 0x48, 0x81, 0x24, 0x24, 0xff, 0xfe, 0xff, 0xff, 0x9d, 0xc3};
    DWORD old;
    VirtualProtect(trap_code, 0x1000, PAGE_EXECUTE_READWRITE, &old);
    memcpy(trap_code, head, sizeof head);
    memcpy(trap_code + sizeof head, body, len);
    memcpy(trap_code + sizeof head + len, tail, sizeof tail);
    VirtualProtect(trap_code, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), trap_code, 0x1000);
    steps = 0; step_dr6 = 0;
    ((void (*)(void))trap_code)();
    printf("trap %-20s steps %d dr6 %llx at", name, steps, (unsigned long long)step_dr6 & 0xf00f);
    for (int i = 0; i < steps && i < 8; i++) printf(" %lld", step_at[i]);
    printf("\n");
}
static void trap_flag(void) {
    static const unsigned char nops[] = {0x90, 0x90, 0x90}, pushpop[] = {0x50, 0x58, 0x90}, rep[] = {0x31, 0xc9, 0xb1, 0x03, 0x48, 0x8d, 0x74, 0x24, 0xe0, 0x48, 0x8d, 0x7c, 0x24, 0xe0, 0xf3, 0xa4},
        jump[] = {0xeb, 0x01, 0xcc, 0x90}, popss[] = {0x8c, 0xd0, 0x8e, 0xd0, 0x90}, pushf_read[] = {0x9c, 0x58, 0x90}, cpuid[] = {0x53, 0x31, 0xc0, 0x0f, 0xa2, 0x5b};
    trap_code = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    PVOID h = AddVectoredExceptionHandler(1, trap_handler);
    trap_run("nops", nops, sizeof nops);
    trap_run("push/pop", pushpop, sizeof pushpop);
    trap_run("rep movsb", rep, sizeof rep);
    trap_run("jump", jump, sizeof jump);
    trap_run("mov ss delays trap", popss, sizeof popss);
    trap_run("pushf sees TF", pushf_read, sizeof pushf_read);
    trap_run("cpuid", cpuid, sizeof cpuid);
    RemoveVectoredExceptionHandler(h);
}

/* ---- debug registers ---- */
static int hw_hits;
static DWORD64 hw_dr6;
static LONG CALLBACK hw_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    hw_hits++;
    hw_dr6 = ep->ContextRecord->Dr6;
    ep->ContextRecord->EFlags |= 0x10000; /* RF */
    if (hw_hits > 3) ep->ContextRecord->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static volatile int hw_data;
static HANDLE hw_go;
static __attribute__((noinline)) void hw_target(void) { __asm__ volatile("nop"); }
static DWORD WINAPI hw_thread(void *arg) {
    WaitForSingleObject(hw_go, INFINITE);
    hw_target();
    hw_data = 1;
    (void)hw_data;
    return 0;
}
static void debug_registers(void) {
    HANDLE t;
    CONTEXT c;
    PVOID h = AddVectoredExceptionHandler(1, hw_handler);
    hw_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    t = CreateThread(NULL, 0, hw_thread, NULL, 0, NULL);
    Sleep(50);
    SuspendThread(t);
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr0 = (DWORD64)hw_target; c.Dr1 = (DWORD64)&hw_data;
    c.Dr7 = 0x1 | 0x4 | (1u << 20 /* R/W1 write */) | (3u << 22 /* LEN1 4 bytes */);
    printf("SetThreadContext debug regs %d\n", SetThreadContext(t, &c));
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    printf("GetThreadContext %d: dr0 %s dr1 %s dr7 %llx\n", GetThreadContext(t, &c), c.Dr0 == (DWORD64)hw_target ? "same" : "DIFF",
           c.Dr1 == (DWORD64)&hw_data ? "same" : "DIFF", (unsigned long long)c.Dr7 & 0xffff00ff);
    ResumeThread(t);
    SetEvent(hw_go);
    WaitForSingleObject(t, 3000);
    printf("hardware breakpoints fired %d times, dr6 %llx\n", hw_hits, (unsigned long long)hw_dr6 & 0xf);
    RemoveVectoredExceptionHandler(h);
}

/* ---- x87 / SSE ---- */
static void fpu_state(void) {
    unsigned short cw, sw;
    unsigned mx;
    static unsigned char area[512] __attribute__((aligned(16)));
    __asm__ volatile("fnstcw %0\n\tfnstsw %1" : "=m"(cw), "=m"(sw));
    __asm__ volatile("stmxcsr %0" : "=m"(mx));
    printf("x87 cw %04x sw %04x mxcsr %08x\n", cw, sw, mx);
    __asm__ volatile("fninit\n\tfnstcw %0\n\tfnstsw %1" : "=m"(cw), "=m"(sw));
    printf("after fninit cw %04x sw %04x\n", cw, sw);
    __asm__ volatile("fxsave64 %0" : "=m"(area));
    printf("fxsave fcw %02x%02x fsw %02x%02x ftw %02x mxcsr_mask %02x%02x%02x%02x\n", area[1], area[0], area[3], area[2], area[4], area[31], area[30], area[29], area[28]);
    {
        /* 1/3 at each precision control; a real x87 computes these at 24, 53 and 64 bits */
        static const unsigned short pc[] = {0x007f, 0x027f, 0x037f};
        static const char *names[] = {"24-bit", "53-bit", "64-bit"};
        for (int i = 0; i < 3; i++) {
            unsigned char out[10];
            __asm__ volatile("fldcw %1\n\tfld1\n\tfldz\n\tfld1\n\tfadd %%st(1),%%st\n\tfadd %%st(0),%%st\n\tfadd %%st(2),%%st\n\tfdivrp\n\tfstpt %0\n\tfstp %%st(0)\n\tfninit"
                             : "=m"(out) : "m"(pc[i]));
            printf("x87 1/3 %s:", names[i]);
            for (int b = 9; b >= 0; b--) printf(" %02x", out[b]);
            printf("\n");
        }
    }
    {
        /* denormal handling: DAZ, FTZ and the flags they leave */
        static const unsigned modes[] = {0x1f80, 0x1f80 | 0x8000, 0x1f80 | 0x40, 0x1f80 | 0x8040};
        for (int i = 0; i < 4; i++) {
            unsigned old, after, mode = modes[i];
            double small = 2.2250738585072014e-308, half = 0.5, tiny = 1e-310, r1, r2;
            __asm__ volatile("stmxcsr %0\n\tldmxcsr %1" : "=m"(old) : "m"(mode));
            __asm__ volatile("movsd %1,%%xmm0\n\tmulsd %2,%%xmm0\n\tmovsd %%xmm0,%0" : "=m"(r1) : "m"(small), "m"(half) : "xmm0");
            __asm__ volatile("movsd %1,%%xmm0\n\taddsd %%xmm0,%%xmm0\n\tmovsd %%xmm0,%0" : "=m"(r2) : "m"(tiny) : "xmm0");
            __asm__ volatile("stmxcsr %0\n\tldmxcsr %1" : "=m"(after) : "m"(old));
            printf("mxcsr %04x: small*0.5 %016llx tiny+tiny %016llx flags %02x\n", mode, *(unsigned long long *)&r1, *(unsigned long long *)&r2, after & 0x3f);
        }
    }
}

/* ---- segments and descriptors ---- */
static void segments(void) {
    unsigned short cs, ss, ds, es, fs, gs;
    unsigned char gdt[10] = {0}, idt[10] = {0};
    __asm__ volatile("mov %%cs,%0\n\tmov %%ss,%1\n\tmov %%ds,%2\n\tmov %%es,%3\n\tmov %%fs,%4\n\tmov %%gs,%5" : "=r"(cs), "=r"(ss), "=r"(ds), "=r"(es), "=r"(fs), "=r"(gs));
    printf("selectors cs %02x ss %02x ds %02x es %02x fs %02x gs %02x\n", cs, ss, ds, es, fs, gs);
    (void)gdt; (void)idt;
}

/* ---- timing ---- */
static volatile unsigned long long shared_tsc;
static volatile LONG tsc_backwards, tsc_stop;
static DWORD WINAPI tsc_thread(void *arg) {
    while (!tsc_stop) {
        unsigned long long before = shared_tsc, now = __rdtsc();
        if (now < before) InterlockedIncrement(&tsc_backwards);
        shared_tsc = now;
    }
    return 0;
}
static void timing(void) {
    unsigned long long a, b, min = ~0ull, total = 0;
    LARGE_INTEGER f, q1, q2;
    HANDLE t[2];
    for (int i = 0; i < 2000; i++) {
        a = __rdtsc(); b = __rdtsc();
        if (b - a < min) min = b - a;
        total += b - a;
    }
    printf("rdtsc delta: %s, zero-delta %s\n", min < 2000 ? "small" : "large", min == 0 ? "yes" : "no");
    fprintf(stderr, "timing rdtsc min %llu avg %llu\n", min, total / 2000);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q1); a = __rdtsc(); Sleep(200); b = __rdtsc(); QueryPerformanceCounter(&q2);
    fprintf(stderr, "timing tsc rate %.0f MHz, qpc %lld Hz\n", (b - a) / ((q2.QuadPart - q1.QuadPart) / (double)f.QuadPart) / 1e6, f.QuadPart);
    t[0] = CreateThread(NULL, 0, tsc_thread, NULL, 0, NULL);
    t[1] = CreateThread(NULL, 0, tsc_thread, NULL, 0, NULL);
    Sleep(300);
    tsc_stop = 1;
    WaitForMultipleObjects(2, t, TRUE, 2000);
    fprintf(stderr, "timing tsc went backwards across threads %ld times (racy, indicative only)\n", tsc_backwards);
}

/* ---- CPUID, for the record: identity differences are not to be fixed ---- */
static void cpuid_leaves(void) {
    int r[4], max, maxext;
    __cpuid(r, 0); max = r[0];
    for (int leaf = 0; leaf <= max && leaf < 0x20; leaf++) {
        __cpuidex(r, leaf, 0);
        printf("cpuid %08x: %08x %08x %08x %08x\n", leaf, r[0], r[1], r[2], r[3]);
    }
    __cpuid(r, 0x80000000); maxext = r[0];
    for (unsigned leaf = 0x80000000; leaf <= (unsigned)maxext && leaf < 0x80000020; leaf++) {
        __cpuid(r, leaf);
        printf("cpuid %08x: %08x %08x %08x %08x\n", leaf, r[0], r[1], r[2], r[3]);
    }
}

int main(int argc, char **argv) {
    char self[MAX_PATH];
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2 && !strcmp(argv[1], "case")) return run_case(atoi(argv[2]));
    if (argc > 1 && !strcmp(argv[1], "popf")) { flags_popf(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "binary-popf")) { flags_binary(); flags_popf(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "shifts-popf")) { flags_shifts(); flags_popf(); return 0; }
    GetModuleFileNameA(NULL, self, MAX_PATH);
    printf("== flags\n"); flags_shifts(); flags_binary(); flags_popf();
    printf("== exceptions\n"); all_cases(self);
    printf("== trap flag\n"); trap_flag();
    printf("== debug registers\n"); debug_registers();
    printf("== x87 and SSE\n"); fpu_state();
    printf("== segments\n"); segments();
    printf("== timing\n"); timing();
    printf("== cpuid\n"); cpuid_leaves();
    return 0;
}
