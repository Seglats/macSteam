/*
 * Cost of QueryPerformanceCounter, and what RDTSC looks like, in the current environment.
 * On Windows QPC is a user-mode TSC read; CrossOver's Wine makes every call a syscall
 * (NtQueryPerformanceCounter), and THE FINALS' game thread makes ~150k a second.
 *
 *   x86_64-w64-mingw32-gcc -O2 -Wall tests/windows_probe/qpc_probe.c -o runtime/build/probe/qpc-probe.exe
 */
#include <windows.h>
#include <stdio.h>
#include <intrin.h>

int main(void)
{
    LARGE_INTEGER f, a, b, c;
    unsigned long long t0, t1, prev, back = 0;
    int i, n = 2000000;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (i = 0; i < n; i++) QueryPerformanceCounter(&b);
    QueryPerformanceCounter(&c);
    printf("QPC frequency %lld Hz; %d calls took %.3f s = %.0f ns per call\n", f.QuadPart, n,
           (double)(c.QuadPart - a.QuadPart) / f.QuadPart, (double)(c.QuadPart - a.QuadPart) / f.QuadPart * 1e9 / n);

    QueryPerformanceCounter(&a); t0 = __rdtsc();
    Sleep(500);
    QueryPerformanceCounter(&b); t1 = __rdtsc();
    printf("RDTSC rate %.1f MHz (over %.3f s of QPC time)\n",
           (double)(t1 - t0) / ((double)(b.QuadPart - a.QuadPart) / f.QuadPart) / 1e6,
           (double)(b.QuadPart - a.QuadPart) / f.QuadPart);

    QueryPerformanceCounter(&a); prev = __rdtsc();
    for (i = 0; i < n; i++) { unsigned long long t = __rdtsc(); if (t < prev) back++; prev = t; }
    QueryPerformanceCounter(&c);
    printf("%d RDTSC reads took %.0f ns each, %llu went backwards\n", n,
           (double)(c.QuadPart - a.QuadPart) / f.QuadPart * 1e9 / n, back);
    {
        int r[4];
        __cpuid(r, 0x80000007);
        printf("CPUID 0x80000007 EDX bit 8 (invariant TSC): %d\n", (r[3] >> 8) & 1);
    }
    return 0;
}
