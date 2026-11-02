/* exectest.c — milestone 1 of the NT correction (NOTES.md "Next milestones"):
 * a payload page whose arm64 code executes natively, in-process, public API
 * only. Exactly ONE tier per process run (argv[1] = tier), so a crash on one
 * tier leaves the others independently testable.
 *
 *   1  anonymous RWX page + raw indirect call from translated code — the
 *      control. Expected: the translator treats an unregistered page as x86
 *      guest code; our arm64 words then decode as garbage and the trailing
 *      0xd6 byte of `ret` is #UD in 64-bit mode, so SIGILL here means
 *      "executed as x86", not "page broken".
 *   2  the same page after syscall(0x7000009, start, X) — the GPTK-public
 *      register_non_native_code_region (gptk/redist libd3dshared.dylib issues
 *      exactly this; our Wine calls it for every PE module it maps, so it is
 *      production-exercised on this machine). 2a = Wine's (start, end) shape,
 *      2b = (start, len).
 *   3  dlopen of a self-built arm64 dylib, dlsym, indirect call — the GPTK
 *      pattern proper: their x86_64 D3DMetal imports arm64 Metal frameworks,
 *      so dyld-bound translated->native calls are the proven bridge. Payload
 *      path from $MF_PAYLOAD_DYLIB, default payload.dylib next to this tool.
 *
 * Payload arm64 (tier 1/2 pages): three probes a coincidence cannot pass:
 *   +0x00 movz w0,#42; ret   +0x08 movz w0,#90; ret   +0x10 add w0,w0,#1; ret
 *
 * Anonymous memory only; nothing file-backed is written or re-protected; the
 * one syscall targets a page we allocated. Steps fsync to
 * /tmp/rosetta-re/exectest.log before/after so a wedge names its step.
 *
 *   clang -arch x86_64 -O1 -Wall -o exectest exectest.c && ./exectest 1
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <libkern/OSCacheControl.h>

static int g_fd = -1;

static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt); vdprintf(g_fd, fmt, ap); va_end(ap);
    fsync(g_fd);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fflush(stdout);
}

static const uint32_t g_code[] = {
    0x52800540, 0xd65f03c0, /* +0x00 movz w0,#42; ret */
    0x52800b40, 0xd65f03c0, /* +0x08 movz w0,#90; ret */
    0x11000400, 0xd65f03c0, /* +0x10 add w0,w0,#1;  ret */
};
typedef int (*fn0)(void);
typedef int (*fn1)(int);

/* perms + vm tag of the page containing a, so a registration that relabels
 * the region shows up in the log. */
static void log_region(const char *tag, mach_vm_address_t a) {
    mach_vm_address_t addr = a;
    mach_vm_size_t size = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    natural_t depth = 0;
    if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                               (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) {
        logf("%s %016llx: region lookup FAILED\n", tag, (unsigned long long)a);
        return;
    }
    logf("%s %016llx: prot %x/%x tag %u share %u\n", tag, (unsigned long long)a,
         info.protection, info.max_protection, info.user_tag, info.share_mode);
}

static void *payload_page(void) {
    void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) { logf("mmap RWX FAILED: %s\n", strerror(errno)); return NULL; }
    memcpy(p, g_code, sizeof g_code);
    sys_icache_invalidate(p, sizeof g_code);
    log_region("page", (mach_vm_address_t)p);
    return p;
}

/* Call all three probes and report. The calls themselves are the experiment:
 * native arm64 -> 42/90/42; translated-as-x86 -> #UD (SIGILL) or junk. */
static void probe_page(const char *tag, void *p) {
    volatile fn0 f0 = (fn0)p, f1 = (fn0)((char *)p + 8);
    volatile fn1 f2 = (fn1)((char *)p + 16);
    logf("%s: calling probe 0 (movz w0,#42)\n", tag);
    int r0 = f0();
    logf("%s: probe 0 -> %d (%s)\n", tag, r0, r0 == 42 ? "ok" : "WRONG");
    logf("%s: calling probe 1 (movz w0,#90)\n", tag);
    int r1 = f1();
    logf("%s: probe 1 -> %d (%s)\n", tag, r1, r1 == 90 ? "ok" : "WRONG");
    logf("%s: calling probe 2 (add w0,w0,#1, arg 41)\n", tag);
    int r2 = f2(41);
    logf("%s: probe 2 -> %d (%s)\n", tag, r2, r2 == 42 ? "ok" : "WRONG");
    logf("%s: verdict %s\n", tag,
         (r0 == 42 && r1 == 90 && r2 == 42) ? "NATIVE ARM64 EXECUTION" : "not native");
}

