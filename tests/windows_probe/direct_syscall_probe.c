/* What does a direct `syscall` instruction from Windows code cost, compared with calling the
 * ntdll export? Protected code often issues syscalls itself instead of calling ntdll. Wine's own
 * stubs never execute `syscall` (they call the dispatcher), so a direct one takes Wine's SIGSYS
 * path; under Rosetta that path goes through Rosetta's exception server.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/direct_syscall_probe.c \
 *     -o runtime/build/probe/direct-syscall-probe.exe
 *
 * The stub copies the Windows/Wine layout (mov r10,rcx; mov eax,N; syscall; ret), with a ret
 * also 0xb bytes after the syscall, where Wine's SIGSYS handler resumes.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef LONG (WINAPI *qpc_fn)(LARGE_INTEGER *, LARGE_INTEGER *);

int main(void)
{
    unsigned char *nt = (unsigned char *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryPerformanceCounter");
    if (!nt || nt[0] != 0x4c || nt[1] != 0x8b || nt[2] != 0xd1 || nt[3] != 0xb8)
    { printf("unexpected NtQueryPerformanceCounter stub\n"); return 1; }
    DWORD id; memcpy(&id, nt + 4, 4);
    printf("NtQueryPerformanceCounter syscall number %#lx\n", id);

    unsigned char *stub = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memset(stub, 0xcc, 64);
    unsigned char code[] = { 0x4c, 0x8b, 0xd1, 0xb8, 0, 0, 0, 0, 0x0f, 0x05, 0xc3 };
    memcpy(code + 4, &id, 4);
    memcpy(stub, code, sizeof(code));
    stub[0x0a + 0xb] = 0xc3;               /* where Wine's SIGSYS handler resumes */
    FlushInstructionCache(GetCurrentProcess(), stub, 64);

    qpc_fn exported = (qpc_fn)nt, direct = (qpc_fn)stub;
    LARGE_INTEGER c, f, t0, t1;
    QueryPerformanceFrequency(&f);
    LONG st = direct(&c, NULL);
    printf("direct syscall returned %#lx, counter %lld\n", st, c.QuadPart);
    if (st) return 1;

    for (int pass = 0; pass < 2; pass++)
    {
        qpc_fn fn = pass ? direct : exported;
        int n = pass ? 20000 : 2000000;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < n; i++) fn(&c, NULL);
        QueryPerformanceCounter(&t1);
        double s = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
        printf("%-30s %8d calls: %.3f s = %.0f ns per call\n",
               pass ? "direct `syscall` instruction" : "ntdll NtQueryPerformanceCounter", n, s, s * 1e9 / n);
    }
    return 0;
}
