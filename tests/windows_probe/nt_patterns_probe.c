/* Does POPF->PUSHF keep NT whatever the surrounding code looks like? A translator (or a hook
 * on one) can treat the flags differently depending on how the instructions are arranged, so
 * this tries many generic arrangements. On a real CPU every line says kept.
 *
 * Build: x86_64-w64-mingw32-clang -O1 -Wall nt_patterns_probe.c -o nt-patterns-probe.exe
 * Same .exe on MacFinals and Proton. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NT 0x4000ull
#define VALUE (0x202ull | NT)

static unsigned long long clear_nt(unsigned long long v) { return v & ~NT; }

/* each returns the flags PUSHF read while NT should be set; the caller's flags are restored */
#define PATTERN(name, text) \
static __attribute__((noinline)) unsigned long long name(void) { \
    unsigned long long got = 0, value = VALUE; \
    __asm__ volatile("pushfq\n\t" text "\n\tpopfq" : "=&r"(got) : "r"(value) : "cc", "memory", "rax", "rcx"); \
    return got; }

PATTERN(reg_then_pushf, "pushq %1\n\tpopfq\n\tpushfq\n\tpopq %0")
PATTERN(imm_mov, "movq %1,%%rax\n\tpushq %%rax\n\tpopfq\n\tpushfq\n\tpopq %%rax\n\tmovq %%rax,%0")
PATTERN(nop_between, "pushq %1\n\tpopfq\n\tnop\n\tnop\n\tpushfq\n\tpopq %0")
PATTERN(mov_between, "pushq %1\n\tpopfq\n\tmovq %%rsp,%%rax\n\tpushfq\n\tpopq %0")
PATTERN(lea_between, "pushq %1\n\tpopfq\n\tleaq 8(%%rsp),%%rax\n\tpushfq\n\tpopq %0")
PATTERN(arith_between, "pushq %1\n\tpopfq\n\taddq $1,%%rax\n\tpushfq\n\tpopq %0")
PATTERN(test_between, "pushq %1\n\tpopfq\n\ttestq %%rax,%%rax\n\tpushfq\n\tpopq %0")
PATTERN(jump_between, "pushq %1\n\tpopfq\n\tjmp 1f\n\tnop\n1:\n\tpushfq\n\tpopq %0")
PATTERN(cond_between, "pushq %1\n\tpopfq\n\tjnz 1f\n\tnop\n1:\n\tpushfq\n\tpopq %0")
PATTERN(call_between, "pushq %1\n\tpopfq\n\tcallq 1f\n\tjmp 2f\n1:\n\tret\n2:\n\tpushfq\n\tpopq %0")
PATTERN(mem_pop, "pushq %1\n\tpushq %1\n\tpopfq\n\tpopq %%rax\n\tpushfq\n\tpopq %0")
PATTERN(popf_twice, "pushq %1\n\tpushq %1\n\tpopfq\n\tpopfq\n\tpushfq\n\tpopq %0")
PATTERN(pushf_twice, "pushq %1\n\tpopfq\n\tpushfq\n\tpushfq\n\tpopq %0\n\tpopq %%rax")
PATTERN(pushf_peek, "pushq %1\n\tpopfq\n\tpushfq\n\tmovq (%%rsp),%0\n\taddq $8,%%rsp")
PATTERN(mem_or, "pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\tpushfq\n\tpopq %0")
PATTERN(bts_mem, "pushfq\n\tbtsq $14,(%%rsp)\n\tpopfq\n\tpushfq\n\tpopq %0")
PATTERN(lahf_between, "pushq %1\n\tpopfq\n\tlahf\n\tpushfq\n\tpopq %0")
PATTERN(std_between, "pushq %1\n\tpopfq\n\tstd\n\tcld\n\tpushfq\n\tpopq %0")

