/* Does Wine's idea of free address space match the host's? Wine's view tree does not know
 * every host mapping (dylibs, Rosetta, Metal), so memory it reports free may not be
 * allocatable. On Windows and Proton a region reported MEM_FREE can be reserved. This walks
 * the whole address space with VirtualQuery, tries to reserve at the start, middle and end of
 * each free region and on a fixed grid, then runs address searches: top-down, zero_bits and
 * VirtualAlloc2 address ranges.
 *
 * Build: x86_64-w64-mingw32-clang -O1 -Wall host_collision_probe.c -o host-collision-probe.exe -lntdll
 * Run "host-collision-probe.exe d3d" to create a D3D12 device first. Same .exe on MacFinals
 * and Proton. Addresses differ between hosts; the "free but refused" counts are what compares. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID *, PSIZE_T, ULONG);

#define GRANULE 0x10000ull
static const unsigned long long TOP = 0x7ffffffe0000ull;

static unsigned attempts, refused, listed;

/* The region is free according to Wine; try to take SIZE at ADDR with the NT call. */
static void try_reserve(unsigned long long addr, SIZE_T size, const char *where) {
    void *p = (void *)addr, *base = p;
    SIZE_T got = size, none = 0;
    NTSTATUS status = NtAllocateVirtualMemory(GetCurrentProcess(), &p, 0, &got, MEM_RESERVE, PAGE_NOACCESS);
    attempts++;
    if (status >= 0) {
        if (p != base) printf("  moved: asked %p got %p\n", base, p);
        NtFreeVirtualMemory(GetCurrentProcess(), &p, &none, MEM_RELEASE);
        return;
    }
    refused++;
    if (listed++ < 40) printf("  free but refused: %012llx size %8llx %-6s status %08lx\n", addr,
                              (unsigned long long)size, where, (unsigned long)status);
}

static int is_free(unsigned long long addr, unsigned long long *region_end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)addr, &info, sizeof info)) { *region_end = addr + GRANULE; return 0; }
    *region_end = (unsigned long long)info.BaseAddress + info.RegionSize;
    return info.State == MEM_FREE;
}

static void walk(void) {
    unsigned long long addr = GRANULE, end;
    unsigned regions = 0;
    attempts = refused = listed = 0;
    while (addr < TOP) {
        if (is_free(addr, &end)) {
            unsigned long long start = (addr + GRANULE - 1) & ~(GRANULE - 1), last = end & ~(GRANULE - 1);
            if (last > TOP) last = TOP;
            regions++;
            if (start + GRANULE <= last) {
                try_reserve(start, GRANULE, "start");
                try_reserve(last - GRANULE, GRANULE, "end");
                try_reserve((start + (last - start) / 2) & ~(GRANULE - 1), GRANULE, "middle");
            }
        }
        if (end <= addr) break;
        addr = end;
    }
    printf("walk: %u free regions, %u attempts, free but refused %u\n", regions, attempts, refused);
}

/* Every 1 GiB up to 16 GiB, then every 256 GiB; all 2^k 64-bit round addresses are hit too. */
static void grid(void) {
    unsigned long long addr, end;
    attempts = refused = listed = 0;
    for (addr = 0x40000000ull; addr < 0x400000000ull; addr += 0x40000000ull)
        if (is_free(addr, &end)) try_reserve(addr, 0x100000, "grid");
    for (addr = 0x4000000000ull; addr < TOP; addr += 0x4000000000ull)
        if (is_free(addr, &end)) try_reserve(addr, 0x100000, "grid");
    /* the top of the address space, where hosts tend to put things */
    for (addr = 0x7f0000000000ull; addr < TOP; addr += 0x10000000ull)
        if (is_free(addr, &end)) try_reserve(addr, 0x100000, "high");
    printf("grid: %u attempts, free but refused %u\n", attempts, refused);
}

static void report(const char *name, void *p, SIZE_T size, unsigned long error) {
    if (p) printf("%-44s ok %s\n", name, (unsigned long long)p >> 32 ? ">4G" : "<4G");
    else printf("%-44s FAILED %lu\n", name, error);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    (void)size;
}

