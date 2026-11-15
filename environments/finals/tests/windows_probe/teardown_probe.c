/* teardown_probe.c — fast reproducer for THE FINALS' start-up crash
 * ("rosetta error: no code fragment associated with the given arm pc").
 *
 * In the game the crash comes at the moment a process is torn down (the launcher handing over to
 * the game): wineserver sends SIGQUIT to every thread of the dying process at once
 * ("nested exception on signal stack ... 0xa8" lines right before it, also in good launches).
 * The suspect is a signal landing on a thread that is inside the NT hook's syscall fast path,
 * an ARM pc Rosetta cannot map back to x86. This probe makes that moment happen over and over:
 *
 *   parent:  starts the child ROUNDS times, one after the other, and reports each exit.
 *   child:   THREADS threads issue the game's invalid-class syscall shape (anon_invalid_site.c
 *            site1, the shape the fast path engages on) back to back; after a random 300-700 ms
 *            the main thread exits the process while they are still at it.
 *   mode "suspend": before exiting, the main thread suspends and reads the context of the
 *            busy threads 50 times over (SIGUSR1 in each target, what crash reporters and
 *            anti-cheats do).
 *   mode "mixed": suspensions as "suspend", against workers that also block in waits, take a
 *            shared critical section, and run beside a timer-queue and a winmm timer; then
 *            ExitProcess with all of it live (the exit hang of 2026-11-14).
 *   mode "waitexit": workers make Wine's own syscalls (SwitchToThread) under the suspend storm,
 *            then block in an infinite wait; each must still answer a suspension, and the
 *            exit must finish.
 *   mode "suspendonly": the same, then stops and joins the threads before exiting, so only
 *            the suspensions can hit.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/teardown_probe.c \
 *     -o runtime/build/probe/teardown-probe.exe -lwinmm
 *   teardown-probe.exe [rounds] [threads] [exit|suspend|suspendonly|mixed|waitexit]
 *
 * Runner: tests/finals_bisect/teardown_check.py (counts the Rosetta aborts in Wine's stderr; A/B
 * with MACFINALS_NT_OFF=syscall).
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG running, stop;

static void site1(unsigned long long *sink)
{
    /* Same shape as anon_invalid_site.c site1: rax=-1, markers in rdi. Wine returns
     * STATUS_INVALID_SYSTEM_SERVICE (c000001c) through its dispatcher, stock path or fast path. */
    __asm__ volatile(
        "movabs $0x1122334455667788, %%r8\n\t"
        "movq   %%r8, (%%rdi)\n\t"
        "movq   %%rdx, 24(%%rdi)\n\t"
        "movl   $0xffffffff, %%eax\n\t"
        "syscall\n\t"
        "movabs $0x8877665544332211, %%r8\n\t"
        "movq   %%r8, 8(%%rdi)\n\t"
        "movq   %%rax, 16(%%rdi)\n\t"
        "movq   %%rdx, 32(%%rdi)\n\t"
        : : "D"(sink) : "r8", "rax", "rdx", "rcx", "r11", "memory");
}

static DWORD WINAPI worker(void *arg)
{
    unsigned long long sink[5];
    InterlockedIncrement(&running);
    while (!stop) site1(sink);
    return 0;
}

/* mode "mixed": closer to the game at quit. Workers alternate bursts of the fast-path
 * syscall with blocking waits on an event, a shared critical section, and Sleep; a
 * timer-queue timer (threadpool/timerqueue locks) and a winmm multimedia timer (WINMM_cs)
 * keep firing. THE FINALS hung on exit on macfinals-113 with those locks held. */
static CRITICAL_SECTION shared_cs;
static HANDLE kick;
static volatile LONG ticks;

static DWORD WINAPI mixed_worker(void *arg)
{
    unsigned long long sink[5];
    unsigned n = 0;
    InterlockedIncrement(&running);
    while (!stop)
    {
        for (int i = 0; i < 200; i++) site1(sink);
        switch (n++ % 4)
        {
        case 0: WaitForSingleObject(kick, 2); break;              /* blocking wait, times out */
        case 1: EnterCriticalSection(&shared_cs); site1(sink); LeaveCriticalSection(&shared_cs); break;
        case 2: SetEvent(kick); break;
        case 3: Sleep(0); break;
        }
    }
    return 0;
}

/* mode "waitexit": workers make Wine's own syscalls back to back (SwitchToThread), which
 * pass the hook's guard and bail to the stock path, while the main thread suspends them;
 * then they block in an infinite wait. A signal deferred in the guard of the syscall that
 * blocks would leave the thread asleep with every signal blocked: the final
 * GetThreadContext (SIGUSR1) or the exit (SIGQUIT) can then never reach it. */
static volatile LONG park;
static HANDLE never;

static DWORD WINAPI wait_worker(void *arg)
{
    InterlockedIncrement(&running);
    while (!park) SwitchToThread();
    WaitForSingleObject(never, INFINITE);
    return 0;
}

static VOID CALLBACK queue_timer(void *ctx, BOOLEAN fired)
{
    unsigned long long sink[5];
    EnterCriticalSection(&shared_cs);
    site1(sink);
    InterlockedIncrement(&ticks);
    LeaveCriticalSection(&shared_cs);
}

static void CALLBACK mm_timer(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR a, DWORD_PTR b)
{
    unsigned long long sink[5];
    site1(sink);
    InterlockedIncrement(&ticks);
}

