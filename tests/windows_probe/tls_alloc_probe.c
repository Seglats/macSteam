/* TLS slot layout and high fixed-address reservations: MacFinals vs Windows vs Proton.
 * Wine on macOS may hold back some TEB TLS slots for macOS's own thread data, which would
 * change the first TlsAlloc index and the number of slots available. The anti-cheat image
 * is mapped at a fixed 0x700000000000, so this also tries reservations there and nearby.
 *
 * Build: x86_64-w64-mingw32-clang -O1 -Wall tls_alloc_probe.c -o tls-alloc-probe.exe
 * Run the same .exe on MacFinals, real Windows and Proton and compare the output. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <stdio.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    DWORD first[6];
    for (int i = 0; i < 6; i++) first[i] = TlsAlloc();
    printf("first TlsAlloc indices    %lu %lu %lu %lu %lu %lu\n",
           first[0], first[1], first[2], first[3], first[4], first[5]);
    int n = 6;
    while (TlsAlloc() != TLS_OUT_OF_INDEXES && n < 4096) n++;
    printf("TLS indices available     %d\n", n);

    DWORD f0 = FlsAlloc(NULL), f1 = FlsAlloc(NULL);
    printf("first FlsAlloc indices    %lu %lu\n", f0, f1);

    /* nonzero TEB TlsSlots before anything is stored = slots someone else uses */
    void **slots = (void **)((char *)NtCurrentTeb() + 0x1480);
    int used = 0;
    for (int i = 0; i < 64; i++) if (slots[i]) used++;
    printf("TEB TlsSlots nonzero      %d of 64 (before any TlsSetValue)\n", used);
    for (int i = 0; i < 64; i++) if (slots[i]) printf("    slot %d = %p\n", i, slots[i]);

    static const unsigned long long bases[] = {0x700000000000ull, 0x6ff000000000ull, 0x710000000000ull};
    static const SIZE_T sizes[] = {0x100000, 0x4000000, 0x40000000};
    for (int b = 0; b < 3; b++)
        for (int s = 0; s < 3; s++) {
            void *p = VirtualAlloc((void *)bases[b], sizes[s], MEM_RESERVE, PAGE_NOACCESS);
            printf("reserve %012llx size %08zx  %s", bases[b], sizes[s], p ? "ok" : "FAILED");
            if (!p) printf(" (%lu)", GetLastError());
            printf("\n");
            if (p) VirtualFree(p, 0, MEM_RELEASE);
        }

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void *)0x700000000000ull, &mbi, sizeof mbi))
        printf("query 700000000000        state %lx base %p region %zx\n",
               mbi.State, mbi.AllocationBase, mbi.RegionSize);
    return 0;
}