static void searches(void) {
    static const SIZE_T sizes[] = {0x10000, 0x100000, 0x10000000, 0x40000000, 0x400000000ull};
    char name[80];
    for (unsigned i = 0; i < ARRAYSIZE(sizes); i++) {
        void *p;
        snprintf(name, sizeof name, "search size %llx", (unsigned long long)sizes[i]);
        p = VirtualAlloc(NULL, sizes[i], MEM_RESERVE, PAGE_NOACCESS);
        report(name, p, sizes[i], GetLastError());
        snprintf(name, sizeof name, "search size %llx top-down", (unsigned long long)sizes[i]);
        p = VirtualAlloc(NULL, sizes[i], MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
        report(name, p, sizes[i], GetLastError());
    }
    /* zero_bits as a count of high bits that must be zero, and as a mask */
    static const ULONG_PTR bits[] = {1, 8, 16, 21, 24, 0x7fffffff, 0x3fffffffff};
    for (unsigned i = 0; i < ARRAYSIZE(bits); i++) {
        void *p = NULL;
        SIZE_T size = 0x100000, none = 0;
        NTSTATUS status = NtAllocateVirtualMemory(GetCurrentProcess(), &p, bits[i], &size, MEM_RESERVE, PAGE_NOACCESS);
        printf("zero_bits %-12llx status %08lx %s\n", (unsigned long long)bits[i], (unsigned long)status, status >= 0 ? "ok" : "");
        if (status >= 0) NtFreeVirtualMemory(GetCurrentProcess(), &p, &none, MEM_RELEASE);
    }
}

/* VirtualAlloc2 with a required address range; a search that must honour limits */
typedef struct { void *low, *high; SIZE_T align; } requirement;
typedef struct { ULONG64 type; union { ULONG64 value; void *pointer; } u; } parameter;
typedef void *(WINAPI *va2_fn)(HANDLE, void *, SIZE_T, ULONG, ULONG, parameter *, ULONG);
static void ranges(void) {
    HMODULE k = GetModuleHandleA("kernelbase.dll");
    va2_fn va2 = k ? (va2_fn)(void *)GetProcAddress(k, "VirtualAlloc2") : NULL;
    if (!va2) { printf("VirtualAlloc2 not available\n"); return; }
    static const unsigned long long spans[][2] = {
        {0x000100000000ull, 0x00017fffffffull}, {0x000200000000ull, 0x0002ffffffffull},
        {0x000300000000ull, 0x000fffffffffull}, {0x010000000000ull, 0x01ffffffffffull},
        {0x600000000000ull, 0x6fffffffffffull}, {0x700000000000ull, 0x7effffffffffull},
        {0x7f0000000000ull, 0x7fefffffffffull}, {0x7ff000000000ull, 0x7ff7ffffffffull},
        {0x7ff800000000ull, 0x7fffffffffffull}};
    static const SIZE_T sizes[] = {0x10000, 0x1000000, 0x40000000};
    for (unsigned i = 0; i < ARRAYSIZE(spans); i++) for (unsigned s = 0; s < ARRAYSIZE(sizes); s++) {
        for (int top = 0; top < 2; top++) {
            requirement req = {(void *)spans[i][0], (void *)spans[i][1], 0};
            parameter par;
            void *p;
            par.type = 1 /* MemExtendedParameterAddressRequirements */;
            par.u.pointer = &req;
            p = va2(GetCurrentProcess(), NULL, sizes[s], MEM_RESERVE | (top ? MEM_TOP_DOWN : 0), PAGE_NOACCESS, &par, 1);
            printf("range %012llx-%012llx size %8llx %s %s", spans[i][0], spans[i][1], (unsigned long long)sizes[s],
                   top ? "down" : "up  ", p ? "ok" : "FAILED");
            if (p) {
                printf(" %s\n", (unsigned long long)p >= spans[i][0] && (unsigned long long)p + sizes[s] - 1 <= spans[i][1] ? "in range" : "OUT OF RANGE");
                VirtualFree(p, 0, MEM_RELEASE);
            } else printf(" %lu\n", GetLastError());
        }
    }
}

/* Searches that start where a host may have mappings. The search steps through occupied
 * memory, so what can differ is the cost; "timing" lines are not expected to match. */
static void search_costs(void) {
    HMODULE k = GetModuleHandleA("kernelbase.dll");
    va2_fn va2 = k ? (va2_fn)(void *)GetProcAddress(k, "VirtualAlloc2") : NULL;
    static const unsigned long long spans[][2] = {
        {0x000200000000ull, 0x003fffffffffull}, {0x7fb300000000ull, 0x7fefffffffffull},
        {0x010000000000ull, 0x7fa3ffffffffull}, {0x7ff000000000ull, 0x7ffa3fffffffull},
        {0x7ff800000000ull, 0x7ffffffeffffull}, {0x010000000000ull, 0x7ffa3fffffffull},
        {0x010000000000ull, 0x7ffffffeffffull}, {0x7ff100000000ull, 0x7ffffffeffffull}};
    LARGE_INTEGER freq, a, b;
    if (!va2) return;
    QueryPerformanceFrequency(&freq);
    for (unsigned i = 0; i < ARRAYSIZE(spans); i++) for (int top = 0; top < 2; top++) {
        requirement req = {(void *)spans[i][0], (void *)spans[i][1], 0};
        parameter par;
        void *p;
        par.type = 1;
        par.u.pointer = &req;
        QueryPerformanceCounter(&a);
        p = va2(GetCurrentProcess(), NULL, 0x10000, MEM_RESERVE | (top ? MEM_TOP_DOWN : 0), PAGE_NOACCESS, &par, 1);
        QueryPerformanceCounter(&b);
        printf("timing %012llx-%012llx %s %9.2f ms %s %p\n", spans[i][0], spans[i][1], top ? "down" : "up  ",
               (b.QuadPart - a.QuadPart) * 1e3 / freq.QuadPart, p ? "ok" : "FAILED", p);
        if (p) VirtualFree(p, 0, MEM_RELEASE);
    }
    /* ordinary top-down and bottom-up searches, as many callers make them */
    for (int top = 0; top < 2; top++) {
        void *p;
        QueryPerformanceCounter(&a);
        p = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | (top ? MEM_TOP_DOWN : 0), PAGE_NOACCESS);
        QueryPerformanceCounter(&b);
        printf("timing plain %s %9.2f ms %p\n", top ? "down" : "up  ", (b.QuadPart - a.QuadPart) * 1e3 / freq.QuadPart, p);
        if (p) VirtualFree(p, 0, MEM_RELEASE);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "d3d")) {
        static const GUID iid = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
        HMODULE d = LoadLibraryA("d3d12.dll");
        HRESULT (WINAPI *create)(void *, int, const GUID *, void **) = d ? (void *)GetProcAddress(d, "D3D12CreateDevice") : NULL;
        void *dev = NULL;
        HRESULT hr = create ? create(NULL, 0xb000, &iid, &dev) : E_FAIL;
        printf("d3d12 device %s (hr %08lx)\n", dev ? "created" : "NOT created", hr);
    }
    walk();
    grid();
    searches();
    ranges();
    search_costs();
    return 0;
}
