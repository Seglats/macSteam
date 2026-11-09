/* Read Wine's fixed-address dispatcher page (0x7ffe1000/0x7ffe1008) from inside a
 * Windows process. Slot 0 is upstream Wine (__wine_syscall_dispatcher); slot 1 is the
 * MacFinals publish patch (prolog_end). Prints both so a runtime can be checked
 * without task_for_pid.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/read_dispatch_page.c \
 *     -o runtime/build/probe/read-dispatch-page.exe
 */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    unsigned long long *page = (unsigned long long *)0x7ffe1000;
    if (IsBadReadPtr(page, 16))
    {
        printf("0x7ffe1000 is NOT readable\n");
        return 1;
    }
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    MEMORY_BASIC_INFORMATION mbi;
    size_t nt_size = 0;
    if (nt && VirtualQuery(nt, &mbi, sizeof mbi))
        nt_size = (char *)mbi.BaseAddress + mbi.RegionSize - (char *)nt; /* first region only */
    unsigned long long nt_base = (unsigned long long)nt;
    printf("ntdll base      %llx (first-region end %llx)\n",
           nt_base, nt_base + (unsigned long long)nt_size);
    printf("dispatcher      %llx  in-ntdll-first-region: %d\n",
           (unsigned long long)page[0],
           page[0] >= nt_base && page[0] < nt_base + nt_size);
    printf("prolog_end      %llx  in-ntdll-first-region: %d\n",
           (unsigned long long)page[1],
           page[1] >= nt_base && page[1] < nt_base + nt_size);
    printf("sanity: slots differ: %d, both canonical-ish (<0x800000000000): %d\n",
           page[0] != page[1], page[0] < 0x800000000000ULL && page[1] < 0x800000000000ULL);
    return 0;
}
