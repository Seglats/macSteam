/* busy_probe.c — a known Windows call chain for stack-sampler tests (2026-11-09).
 *
 * main -> outer_work -> inner_work spins for N seconds (default 15) and prints the three
 * function addresses, so a sample of the Wine process can be checked against them: does
 * the sampler show the Windows code (PE addresses) at all, and with which frames?
 *
 *   x86_64-w64-mingw32-gcc -O1 -fno-omit-frame-pointer -Wall \
 *     tests/windows_probe/busy_probe.c -o runtime/build/probe/busy-probe.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static volatile unsigned long long sink;

__attribute__((noinline)) static void inner_work(unsigned long n)
{
    for (unsigned long i = 0; i < n; i++) sink += i * 2654435761u;
}

__attribute__((noinline)) static void outer_work(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) inner_work(1000000);
}

int main(int argc, char **argv)
{
    DWORD secs = argc > 1 ? (DWORD)atoi(argv[1]) : 15;
    printf("busy-probe: main %p outer_work %p inner_work %p, %lu s\n",
           (void *)main, (void *)outer_work, (void *)inner_work, (unsigned long)secs);
    fflush(stdout);
    outer_work(secs * 1000);
    printf("busy-probe: done\n");
    return 0;
}
