/* exctest.c — can an in-process thread-level Mach exception handler intercept
 * the brk#5 raised by a translated thread's invalid-class `syscall`?
 *
 * This is the feasibility probe for the exception-routing variant of the
 * syscall fastpath (NOTES.md, "thunkmap probe" section): if the exception can
 * be caught in-process and answered with a redirected PC, the brk#5 ->
 * exceptionserver -> SIGSYS -> Wine round trip (~16-22 us) can be replaced
 * WITHOUT reading or writing any Rosetta JIT page — both of which are now
 * known-dangerous (kernel panic class, 2026-11-07).
 *
 * Method constraints honored: no mach_vm_* calls at all (no vm reads, writes
 * or protects — nothing in either known kernel-crash class). Only ordinary
 * mach port RPC, the same exception delivery path Rosetta's exceptionserver
 * itself uses. Everything runs in-process on our own thread.
 *
 * Probes, in order:
 *   0. log the thread/task exception ports that exist BEFORE we touch
 *      anything (what does Rosetta hold?)
 *   A. set a thread-level port; trigger one invalid-class syscall; listener
 *      logs the exception (type, codes, full ARM state) and replies
 *      KERN_FAILURE — i.e. "not handled" — which makes the kernel fall
 *      through to the next handler in the chain (Rosetta's). Expect: the
 *      stock SIGSYS path still completes (marker after the syscall stores,
 *      SIGSYS noop handler runs). This proves interception AND clean
 *      fail-closed pass-through.
 *   B. trigger again; this time reply KERN_SUCCESS with pc += 4 (skip the
 *      brk word). Observe whether execution resumes at the modified PC —
 *      repeated exceptions / continued execution both tell us something.
 *      Any exception after the first B reply gets KERN_FAILURE (escape from
 *      a re-execute loop defers to Rosetta).
 *
 * Exit: 0 full success (A and B observed), 3 intercepted but no redirect,
 * 5 no exception ever arrived (routing not interceptable at thread level),
 * 1 setup failure. Every step is fsync-logged to /tmp/rosetta-re/exctest.log
 * before it runs.
 *
 *   clang -arch x86_64 -O1 -Wall -o exctest exctest.c && ./exctest
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/exc.h>

/* arm thread state is not declared in the x86_64 SDK slice; the layout is
 * ABI-stable, so declare the leading-field view we need. Full flavor buffer
 * is 416 bytes (104 words); the kernel tells us the real count per message. */
#define ARM_THREAD_STATE64       6
#define ARM_THREAD_STATE64_COUNT 104
typedef struct {
    uint64_t x[29];
    uint64_t fp, lr, sp, pc;
    uint32_t cpsr, pad;
} arm_st64_view_t;               /* pc sits at word 64 (byte 256) */

static int g_fd = -1;
static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); va_end(ap);
    fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fflush(stdout);
}

/* ---- exception message shapes (EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES)
 * hand-rolled because we do not link MIG-generated servers */
typedef struct {
    mach_msg_header_t  Head;
    mach_msg_body_t    body;
    mach_msg_port_descriptor_t task;    /* MACH_MSG_TYPE_PORT_SEND right */
    mach_msg_port_descriptor_t thread;
    NDR_record_t       NDR;
    exception_type_t   exception;
    mach_msg_type_number_t codeCnt;
    int64_t            code[2];
    int                flavor;
    mach_msg_type_number_t old_stateCnt;
    natural_t          old_state[ARM_THREAD_STATE64_COUNT];
    mach_msg_trailer_t trailer;         /* minimal trailer for the kernel send */
} exc_msg_t;

typedef struct {
    mach_msg_header_t  Head;
    NDR_record_t       NDR;
    kern_return_t      RetCode;
    int                flavor;
    mach_msg_type_number_t new_stateCnt;
    natural_t          new_state[ARM_THREAD_STATE64_COUNT];
} exc_reply_t;

/* ---- shared state with the listener thread */
static mach_port_t g_port = MACH_PORT_NULL;
static volatile int g_mode = 0;        /* 0 idle, 1 = A (KERN_FAILURE), 2 = B (redirect) */
static volatile int g_seen = 0;        /* exceptions observed */
static volatile int g_repeats = 0;     /* exceptions after the first B reply */
static volatile kern_return_t g_reply_rc = -1;

static void dump_state(const char *tag, const arm_st64_view_t *st) {
    logf("  %s pc %016llx lr %016llx fp %016llx sp %016llx cpsr %08x\n",
         tag, (unsigned long long)st->pc, (unsigned long long)st->lr,
         (unsigned long long)st->fp, (unsigned long long)st->sp, st->cpsr);
    logf("  %s x0 %016llx x1 %016llx x8 %016llx x16 %016llx x17 %016llx\n",
         tag, (unsigned long long)st->x[0], (unsigned long long)st->x[1],
         (unsigned long long)st->x[8], (unsigned long long)st->x[16],
         (unsigned long long)st->x[17]);
    logf("  %s x22 %016llx x23 %016llx x26 %016llx x27 %016llx x28 %016llx\n",
         tag, (unsigned long long)st->x[22], (unsigned long long)st->x[23],
         (unsigned long long)st->x[26], (unsigned long long)st->x[27],
         (unsigned long long)st->x[28]);
}