/* several groups in one block, as code that saves and restores flags repeatedly has */
static __attribute__((noinline)) unsigned long long two_groups_first(void) {
    unsigned long long got, other, value = VALUE;
    __asm__ volatile("pushfq\n\tpushq %2\n\tpopfq\n\tpushfq\n\tpopq %0\n\tpopfq\n\t"
                     "pushfq\n\tpushq $0x202\n\tpopfq\n\tpushfq\n\tpopq %1\n\tpopfq"
                     : "=&r"(got), "=&r"(other) : "r"(value) : "cc", "memory");
    return got;
}
static __attribute__((noinline)) unsigned long long two_groups_second(void) {
    unsigned long long got, other, value = VALUE;
    __asm__ volatile("pushfq\n\tpushq $0x202\n\tpopfq\n\tpushfq\n\tpopq %1\n\tpopfq\n\t"
                     "pushfq\n\tpushq %2\n\tpopfq\n\tpushfq\n\tpopq %0\n\tpopfq"
                     : "=&r"(got), "=&r"(other) : "r"(value) : "cc", "memory");
    return got;
}
static __attribute__((noinline)) unsigned long long same_group_twice(void) {
    unsigned long long got, other, value = VALUE;
    __asm__ volatile("pushfq\n\tpushq %2\n\tpopfq\n\tpushfq\n\tpopq %1\n\tpopfq\n\t"
                     "pushfq\n\tpushq %2\n\tpopfq\n\tpushfq\n\tpopq %0\n\tpopfq"
                     : "=&r"(got), "=&r"(other) : "r"(value) : "cc", "memory");
    return got;
}
/* the value arrives from a loop, one bit at a time, ending on NT */
static __attribute__((noinline)) unsigned long long loop_of_bits(void) {
    unsigned long long got = 0;
    for (int bit = 0; bit < 22; bit++) {
        unsigned long long want = 0x202ull | (1ull << bit), one;
        if (bit == 8 || bit == 9) continue;
        __asm__ volatile("pushfq\n\tpushq %1\n\tpopfq\n\tpushfq\n\tpopq %0\n\tpopfq" : "=&r"(one) : "r"(want) : "cc", "memory");
        if (bit == 14) got = one;
    }
    return got;
}
/* after IOPL bits, which precede NT in that loop */
static __attribute__((noinline)) unsigned long long after_iopl(void) {
    unsigned long long got, other, iopl = 0x202ull | 0x3000, value = VALUE;
    __asm__ volatile("pushfq\n\tpushq %2\n\tpopfq\n\tpushfq\n\tpopq %1\n\tpopfq\n\t"
                     "pushfq\n\tpushq %3\n\tpopfq\n\tpushfq\n\tpopq %0\n\tpopfq"
                     : "=&r"(got), "=&r"(other) : "r"(iopl), "r"(value) : "cc", "memory");
    return got;
}

/* a literal 0x202 in the code just before: the usual RFLAGS value, so common in flag handling */
#define LITERAL(name, text) \
static __attribute__((noinline)) unsigned long long name(void) { \
    unsigned long long got = 0; \
    __asm__ volatile("pushfq\n\t" text "\n\tpopfq" : "=&r"(got) : : "cc", "memory", "rax", "rcx"); \
    return got; }
LITERAL(literal_bts, "movl $0x202,%%eax\n\tmovl $14,%%ecx\n\tbtsq %%rcx,%%rax\n\tpushq %%rax\n\tpopfq\n\tpushfq\n\tpopq %0")
LITERAL(literal_or, "movl $0x202,%%eax\n\torl $0x4000,%%eax\n\tpushq %%rax\n\tpopfq\n\tpushfq\n\tpopq %0")
LITERAL(literal_unrelated, "movl $0x202,%%ecx\n\tmovl $0x4202,%%eax\n\tpushq %%rax\n\tpopfq\n\tpushfq\n\tpopq %0")
LITERAL(literal_after_popf, "movl $0x4202,%%eax\n\tpushq %%rax\n\tpopfq\n\tmovl $0x202,%%ecx\n\tpushfq\n\tpopq %0")
LITERAL(literal_far_before, "movl $0x202,%%ecx\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tmovl $0x4202,%%eax\n\tpushq %%rax\n\tpopfq\n\tpushfq\n\tpopq %0")
LITERAL(literal_jump_after, "movl $0x202,%%ecx\n\tjmp 1f\n1:\n\tmovl $0x4202,%%eax\n\tpushq %%rax\n\tpopfq\n\tpushfq\n\tpopq %0")
LITERAL(zero_before_popf, "movl $0x4202,%%eax\n\tmovl $0,%%ecx\n\tpushq %%rax\n\tpopfq\n\tpushfq\n\tpopq %0")
LITERAL(zero_before_pushf, "movl $0x4202,%%eax\n\tpushq %%rax\n\tpopfq\n\tmovl $0,%%ecx\n\tpushfq\n\tpopq %0")
LITERAL(zero_both, "movl $0,%%ecx\n\tmovl $0x4202,%%eax\n\tmovl $0,%%ecx\n\tpushq %%rax\n\tpopfq\n\tmovl $0,%%ecx\n\tpushfq\n\tpopq %0")
LITERAL(other_literal, "movl $0x206,%%eax\n\tmovl $14,%%ecx\n\tbtsq %%rcx,%%rax\n\tpushq %%rax\n\tpopfq\n\tpushfq\n\tpopq %0")

