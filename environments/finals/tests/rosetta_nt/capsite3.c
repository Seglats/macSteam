/* capsite3.c — insert-land forensics with exception survival (r21).
 *
 * capsite2 showed the r19 insert corrupts the placed TB nondeterministically:
 * sometimes a benign clobber (site1 ran clean), sometimes an undefined word
 * (EXC_BAD_INSTRUCTION -> Rosetta's exception server asserts, exit 132).
 * exctest (2026-11-07) proved synchronous faults EXCEPT brk#5 DO arrive at
 * thread-level Mach exception ports under Rosetta. So this carrier installs
 * an EXC_MASK_BAD_INSTRUCTION thread port (brk#5 is EXC_BREAKPOINT — the
 * stock syscall path is untouched), and on each fault: log pc, the faulting
 * word and the 10 words before it (guarded loads), then reply pc += 4 and
 * continue. After the site runs, scan the readable JIT arena for the insert
 * chain and dump the physical TB. Cap 64 redirects; SIGALRM belt-and-braces.
 *
 * Mach port RPC only — no vm read/write/protect anywhere.
 *
 *   clang -arch x86_64 -O1 -Wall -Wno-pointer-sign -o capsite3 capsite3.c
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>
#include <setjmp.h>
#include <pthread.h>
#include <sys/mman.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/exc.h>

/* arm thread state view (x86_64 SDK has no arm headers) — exctest-proven */
#define ARM_THREAD_STATE64       6
#define ARM_THREAD_STATE64_COUNT 104
typedef struct {
    uint64_t x[29];
    uint64_t fp, lr, sp, pc;
    uint32_t cpsr, pad;
} arm_st64_view_t;

typedef struct {
    mach_msg_header_t  Head;
    mach_msg_body_t    body;
    mach_msg_port_descriptor_t task;
    mach_msg_port_descriptor_t thread;
    NDR_record_t       NDR;
    exception_type_t   exception;
    mach_msg_type_number_t codeCnt;
    int64_t            code[2];
    int                flavor;
    mach_msg_type_number_t old_stateCnt;
    natural_t          old_state[ARM_THREAD_STATE64_COUNT];
    mach_msg_trailer_t trailer;
} exc_msg_t;
typedef struct {
    mach_msg_header_t  Head;
    NDR_record_t       NDR;
    kern_return_t      RetCode;
    int                flavor;
    mach_msg_type_number_t new_stateCnt;
    natural_t          new_state[ARM_THREAD_STATE64_COUNT];
} exc_reply_t;

static int g_fd = -1;
static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); fflush(stdout);
}
static void sigsys_noop(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; }

static mach_port_t g_port = MACH_PORT_NULL;
static volatile int g_faults = 0;

/* guarded reader */
static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_armed = 0;
static void recover(int sig, siginfo_t *si, void *uc) {
    if (g_armed) siglongjmp(g_jmp, 1);
    /* v3/v4 died _exit(77) with no clue WHERE: name the fault before dying.
     * x86_64 process under Rosetta -> x86_64 ucontext. */
    ucontext_t *c = uc;
    dprintf(g_fd, "FATAL-77 sig %d addr %p rip %llx (armed %d)\n",
            sig, si ? si->si_addr : NULL,
            (unsigned long long)c->uc_mcontext->__ss.__rip, (int)g_armed);
    _exit(77);
}
static int safe_read32(const uint32_t *p, uint32_t *out) {
    g_armed = 1;
    if (sigsetjmp(g_jmp, 1)) { g_armed = 0; return 0; }
    *out = *(volatile const uint32_t *)p;
    g_armed = 0;
    return 1;
}

#define SITE(n, MA, MB)                                                   \
    static void site##n(unsigned long *sink) {                            \
        __asm__ volatile(                                                 \
            "movabs $" #MA ", %%r8\n\t"                                   \
            "movq   %%r8, (%%rdi)\n\t"                                    \
            "movl   $0xffffffff, %%eax\n\t"                               \
            "syscall\n\t"                                                 \
            "movabs $" #MB ", %%r8\n\t"                                   \
            "movq   %%r8, 8(%%rdi)\n\t"                                   \
            : : "D"(sink) : "r8", "rax", "rcx", "r11", "memory");         \
    }
SITE(1, 0x1122334455667788, 0x8877665544332211)
SITE(2, 0x99aabbccddeeff00, 0x00ffeeddccbbaa99)
SITE(3, 0x55aa55aa55aa55aa, 0xaa55aa55aa55aa55)

