/* Does NT stay set across what happens between two instructions: system calls, callbacks,
 * exceptions, being suspended and read from another thread? And does it stay out of what
 * should not inherit it? On a real CPU under Wine this is the baseline to match.
 *
 * Build: x86_64-w64-mingw32-clang -O1 -Wall nt_transitions_probe.c -o nt-transitions-probe.exe -lntdll
 * Same .exe on MacFinals and Proton; diff the output. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

#define NT 0x4000ull
NTSTATUS NTAPI NtYieldExecution(void);
NTSTATUS NTAPI NtDelayExecution(BOOLEAN, PLARGE_INTEGER);
NTSTATUS NTAPI NtQuerySystemTime(PLARGE_INTEGER);

static inline __attribute__((always_inline)) void nt_on(void) {
    __asm__ volatile("pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq" : : : "cc", "memory");
}
static inline __attribute__((always_inline)) unsigned long long read_flags(void) {
    unsigned long long v;
    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(v) : : "memory");
    return v;
}
static inline __attribute__((always_inline)) void nt_off(void) {
    __asm__ volatile("pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq" : : : "cc", "memory");
}
static void show(const char *name, unsigned long long flags) {
    printf("%-44s NT %d\n", name, flags & NT ? 1 : 0);
}

/* NT set, one call, NT read; cleared before printing, since printf is ordinary code */
#define ACROSS(name, call) do { unsigned long long f; nt_on(); call; f = read_flags(); nt_off(); show(name, f); } while (0)