/* GPTK's register_non_native_code_region, verbatim: syscall(0x7000009, a, b).
 * Our Wine calls it with (start, end) for every mapped PE; the GPTK dylib's
 * own signature suggests (start, len) — tier 2 tries both. */
static long reg_non_native(void *a, void *b) {
    errno = 0;
    long r = syscall(0x7000009, a, b);
    logf("syscall(0x7000009, %p, %p) -> %ld errno %d (%s)\n", a, b, r, errno,
         errno ? strerror(errno) : "-");
    return r;
}

static int tier1(void) {
    void *p = payload_page();
    if (!p) return 1;
    probe_page("tier1 plain RWX anon", p);
    return 0;
}

static int tier2(void) {
    void *p = payload_page();
    if (!p) return 1;
    logf("tier2a: registering (start, end)\n");
    reg_non_native(p, (char *)p + 0x1000);
    log_region("after 2a", (mach_vm_address_t)p);
    probe_page("tier2a registered (start,end)", p);

    void *q = payload_page();
    if (!q) return 1;
    logf("tier2b: registering (start, len)\n");
    reg_non_native(q, (void *)0x1000);
    log_region("after 2b", (mach_vm_address_t)q);
    probe_page("tier2b registered (start,len)", q);
    return 0;
}

static int tier3(void) {
    const char *path = getenv("MF_PAYLOAD_DYLIB");
    if (!path) {
        char cwd[4096];
        getcwd(cwd, sizeof cwd);
        static char def[4096];
        snprintf(def, sizeof def, "%s/payload.dylib", cwd);
        path = def;
    }
    logf("tier3: dlopen(%s)\n", path);
    void *h = dlopen(path, RTLD_LAZY);
    if (!h) { logf("tier3: dlopen FAILED: %s\n", dlerror()); return 1; }
    fn0 c = (fn0)dlsym(h, "nt_probe_const");
    fn1 i = (fn1)dlsym(h, "nt_probe_inc");
    if (!c || !i) { logf("tier3: dlsym FAILED (%p %p): %s\n", c, i, dlerror()); return 1; }
    Dl_info dli;
    if (dladdr(c, &dli)) {
        unsigned long sz = 0;
        getsegmentdata(dli.dli_fbase, "__TEXT", &sz);
        logf("tier3: payload image %016llx __TEXT %016llx..%016llx sym %llx\n",
             (unsigned long long)(uintptr_t)dli.dli_fbase,
             (unsigned long long)(uintptr_t)dli.dli_fbase,
             (unsigned long long)(uintptr_t)dli.dli_fbase + sz,
             (unsigned long long)((char *)c - (char *)dli.dli_fbase));
        log_region("payload", (mach_vm_address_t)dli.dli_fbase);
    }
    volatile fn0 c0 = c; volatile fn1 i0 = i;
    for (int pass = 1; pass <= 2; pass++) { /* cold then warm */
        logf("tier3: calling probes (pass %d)\n", pass);
        int a = c0(), b = i0(41);
        logf("tier3: pass %d -> const %d (%s), inc(41) %d (%s)\n", pass, a,
             a == 42 ? "ok" : "WRONG", b, b == 42 ? "ok" : "WRONG");
        if (a != 42 || b != 42) return 1;
    }
    logf("tier3: verdict NATIVE ARM64 EXECUTION via dyld bridge\n");
    return 0;
}

int main(int argc, char **argv) {
    int tier = argc > 1 ? atoi(argv[1]) : 1;
    if (tier < 1 || tier > 3) tier = 1;
    g_fd = open("/tmp/rosetta-re/exectest.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_fd < 0) { perror("/tmp/rosetta-re/exectest.log"); return 2; }
    int translated = -1; size_t l = sizeof translated;
    /* "kern.proc_translated" is gone on this macOS; the live OID is the one
     * GPTK's libd3dshared also queries: "sysctl.proc_translated" */
    sysctlbyname("sysctl.proc_translated", &translated, &l, NULL, 0);
    logf("=== exectest pid %d tier %d translated %d\n", getpid(), tier, translated);
    if (translated != 1) { logf("=== not translated; tiers meaningless\n"); return 2; }

    int rc = tier == 1 ? tier1() : tier == 2 ? tier2() : tier3();
    logf("=== exectest tier %d done rc=%d\n", tier, rc);
    return rc;
}