/* popf and pushf in separate functions, so in separate translated blocks */
static __attribute__((noinline)) void set_flags(unsigned long long v) { __asm__ volatile("pushq %0\n\tpopfq" : : "r"(v) : "cc"); }
static __attribute__((noinline)) unsigned long long get_flags(void) { unsigned long long v; __asm__ volatile("pushfq\n\tpopq %0" : "=r"(v)); return v; }
static unsigned long long separate_functions(void) {
    unsigned long long saved = get_flags(), got;
    set_flags(VALUE); got = get_flags(); set_flags(clear_nt(saved));
    return got;
}

/* the same bytes run from memory made executable at run time */
static unsigned long long dynamic_code(void) {
    /* pushfq; push rcx; popfq; pushfq; pop rax; popfq; ret */
    static const unsigned char code[] = {0x9c, 0x51, 0x9d, 0x9c, 0x58, 0x9d, 0xc3};
    DWORD old;
    void *p = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memcpy(p, code, sizeof code);
    VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old);
    return ((unsigned long long (*)(unsigned long long))p)(VALUE);
}

/* the third execution of a block can differ from the first, once a translator has seen it */
static unsigned long long repeated(void) {
    unsigned long long got = 0;
    for (int i = 0; i < 2000; i++) got = reg_then_pushf();
    return got;
}

static DWORD WINAPI on_thread(void *arg) { *(unsigned long long *)arg = reg_then_pushf(); return 0; }
static unsigned long long second_thread(void) {
    unsigned long long got = 0;
    HANDLE t = CreateThread(NULL, 0, on_thread, &got, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    return got;
}

static const struct { const char *name; unsigned long long (*fn)(void); } patterns[] = {
    {"push reg, popf, pushf, pop reg", reg_then_pushf}, {"through rax", imm_mov}, {"nops between", nop_between},
    {"mov between", mov_between}, {"lea between", lea_between}, {"add between", arith_between},
    {"test between", test_between}, {"jmp between", jump_between}, {"jcc between", cond_between},
    {"call between", call_between}, {"pop between", mem_pop}, {"popf twice", popf_twice},
    {"pushf twice", pushf_twice}, {"pushf, read from stack", pushf_peek}, {"or NT into saved flags", mem_or},
    {"bts NT into saved flags", bts_mem}, {"lahf between", lahf_between}, {"std/cld between", std_between},
    {"separate functions", separate_functions}, {"dynamic code", dynamic_code}, {"2000 times", repeated},
    {"second thread", second_thread}, {"two groups, first", two_groups_first}, {"two groups, second", two_groups_second},
    {"same group twice", same_group_twice}, {"loop of bits", loop_of_bits}, {"after IOPL", after_iopl},
    {"literal 0x202, bts NT", literal_bts}, {"literal 0x202, or NT", literal_or}, {"literal 0x202 unrelated", literal_unrelated},
    {"literal 0x202 after popf", literal_after_popf}, {"literal 0x202 further back", literal_far_before},
    {"literal 0x202, then jmp", literal_jump_after}, {"literal 0x206, bts NT", other_literal},
    {"mov 0 before popf", zero_before_popf}, {"mov 0 before pushf", zero_before_pushf}, {"mov 0 around both", zero_both}};

int main(void) {
    int lost = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned i = 0; i < ARRAYSIZE(patterns); i++) {
        unsigned long long got = patterns[i].fn();
        printf("%-34s %s (flags %06llx)\n", patterns[i].name, got & NT ? "kept" : "LOST", got);
        if (!(got & NT)) lost++;
    }
    printf("== %d of %u lost\n", lost, (unsigned)ARRAYSIZE(patterns));
    return lost != 0;
}
