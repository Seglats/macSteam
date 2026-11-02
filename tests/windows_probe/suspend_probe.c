/*
 * Suspend and read the context of threads right after they start, over and over: what a
 * debugger, a crash reporter or a thread-scanning library does to a process. Wine handles
 * each suspension with SIGUSR1 in the target thread (usr1_handler). In THE FINALS a thread
 * caught this way was killed with "nested exception on signal stack" at usr1_handler's first
 * read of its signal context (ntdll.so+0x3b67c, 2026-11-02), and Steam's crash reporter hung
 * while suspending the game's threads.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/suspend_probe.c \
 *     -o runtime/build/probe/suspend-probe.exe
 *   suspend-probe.exe [rounds]
 *
 * Exit 0 when every round completes; a killed thread shows up as a wait that times out.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static volatile LONG started, finished;

static DWORD WINAPI worker(void *arg)
{
    volatile unsigned spin = 0;
    int i;
    InterlockedIncrement(&started);
    for (i = 0; i < 2000; i++)
    {
        spin++;
        if (!(i % 200)) SwitchToThread();          /* in and out of syscalls, like a new thread */
    }
    InterlockedIncrement(&finished);
    return 0;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 3000, r, bad = 0, ok_ctx = 0;
    for (r = 0; r < rounds; r++)
    {
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        CONTEXT ctx;
        int k;
        if (!t) { printf("CreateThread failed at round %d\n", r); return 2; }
        for (k = 0; k < 3; k++)                      /* hit it while it is still starting */
        {
            if (SuspendThread(t) == (DWORD)-1) break;
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t, &ctx)) ok_ctx++;
            ResumeThread(t);
        }
        if (WaitForSingleObject(t, 5000) != WAIT_OBJECT_0)
        {
            printf("round %d: thread did not finish within 5 s\n", r);
            if (++bad >= 5) break;
        }
        else
        {
            DWORD code = 0;
            GetExitCodeThread(t, &code);
            if (code) { printf("round %d: thread exit code %lu (killed?)\n", r, code); bad++; }
        }
        CloseHandle(t);
        if (!(r % 500)) { printf("round %d: %ld started, %ld finished, %d contexts read\n", r, started, finished, ok_ctx); fflush(stdout); }
    }
    printf("done: %d rounds, %ld started, %ld finished, %d contexts read, %d bad\n", r, started, finished, ok_ctx, bad);
    return bad ? 1 : 0;
}
