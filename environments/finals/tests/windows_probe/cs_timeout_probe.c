/* cs_timeout_probe.c: checks the WINE_SYSCALL_LOG "stuck-dump" (crashes/C16).
 *
 * A worker takes a critical section and blocks forever in WaitForSingleObject; the main thread
 * then tries to enter it. Wine's RtlpWaitForCriticalSection times out after 5 s and again after
 * 60 s; on the 60 s timeout the counters dump every thread's current NT call. The dump should
 * name the worker's thread id (printed below) as "in NtWaitForSingleObject".
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/cs_timeout_probe.c -o cs-timeout-probe.exe
 *   WINE_SYSCALL_LOG=/tmp/perf/wine wine cs-timeout-probe.exe   # then look in /tmp/perf/wine.<pid>
 *
 * Exits by itself after about 70 s (the main thread gives up via a watchdog).
 */
#include <windows.h>
#include <stdio.h>

static CRITICAL_SECTION cs;
static HANDLE never;

static DWORD WINAPI owner(void *arg)
{
    EnterCriticalSection(&cs);
    printf("owner thread %04lx holds the section, now waits forever\n", GetCurrentThreadId());
    fflush(stdout);
    WaitForSingleObject(never, INFINITE);
    return 0;
}

static DWORD WINAPI quitter(void *arg)
{
    /* exits long before the dump: its slot must render as one "exited" line, not as
     * "in NtTerminateThread" (the -119 dump listed ~150 threads that way). */
    printf("quitter thread %04lx exits immediately\n", GetCurrentThreadId());
    fflush(stdout);
    return 0;
}

static DWORD WINAPI watchdog(void *arg)
{
    Sleep(72000);
    printf("watchdog: done\n");
    fflush(stdout);
    ExitProcess(0);
}

int main(void)
{
    InitializeCriticalSection(&cs);
    never = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, quitter, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, quitter, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, owner, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    Sleep(500);
    printf("main thread %04lx enters the section (times out after 5 s, then 60 s)\n", GetCurrentThreadId());
    fflush(stdout);
    EnterCriticalSection(&cs);
    printf("unexpected: got the section\n");
    return 1;
}
