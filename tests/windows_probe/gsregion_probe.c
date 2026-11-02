/* Does code in dynamically created executable memory see the same GS as loader-mapped code?
 * On Windows (and Proton), gs is the TEB for every instruction, wherever it lives. On
 * MacFinals, Wine registers code with Rosetta (register_non_native_code_region) only for
 * PE modules loaded through its own loader, so code in memory made executable later may
 * read something else through gs. This copies one tiny routine that reads gs:[0x30] (TEB
 * self), gs:[0x60] (PEB) and gs:[0x58] (TLS pointer) into several kinds of executable
 * memory, runs it, and compares with the same reads from this image.
 *
 * Build: x86_64-w64-mingw32-clang -O1 -Wall gsregion_probe.c -o gsregion-probe.exe
 * Run "gsregion-probe.exe d3d" to create a D3D12 device first in each case (as the game does).
 * Run with no arguments: it runs each case in a child process, so a case that kills the
 * process cannot hide the others. Run the same .exe on MacFinals, Windows and Proton. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* void f(void *out): out[0]=gs:[0x40] pid, out[1]=gs:[0x48] tid, out[2]=gs:[0x08] StackBase.
 * Wine on macOS mirrors only a few TEB fields (0x30, 0x58, 0x60) at their gs offsets, so
 * these deliberately read fields it does not mirror. */
static const unsigned char gsread[] = {
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x40, 0x00, 0x00, 0x00,  /* mov rax, gs:[0x40] pid */
    0x48, 0x89, 0x01,                                      /* mov [rcx], rax     */
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,  /* mov rax, gs:[0x48] tid */
    0x48, 0x89, 0x41, 0x08,                                /* mov [rcx+8], rax   */
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x08, 0x00, 0x00, 0x00,  /* mov rax, gs:[0x08] StackBase */
    0x48, 0x89, 0x41, 0x10,                                /* mov [rcx+16], rax  */
    0xc3,                                                  /* ret                */
};
typedef void (*gsfn)(void *);

/* the truth: the same fields read from the TEB itself, not through gs */
static void reference(void **out) {
    unsigned char *teb = (unsigned char *)NtCurrentTeb();
    out[0] = *(void **)(teb + 0x40);
    out[1] = *(void **)(teb + 0x48);
    out[2] = *(void **)(teb + 0x08);
}
/* the same reads through gs from this loader-mapped image */
static void gs_here(void **out) {
    __asm__ volatile("mov %%gs:0x40,%0" : "=r"(out[0]));
    __asm__ volatile("mov %%gs:0x48,%0" : "=r"(out[1]));
    __asm__ volatile("mov %%gs:0x08,%0" : "=r"(out[2]));
}

static void *place(int which, const char **name) {
    void *p = NULL; DWORD old;
    switch (which) {
    case 0:
        *name = "VirtualAlloc RWX";
        p = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) memcpy(p, gsread, sizeof gsread);
        break;
    case 1:
        *name = "VirtualAlloc RW -> VirtualProtect RX";
        p = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p) { memcpy(p, gsread, sizeof gsread);
                 if (!VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old)) p = NULL; }
        break;
    case 2: {
        *name = "section view (FILE_MAP_EXECUTE)";
        HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, 0x1000, NULL);
        if (m) p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, 0x1000);
        if (p) memcpy(p, gsread, sizeof gsread);
        break; }
    case 3:
        *name = "reserve then commit RX at 0x710000000000";
        p = VirtualAlloc((void *)0x710000000000ull, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
        if (p) p = VirtualAlloc(p, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        if (p) { memcpy(p, gsread, sizeof gsread);
                 if (!VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old)) p = NULL; }
        break;
    case 4:
        *name = "same, second thread (case 1 memory)";
        return place(1, &(const char *){0});
    default:
        return NULL;
    }
    return p;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    printf("    exception %08lx at %p\n", ep->ExceptionRecord->ExceptionCode,
           ep->ExceptionRecord->ExceptionAddress);
    ExitProcess(3);
}