static void *listener(void *arg) {
    (void)arg;
    for (;;) {
        exc_msg_t msg;
        memset(&msg, 0, sizeof msg);
        mach_msg_return_t mr = mach_msg(&msg.Head, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                        0, sizeof msg, g_port, 4000, MACH_PORT_NULL);
        if (mr == MACH_RCV_TIMED_OUT) { logf("listener: recv timeout (no more exceptions)\n"); break; }
        if (mr != MACH_MSG_SUCCESS) { logf("listener: recv FAILED mr=%d\n", mr); break; }
        g_seen++;
        logf("exception #%d: type %d codes [%lld, %lld] flavor %d statesz %u\n",
             g_seen, msg.exception, (long long)msg.code[0], (long long)msg.code[1],
             msg.flavor, msg.old_stateCnt);
        if (msg.old_stateCnt >= ARM_THREAD_STATE64_COUNT && msg.flavor == ARM_THREAD_STATE64)
            dump_state("state", (const arm_st64_view_t *)msg.old_state);
        else {
            logf("  raw state (%u words):", msg.old_stateCnt);
            for (unsigned w = 0; w < msg.old_stateCnt && w < 16; w++)
                logf(" %08x", msg.old_state[w]);
            logf("\n");
        }

        exc_reply_t reply;
        memset(&reply, 0, sizeof reply);
        reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        reply.Head.msgh_size = sizeof reply;
        reply.Head.msgh_remote_port = msg.Head.msgh_remote_port;
        reply.Head.msgh_local_port = MACH_PORT_NULL;
        reply.Head.msgh_id = msg.Head.msgh_id + 100;   /* reply id = msg id + 100 */
        reply.NDR = msg.NDR;
        reply.flavor = msg.flavor;
        reply.new_stateCnt = msg.old_stateCnt;
        memcpy(reply.new_state, msg.old_state,
               msg.old_stateCnt * sizeof(natural_t));

        if (g_mode == 1 || g_seen > 1 /* any repeat: escape via pass-through */) {
            if (g_mode == 2 && g_seen > 1) g_repeats++;
            reply.RetCode = KERN_FAILURE;      /* not handled: kernel falls
                                                  through to Rosetta's chain */
            logf("  reply KERN_FAILURE (pass-through)\n");
        } else if (msg.flavor == ARM_THREAD_STATE64 &&
                   msg.old_stateCnt >= 66) {   /* mode B: redirect */
            arm_st64_view_t *st = (arm_st64_view_t *)reply.new_state;
            st->pc += 4;                      /* skip the brk word */
            reply.RetCode = KERN_SUCCESS;
            logf("  reply KERN_SUCCESS pc->%016llx (+4)\n",
                 (unsigned long long)st->pc);
        } else {
            reply.RetCode = KERN_FAILURE;      /* unknown state flavor */
            logf("  reply KERN_FAILURE (unhandled flavor %d)\n", msg.flavor);
        }
        g_reply_rc = mach_msg(&reply.Head, MACH_SEND_MSG, sizeof reply, 0,
                              MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        logf("  reply send rc=%d\n", (int)g_reply_rc);
    }
    return NULL;
}

static void sigsys_noop(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; }

static const unsigned char code[] = {
    0x49, 0xb8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, /* movabs $0x8877665544332211,%r8 */
    0x4c, 0x89, 0x07,                     /* mov %r8,(%rdi)      marker 1 (before) */
    0xb8, 0xff, 0xff, 0xff, 0xff,         /* mov $0xffffffff,%eax */
    0x0f, 0x05,                           /* syscall             -> brk#5 under Rosetta */
    0x49, 0xb8, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, /* movabs marker2 */
    0x4c, 0x89, 0x47, 0x08,               /* mov %r8,0x8(%rdi)   marker 2 (after) */
    0xc3,                                 /* ret */
};
#define M1 0x8877665544332211UL
#define M2 0x00ffeeddccbbaa99UL

static void show_ports(const char *what, mach_port_t tgt) {
    logf("%s exception ports BEFORE we touch anything:\n", what);
    exception_mask_t masks[8];
    mach_port_t ports[8];
    exception_behavior_t beh[8];
    thread_state_flavor_t flav[8];
    mach_msg_type_number_t n = 8;
    kern_return_t kr = thread_get_exception_ports(tgt, EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BREAKPOINT | EXC_MASK_BAD_ACCESS, masks, ports, beh, flav, &n);
    if (kr != KERN_SUCCESS) { logf("  get FAILED kr=%d\n", kr); return; }
    for (unsigned i = 0; i < n; i++)
        logf("  mask %08x port %x behavior %x flavor %d\n",
             masks[i], ports[i] ? (unsigned)MACH_PORT_INDEX(ports[i]) : 0, beh[i], flav[i]);
    for (unsigned i = 0; i < n; i++)
        if (ports[i]) mach_port_deallocate(mach_task_self(), ports[i]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_fd = open("/tmp/rosetta-re/exctest.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("log"); return 1; }
    logf("=== exctest pid %d (mach ports only, no vm calls)\n", (int)getpid());

    struct sigaction ss = {.sa_sigaction = sigsys_noop, .sa_flags = SA_SIGINFO};
    sigemptyset(&ss.sa_mask);
    sigaction(SIGSYS, &ss, NULL);
    logf("SIGSYS noop handler installed\n");

    show_ports("thread", mach_thread_self());

    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_port);
    if (kr != KERN_SUCCESS) { logf("port allocate FAILED %d\n", kr); return 1; }
    kr = mach_port_insert_right(mach_task_self(), g_port, g_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) { logf("insert right FAILED %d\n", kr); return 1; }
    logf("port ready %x\n", (unsigned)MACH_PORT_INDEX(g_port));

    pthread_t th;
    if (pthread_create(&th, NULL, listener, NULL) != 0) { perror("pthread"); return 1; }
    logf("listener running\n");

    logf("installing thread-level port for EXC_BAD_INSTRUCTION|EXC_BREAKPOINT|EXC_BAD_ACCESS "
         "(STATE_IDENTITY|MACH_EXCEPTION_CODES, ARM_THREAD_STATE64)\n");
    int tasklevel = !!getenv("TASKLEVEL");
    if (!getenv("NOSET")) {
        logf(tasklevel ? "TASK-LEVEL install\n" : "thread-level install\n");
        if (tasklevel)
            kr = task_set_exception_ports(mach_task_self(),
                                        EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BREAKPOINT |
                                        EXC_MASK_BAD_ACCESS, g_port,
                                        EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES,
                                        ARM_THREAD_STATE64);
        else
            kr = thread_set_exception_ports(mach_thread_self(),
                                        EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BREAKPOINT |
                                        EXC_MASK_BAD_ACCESS, g_port,
                                        EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES,
                                        ARM_THREAD_STATE64);
        if (kr != KERN_SUCCESS) { logf("set_exception_ports FAILED kr=%d\n", kr); return 1; }
        logf("installed\n");
    } else {
        logf("NOSET: control run, port NOT installed\n");
    }

    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) { perror("mmap"); return 1; }
    memcpy(page, code, sizeof code);
    unsigned long sink[2] = {0, 0};

    logf("trigger A (reply KERN_FAILURE = pass-through)\n");
    g_seen = 0; g_mode = 1;
    logf("pre-call page=%p sink=%p sink0=%lx sink1=%lx sizeof code=%zu\n",
         page, (void*)sink, sink[0], sink[1], sizeof code);
    ((void (*)(unsigned long *))page)(sink);
    logf("post-call sink0=%lx sink1=%lx\n", sink[0], sink[1]);
    sleep(1);
    logf("A result: marker1 %s marker2 %s exceptions %d\n",
         sink[0] == M1 ? "OK" : "MISSING",
         sink[1] == M2 ? "OK" : "MISSING", g_seen);

    logf("trigger B (reply KERN_SUCCESS, pc += 4)\n");
    sink[0] = sink[1] = 0; g_seen = 0; g_mode = 2;
    ((void (*)(unsigned long *))page)(sink);
    sleep(1);
    logf("B result: marker1 %s marker2 %s exceptions %d repeats %d\n",
         sink[0] == M1 ? "OK" : "MISSING",
         sink[1] == M2 ? "OK" : "MISSING", g_seen, g_repeats);

    if (!getenv("RUN_C")) { logf("trigger C skipped (RUN_C=1 to run)\n"); }
    else {
    logf("trigger C (null write from translated code; reply pc+=4)\n");
    g_seen = 0; g_mode = 2;
    *(volatile int *)0 = 42;
    logf("C SURVIVED the null write (state-modified reply worked)\n");
    sleep(1);
    logf("C result: exceptions %d repeats %d\n", g_seen, g_repeats);
    }

    g_mode = 0;
    pthread_join(th, NULL);
    logf("=== exctest done\n");

    int rc;
    if (g_seen == 0) { logf("VERDICT: NOT interceptable at thread level\n"); rc = 5; }
    else if (sink[1] == M2) { logf("VERDICT: intercepted AND redirectable\n"); rc = 0; }
    else { logf("VERDICT: intercepted; redirect inconclusive\n"); rc = 3; }
    printf("exctest rc=%d\n", rc);
    return rc;
}
