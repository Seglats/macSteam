/* stuck_send_probe.c: checks the two crash diagnostics for a never-answered SendMessage
 * (crashes/C17): the win32u "no reply after ... ms" ERR line, and the scan-triggered
 * WINE_SYSCALL_LOG stuck-dump.
 *
 * The owner thread creates a hidden window and then blocks forever without pumping messages;
 * the main thread sends it WM_GETTEXT, which can then never be replied to. A poller thread
 * does 5.9 s timed waits the whole time: ordinary timed waits that must NOT be what triggers
 * the dump (the return-based trigger fired on exactly those). Expected, with
 * WINE_SYSCALL_LOG=<dir>:
 *
 *   - after 5 s, on stderr: "no reply after ... ms: still waiting for tid <owner>"
 *   - after 60 s, in <dir>/wine.<pid>: a stuck-dump; its body lists the MAIN thread in
 *     NtUserMessageCall (the sender) and the owner in NtWaitForSingleObject. The headline
 *     goes to a non-wait call once the scan prefers one, which lands on the sender at the
 *     second dump (~115 s; the first dump's headline is whichever thread crossed 60 s
 *     first) — hence the 140 s watchdog.
 *   - a sleeper thread blocks in a wait like any service thread, and the watchdog kills it
 *     mid-call at ~65 s (TerminateThread; the C17-043c shape: a thread dying inside an NT
 *     call). The second dump must then print "exited; never returned from
 *     NtWaitForSingleObject" for it — no growing duration, and it must not be the trigger
 *     or the headline of any later dump.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/stuck_send_probe.c -o stuck-send-probe.exe
 *   WINE_SYSCALL_LOG=/tmp/perf/wine wine stuck-send-probe.exe   # then look in /tmp/perf/wine.<pid>
 *
 * Exits by itself after about 140 s (watchdog).
 */
#include <windows.h>
#include <stdio.h>

static HWND victim;
static HANDLE never, ready, sleeper_handle;

static DWORD WINAPI owner(void *arg)
{
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = L"stuck_send_victim";
    RegisterClassW(&cls);
    victim = CreateWindowExW(0, cls.lpszClassName, L"victim", WS_OVERLAPPED,
                             0, 0, 10, 10, NULL, NULL, NULL, NULL);
    printf("owner thread %04lx created window %p, now blocks (never pumps messages)\n",
           GetCurrentThreadId(), victim);
    fflush(stdout);
    /* main must not send before the window exists: window creation goes through the
     * mac driver and can take longer than any fixed sleep (the first version's 500 ms
     * ran out, the send hit a NULL window and the probe proved nothing). */
    SetEvent(ready);
    WaitForSingleObject(never, INFINITE);
    return 0;
}

static DWORD WINAPI poller(void *arg)
{
    int i;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    printf("poller thread %04lx: 5.9 s timed waits (must not be the dump's trigger)\n",
           GetCurrentThreadId());
    fflush(stdout);
    for (i = 0; i < 13; i++) WaitForSingleObject(ev, 5900);
    return 0;
}

static DWORD WINAPI sleeper(void *arg)
{
    /* blocks like any service thread; the watchdog kills it mid-wait at ~65 s so the
     * dump must show it as exited, not "in NtWaitForSingleObject for N s" forever */
    printf("sleeper thread %04lx: blocks in a wait; watchdog kills it mid-call\n",
           GetCurrentThreadId());
    fflush(stdout);
    WaitForSingleObject(never, INFINITE);
    printf("sleeper: unreachable (TerminateThread does not return here)\n");
    return 0;
}

static DWORD WINAPI watchdog(void *arg)
{
    Sleep(65000);
    printf("watchdog: killing sleeper mid-wait (the C17 043c shape)\n");
    fflush(stdout);
    TerminateThread(sleeper_handle, 0);
    Sleep(75000);
    printf("watchdog: done\n");
    fflush(stdout);
    ExitProcess(0);
}

int main(void)
{
    char buf[64];
    never = CreateEventA(NULL, TRUE, FALSE, NULL);
    ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, owner, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, poller, NULL, 0, NULL));
    sleeper_handle = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
    CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    if (WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0)
    {
        printf("owner never created the window; aborting\n");
        return 1;
    }
    printf("main thread %04lx sends WM_GETTEXT (ERR after 5 s, stuck-dump after 60 s)\n",
           GetCurrentThreadId());
    fflush(stdout);
    SendMessageA(victim, WM_GETTEXT, sizeof(buf), (LPARAM)buf);
    printf("unexpected: got a reply\n");
    return 1;
}