struct job { gsfn f; void *out[3]; };
static DWORD WINAPI run_on_thread(void *arg) {
    struct job *j = arg; j->f(j->out); return 0;
}

/* the game always has D3DMetal loaded; GPTK's code-region registration exists to tell
 * Windows code from D3DMetal's macOS x86 code in one process, so GS may only change then */
static int with_d3d;
static void load_d3d12(void) {
    HMODULE d = LoadLibraryA("d3d12.dll");
    typedef HRESULT (WINAPI *create_fn)(void *, int, const GUID *, void **);
    static const GUID iid_dev = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
    create_fn create = d ? (create_fn)(void *)GetProcAddress(d, "D3D12CreateDevice") : NULL;
    void *dev = NULL;
    HRESULT hr = create ? create(NULL, 0xb000 /* FL 11_0 */, &iid_dev, &dev) : E_FAIL;
    printf("    d3d12: module %p device %s (hr %08lx)\n", (void *)d, dev ? "created" : "NOT created", hr);
}

static int run_case(int which) {
    const char *name = "?"; void *ref[3], *got[3] = {0};
    AddVectoredExceptionHandler(1, veh);
    if (with_d3d) load_d3d12();
    void *p = place(which, &name);
    if (which == 4) name = "RW -> RX, called on a second thread";
    if (!p) { printf("%-42s could not allocate (%lu)\n", name, GetLastError()); return 2; }
    FlushInstructionCache(GetCurrentProcess(), p, sizeof gsread);
    if (which == 4) {
        struct job j = {(gsfn)p, {0}};
        HANDLE t = CreateThread(NULL, 0, run_on_thread, &j, 0, NULL);
        WaitForSingleObject(t, INFINITE);
        memcpy(got, j.out, sizeof got);
        /* that thread's TEB is gone; compare PEB, and TEB only for "not ours" */
        reference(ref);
        printf("%-42s at %p: pid %s  tid %s\n", name, p,
               got[0] == ref[0] ? "same" : "DIFF",
               got[1] && got[1] != ref[1] ? "own (ok)" : "WRONG");
        return 0;
    }
    ((gsfn)p)(got);
    reference(ref);
    printf("%-42s at %p: pid %s  tid %s  StackBase %s\n", name, p,
           got[0] == ref[0] ? "same" : "DIFF", got[1] == ref[1] ? "same" : "DIFF",
           got[2] == ref[2] ? "same" : "DIFF");
    if (got[0] != ref[0] || got[1] != ref[1] || got[2] != ref[2])
        printf("    got %p %p %p  want %p %p %p\n", got[0], got[1], got[2], ref[0], ref[1], ref[2]);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    with_d3d = argc > 2 || (argc > 1 && !strcmp(argv[1], "d3d"));
    if (argc > 1 && strcmp(argv[1], "d3d")) return run_case(atoi(argv[1]));
    void *ref[3], *here[3]; reference(ref);
    if (with_d3d) load_d3d12();
    gs_here(here);
    printf("TEB truth                                  pid %p tid %p StackBase %p\n", ref[0], ref[1], ref[2]);
    printf("gs reads from this loader-mapped image     pid %s  tid %s  StackBase %s\n",
           here[0] == ref[0] ? "same" : "DIFF", here[1] == ref[1] ? "same" : "DIFF",
           here[2] == ref[2] ? "same" : "DIFF");
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
    for (int i = 0; i <= 4; i++) {
        char cmd[MAX_PATH + 16]; snprintf(cmd, sizeof cmd, "\"%s\" %d%s", self, i, with_d3d ? " d3d" : "");
        STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi;
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            printf("case %d: CreateProcess failed %lu\n", i, GetLastError()); continue; }
        if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_TIMEOUT) {
            printf("case %d: hung, killed\n", i); TerminateProcess(pi.hProcess, 9); }
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        if (code != 0) printf("    case %d exit code %08lx\n", i, code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    return 0;
}