static int child(int threads, int suspend)
{
    HANDLE th[64];
    unsigned long long sink[5] = {0};
    LARGE_INTEGER f, a, b;
    int i, n = 0;

    srand(GetCurrentProcessId() * 2654435761u ^ GetTickCount());
    /* Warm the site on the main thread first and time it: about 30 ns a call means the fast
     * path is engaged, 15-20 us is the stock SIGSYS path. */
    if (!getenv("TEARDOWN_NOWARM")) for (i = 0; i < 2000; i++) site1(sink);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    if (!getenv("TEARDOWN_NOWARM")) for (i = 0; i < 20000; i++) site1(sink);
    QueryPerformanceCounter(&b);
    printf("child %lu: %.0f ns per syscall, rax %llx\n", GetCurrentProcessId(),
           (double)(b.QuadPart - a.QuadPart) * 1e9 / f.QuadPart / 20000, sink[2]);
    fflush(stdout);

    if (suspend == 3)
    {
        HANDLE timer;
        InitializeCriticalSection(&shared_cs);
        kick = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateTimerQueueTimer(&timer, NULL, queue_timer, NULL, 1, 1, WT_EXECUTEDEFAULT);
        timeBeginPeriod(1);
        timeSetEvent(1, 0, mm_timer, 0, TIME_PERIODIC);
    }
    for (i = 0; i < threads && i < 64; i++)
    {
        LPTHREAD_START_ROUTINE fn = suspend == 3 ? mixed_worker : suspend == 4 ? wait_worker : worker;
        if (suspend == 4 && !never) never = CreateEventA(NULL, TRUE, FALSE, NULL);
        if ((th[n] = CreateThread(NULL, 0, fn, NULL, 0, NULL))) n++;
    }
    while (running < n) Sleep(1);
    Sleep(300 + rand() % 400);

    if (suspend)
    {
        CONTEXT ctx;
        int round;
        for (round = 0; round < 50; round++)
            for (i = 0; i < n; i++)
            {
                if (getenv("TEARDOWN_TRACE")) fprintf(stderr, "suspend %d.%d\n", round, i);
                if (SuspendThread(th[i]) == (DWORD)-1) continue;
                if (getenv("TEARDOWN_TRACE")) fprintf(stderr, "context %d.%d\n", round, i);
                ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(th[i], &ctx);
                if (getenv("TEARDOWN_TRACE")) fprintf(stderr, "resume %d.%d rip %llx\n", round, i, ctx.Rip);
                ResumeThread(th[i]);
            }
        if (suspend == 4)   /* "waitexit": park them in the infinite wait, then each must
                             * still answer one suspension */
        {
            park = 1;
            Sleep(100);
            for (i = 0; i < n; i++)
            {
                if (getenv("TEARDOWN_TRACE")) fprintf(stderr, "parked suspend %d\n", i);
                SuspendThread(th[i]);
                ctx.ContextFlags = CONTEXT_FULL;
                GetThreadContext(th[i], &ctx);
                ResumeThread(th[i]);
            }
            if (getenv("TEARDOWN_TRACE")) fprintf(stderr, "parked threads all answered\n");
        }
        if (suspend == 2)   /* "suspendonly": stop and join the workers first, so only the
                             * suspensions can have hit the stub */
        {
            stop = 1;
            if (getenv("TEARDOWN_TRACE")) fprintf(stderr, "joining\n");
            if (WaitForMultipleObjects(n, th, TRUE, 10000) == WAIT_TIMEOUT)
                fprintf(stderr, "join timed out\n");
            else if (getenv("TEARDOWN_TRACE")) fprintf(stderr, "joined\n");
        }
    }
    if (getenv("TEARDOWN_TRACE")) fprintf(stderr, "exiting (%ld timer ticks)\n", ticks);
    ExitProcess(0);   /* the other threads are still in their loop: SIGQUIT lands mid-syscall */
}

int main(int argc, char **argv)
{
    int rounds = 20, threads = 16, suspend = 0, i, bad = 0;
    char self[MAX_PATH], cmd[MAX_PATH + 64];

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "child"))
        return child(argc > 2 ? atoi(argv[2]) : 16, argc > 3 ? (!strcmp(argv[3], "suspend") ? 1 : !strcmp(argv[3], "suspendonly") ? 2 : !strcmp(argv[3], "mixed") ? 3 : !strcmp(argv[3], "waitexit") ? 4 : 0) : 0);
    if (argc > 1) rounds = atoi(argv[1]);
    if (argc > 2) threads = atoi(argv[2]);
    if (argc > 3) suspend = !strcmp(argv[3], "suspend") ? 1 : !strcmp(argv[3], "suspendonly") ? 2 : !strcmp(argv[3], "mixed") ? 3 : !strcmp(argv[3], "waitexit") ? 4 : 0;

    GetModuleFileNameA(NULL, self, sizeof(self));
    snprintf(cmd, sizeof(cmd), "\"%s\" child %d %s", self, threads, suspend == 4 ? "waitexit" : suspend == 3 ? "mixed" : suspend == 2 ? "suspendonly" : suspend ? "suspend" : "exit");
    for (i = 0; i < rounds; i++)
    {
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        DWORD code = 0, wait;
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        { printf("round %d: CreateProcess failed %lu\n", i + 1, GetLastError()); return 2; }
        wait = WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &code);
        if (wait != WAIT_OBJECT_0 || code) bad++;
        printf("round %d: %s, exit code %#lx\n", i + 1, wait == WAIT_OBJECT_0 ? "exited" : "HUNG", code);
        if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    printf("%d of %d rounds ended abnormally\n", bad, rounds);
    return bad ? 1 : 0;
}