static LONG CALLBACK resume_handler(EXCEPTION_POINTERS *e) {
    static unsigned long long seen;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    seen = e->ContextRecord->EFlags;
    e->ContextRecord->Rip = (ULONG_PTR)e->ExceptionRecord->ExceptionAddress + 1;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static LONG CALLBACK flags_in_handler(EXCEPTION_POINTERS *e) {
    /* what the handler itself runs with, not what the context says */
    unsigned long long live = read_flags();
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    printf("%-44s NT %d (context NT %d)\n", "live flags inside an exception handler", live & NT ? 1 : 0, e->ContextRecord->EFlags & NT ? 1 : 0);
    e->ContextRecord->Rip = (ULONG_PTR)e->ExceptionRecord->ExceptionAddress + 1;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static volatile LONG spin_state;
static unsigned long long thread_start_flags;
static DWORD WINAPI spinner(void *arg) {
    nt_on();
    spin_state = 1;
    while (spin_state == 1) { __asm__ volatile("pause"); }
    thread_start_flags = read_flags();
    nt_off();
    return 0;
}
static DWORD WINAPI start_reader(void *arg) { *(unsigned long long *)arg = read_flags(); return 0; }

static void CALLBACK apc_routine(ULONG_PTR arg) { *(unsigned long long *)arg = read_flags(); }
static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM a, LPARAM b) {
    if (m == WM_USER + 1) { *(unsigned long long *)a = read_flags(); return 0; }
    return DefWindowProcA(w, m, a, b);
}
static VOID CALLBACK fiber_main(void *arg) {
    *(unsigned long long *)arg = read_flags();
    SwitchToFiber(*(void **)((unsigned long long *)arg + 1));
}

int main(int argc, char **argv) {
    LARGE_INTEGER now, zero = {{0}};
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "child")) { printf("%-44s NT %d\n", "child process at main", read_flags() & NT ? 1 : 0); return 0; }

    printf("== across calls\n");
    ACROSS("no call", (void)0);
    ACROSS("NtQuerySystemTime (syscall through ntdll)", NtQuerySystemTime(&now));
    ACROSS("NtYieldExecution", NtYieldExecution());
    ACROSS("NtDelayExecution 0", NtDelayExecution(FALSE, &zero));
    ACROSS("Sleep(1)", Sleep(1));
    ACROSS("GetTickCount", GetTickCount());
    ACROSS("GetCurrentProcessId", GetCurrentProcessId());
    ACROSS("VirtualProtect-free TlsGetValue", TlsGetValue(0));
    ACROSS("GetModuleHandle", GetModuleHandleA("ntdll.dll"));
    ACROSS("CreateEvent", CloseHandle(CreateEventA(NULL, 0, 0, NULL)));
    ACROSS("GetSystemMetrics (win32u)", GetSystemMetrics(0));
    ACROSS("GetSystemMetrics again", GetSystemMetrics(0));
    ACROSS("GetDesktopWindow", GetDesktopWindow());
    ACROSS("GetKeyState", GetKeyState(0x10));
    ACROSS("GetForegroundWindow", GetForegroundWindow());
    ACROSS("GetCursorPos", ({ POINT p; GetCursorPos(&p); }));
    ACROSS("GetDC/ReleaseDC", ReleaseDC(NULL, GetDC(NULL)));
    ACROSS("LoadLibrary kernel32", LoadLibraryA("kernel32.dll"));
    ACROSS("LoadLibrary version (first load)", LoadLibraryA("version.dll"));
    ACROSS("VirtualAlloc", VirtualFree(VirtualAlloc(NULL, 0x1000, MEM_COMMIT, PAGE_READWRITE), 0, MEM_RELEASE));
    ACROSS("file open", CloseHandle(CreateFileA("NUL", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL)));
    ACROSS("QueryPerformanceCounter", QueryPerformanceCounter(&now));

    printf("== exceptions\n");
    {
        PVOID h = AddVectoredExceptionHandler(1, resume_handler);
        ACROSS("int3, handled, resumed", __asm__ volatile("int3" : : : "memory"));
        RemoveVectoredExceptionHandler(h);
        h = AddVectoredExceptionHandler(1, flags_in_handler);
        nt_on(); __asm__ volatile("int3" : : : "memory"); nt_off();
        RemoveVectoredExceptionHandler(h);
    }

    printf("== another thread looks\n");
    {
        CONTEXT c;
        HANDLE t = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
        while (!spin_state) Sleep(1);
        SuspendThread(t);
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(t, &c);
        printf("%-44s NT %d\n", "GetThreadContext of a suspended spinning thread", c.EFlags & NT ? 1 : 0);
        c.EFlags &= ~NT;
        SetThreadContext(t, &c);
        ResumeThread(t);
        Sleep(20);
        spin_state = 2;
        WaitForSingleObject(t, 3000);
        show("thread after SetThreadContext cleared NT", thread_start_flags);
        spin_state = 0;
        t = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
        while (!spin_state) Sleep(1);
        SuspendThread(t);
        ResumeThread(t);
        Sleep(20);
        spin_state = 2;
        WaitForSingleObject(t, 3000);
        show("thread after suspend and resume", thread_start_flags);
    }

    printf("== what should not inherit\n");
    {
        unsigned long long f = 0, slots[2] = {0};
        HANDLE t;
        nt_on(); t = CreateThread(NULL, 0, start_reader, &f, 0, NULL); nt_off();
        WaitForSingleObject(t, 3000);
        show("new thread started while creator had NT", f);
        nt_on(); QueueUserAPC(apc_routine, GetCurrentThread(), (ULONG_PTR)&f); SleepEx(0, TRUE); nt_off();
        show("APC delivered during alertable wait", f);
        {
            WNDCLASSA wc = {0};
            HWND w;
            wc.lpfnWndProc = wndproc; wc.lpszClassName = "nttransitions"; wc.hInstance = GetModuleHandleA(NULL);
            RegisterClassA(&wc);
            w = CreateWindowA("nttransitions", "", 0, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
            nt_on(); SendMessageA(w, WM_USER + 1, (WPARAM)&f, 0); nt_off();
            show("window procedure called through win32u", f);
            DestroyWindow(w);
        }
        {
            void *fiber;
            slots[1] = (unsigned long long)ConvertThreadToFiber(NULL);
            fiber = CreateFiber(0, fiber_main, slots);
            nt_on(); SwitchToFiber(fiber); nt_off();
            show("fiber entered while switcher had NT", slots[0]);
            DeleteFiber(fiber);
        }
        {
            char self[MAX_PATH], command[MAX_PATH + 16];
            STARTUPINFOA si = {sizeof si};
            PROCESS_INFORMATION pi;
            BOOL ok;
            GetModuleFileNameA(NULL, self, MAX_PATH);
            snprintf(command, sizeof command, "\"%s\" child", self);
            nt_on(); ok = CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi); nt_off();
            if (ok) { WaitForSingleObject(pi.hProcess, 15000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        }
    }
    return 0;
}