static void *listener(void *arg) {
    (void)arg;
    for (;;) {
        exc_msg_t msg;
        memset(&msg, 0, sizeof msg);
        mach_msg_return_t mr = mach_msg(&msg.Head, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                        0, sizeof msg, g_port, 4000, MACH_PORT_NULL);
        if (mr == MACH_RCV_TIMED_OUT) break;
        if (mr != MACH_MSG_SUCCESS) { logf("listener recv FAILED %d\n", (int)mr); break; }
        g_faults++;
        logf("FAULT #%d: type %d codes [%lld,%lld] flavor %d cnt %u",
             g_faults, msg.exception, (long long)msg.code[0], (long long)msg.code[1],
             msg.flavor, msg.old_stateCnt);
        if (msg.flavor == ARM_THREAD_STATE64 && msg.old_stateCnt >= 66) {
            arm_st64_view_t st;
            memcpy(&st, msg.old_state, sizeof st);
            logf(" pc %016llx x0 %llx x26 %llx x27 %llx x28 %llx\n",
                 (unsigned long long)st.pc, (unsigned long long)st.x[0],
                 (unsigned long long)st.x[26], (unsigned long long)st.x[27],
                 (unsigned long long)st.x[28]);
            const uint32_t *pw = (const uint32_t *)(uintptr_t)st.pc;
            uint32_t w;
            logf("  context before:");
            for (int k = -10; k <= 2; k++) {
                if (safe_read32(pw + k, &w)) logf(" [%+d]%08x", k, w);
                else { logf(" [%+d]XXXX", k); break; }
            }
            logf("\n");
        } else logf(" (state flavor %d not ARM64 view)\n", msg.flavor);

        exc_reply_t reply;
        memset(&reply, 0, sizeof reply);
        reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        reply.Head.msgh_size = sizeof reply;
        reply.Head.msgh_remote_port = msg.Head.msgh_remote_port;
        reply.Head.msgh_local_port = MACH_PORT_NULL;
        reply.Head.msgh_id = msg.Head.msgh_id + 100;
        reply.NDR = msg.NDR;
        reply.flavor = msg.flavor;
        reply.new_stateCnt = msg.old_stateCnt;
        memcpy(reply.new_state, msg.old_state, msg.old_stateCnt * sizeof(natural_t));
        if (g_faults <= 64 && msg.flavor == ARM_THREAD_STATE64 && msg.old_stateCnt >= 66) {
            arm_st64_view_t *rs = (arm_st64_view_t *)reply.new_state;
            rs->pc += 4;                       /* skip the corrupt word */
            reply.RetCode = KERN_SUCCESS;
            logf("  reply: skip +4\n");
        } else {
            reply.RetCode = KERN_FAILURE;      /* pass through: enough evidence */
            logf("  reply: pass-through (cap reached)\n");
        }
        mach_msg(&reply.Head, MACH_SEND_MSG, sizeof reply, 0,
                 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return NULL;
}

static int is_movz_x26_family(uint32_t w) {
    return (w & 0xffe0001fu) == 0xd2800000u + 26u ||
           (w & 0xffe0001fu) == 0xf2800000u + 26u ||
           (w & 0xffe0001fu) == 0xf2a00000u + 26u ||
           (w & 0xffe0001fu) == 0xf2c00000u + 26u ||
           (w & 0xffe0001fu) == 0xf2e00000u + 26u;
}
#define BR_X26 0xd61f0340u
#define BLR_X26 0xd63f0340u

static int g_scan_gen = 0;
static void scan_and_dump(void) {
    logf("scan %d: walking regions (page-guarded fast scan)\n", g_scan_gen);
    mach_vm_address_t addr = 0;
    mach_vm_size_t size;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt;
    natural_t depth = 0;
    unsigned regions = 0, hits = 0, stomps = 0;
    for (;;) {
        cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS)
            break;
        mach_vm_address_t next = addr + size;
        if (!info.is_submap && size <= 512ull * 1024 * 1024 &&
            (info.protection & VM_PROT_READ) &&
            ((info.protection | info.max_protection) & VM_PROT_EXECUTE)) {
            char nm[256] = {0};
            proc_regionfilename(getpid(), addr, nm, sizeof nm - 1);
            if (!nm[0]) {
                regions++;
                /* page-guarded: one guarded probe per 4K page, then an
                 * unguarded word loop inside the page (a readable first
                 * word means the whole page is readable) */
                for (unsigned long pgoff = 0; pgoff < size; pgoff += 0x1000) {
                    const uint32_t *pw = (const uint32_t *)(addr + pgoff);
                    unsigned limit = (size - pgoff < 0x1000) ? (unsigned)(size - pgoff) : 0x1000;
                    uint32_t probe;
                    if (!safe_read32(pw, &probe)) continue;   /* hole: skip page */
                    /* one recovery point per PAGE (not per word — the v3 run
                     * died _exit(77) on an unguarded load when Rosetta tore
                     * an arena down mid-scan; any fault now skips the page) */
                    g_armed = 1;
                    if (sigsetjmp(g_jmp, 1)) { g_armed = 0; continue; }
                    for (unsigned wo = 0; wo + 4 <= limit; wo++) {
                        uint32_t w = pw[wo];
                        const uint32_t *at = pw + wo;
                        int is_chain = 0;
                        if (w == BR_X26 || w == BLR_X26) {
                            for (int k = 1; k <= 4 && !is_chain; k++)
                                if (is_movz_x26_family(at[-k])) is_chain = 1;
                        }
                        int is_stomp = (w & 0xfffff000u) == 0x13ffe000u;
                        if (is_chain) {
                            char path[128];
                            snprintf(path, sizeof path, "/tmp/rosetta-re/tbdump-g%u-h%u.bin", g_scan_gen, hits);
                            FILE *f = fopen(path, "wb");
                            logf("HIT g%u-%u at %016llx (region %016llx prot %x) words:",
                                 g_scan_gen, hits,
                                 (unsigned long long)(uintptr_t)at,
                                 (unsigned long long)addr,
                                 (unsigned)info.protection);
                            for (int k = -12; k <= 8; k++) {
                                uint32_t v = at[k];
                                logf(" %08x", v);
                                if (f) fwrite(&v, 4, 1, f);
                            }
                            logf("\n");
                            if (f) fclose(f);
                            hits++;
                            wo += 8;
                        } else if (is_stomp) {
                            if (stomps < 8)
                                logf("STOMP g%u-%u at %016llx: %08x ctx:",
                                     g_scan_gen, stomps,
                                     (unsigned long long)(uintptr_t)at, w);
                            for (int k = -3; k <= 3 && stomps < 8; k++)
                                logf(" [%+d]%08x", k, at[k]);
                            if (stomps < 8) logf("\n");
                            stomps++;
                        }
                    }
                    g_armed = 0;
                }
            }
        }
        addr = next;
    }
    logf("scan done: %u regions, %u insert-chain hits, %u stomps\n", regions, hits, stomps);
}

static void on_alrm(int sig) { (void)sig; logf("SIGALRM: emergency scan+exit\n"); scan_and_dump(); _exit(3); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_fd = open("/tmp/rosetta-re/capsite3.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("log"); return 1; }
    logf("=== capsite3 pid %d\n", (int)getpid());

    struct sigaction rec = {.sa_sigaction = recover, .sa_flags = SA_SIGINFO};
    sigemptyset(&rec.sa_mask);
    sigaction(SIGSEGV, &rec, NULL);
    sigaction(SIGBUS, &rec, NULL);
    struct sigaction ss = {.sa_sigaction = sigsys_noop, .sa_flags = SA_SIGINFO};
    sigemptyset(&ss.sa_mask);
    sigaction(SIGSYS, &ss, NULL);
    signal(SIGALRM, on_alrm);
    alarm(20);
    logf("handlers installed\n");

    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_port);
    if (kr != KERN_SUCCESS) { logf("port alloc FAILED %d\n", (int)kr); return 1; }
    kr = mach_port_insert_right(mach_task_self(), g_port, g_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) { logf("insert right FAILED %d\n", (int)kr); return 1; }
    pthread_t th;
    if (pthread_create(&th, NULL, listener, NULL) != 0) { perror("pthread"); return 1; }
    kr = thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_INSTRUCTION,
                                    g_port, EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES,
                                    ARM_THREAD_STATE64);
    if (kr != KERN_SUCCESS) { logf("set_exception_ports FAILED %d\n", (int)kr); return 1; }
    logf("BAD_INSTRUCTION port installed (main thread)\n");

    unsigned long sink[2];
    sink[0] = sink[1] = 0; site1(sink);
    logf("site1 run1: %016lx %016lx (faults %d)\n", sink[0], sink[1], g_faults);
    scan_and_dump();          /* run1 was clean: TB exists, dump before run2 */
    g_scan_gen = 1;

    sink[0] = sink[1] = 0; site1(sink);
    logf("site1 run2: %016lx %016lx (faults %d)\n", sink[0], sink[1], g_faults);
    scan_and_dump();
    g_scan_gen = 2;

    logf("phase3: site2\n");
    sink[0] = sink[1] = 0; site2(sink);
    logf("site2 run1: %016lx %016lx (faults %d)\n", sink[0], sink[1], g_faults);
    logf("phase4: site3\n");
    sink[0] = sink[1] = 0; site3(sink);
    logf("site3 run1: %016lx %016lx (faults %d)\n", sink[0], sink[1], g_faults);
    alarm(0);
    logf("=== capsite3 done\n");
    return 0;
}
