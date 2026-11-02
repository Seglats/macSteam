/* Does every way of creating executable memory keep TEB semantics through gs, at scale?
 * Follows gsregion_probe.c, for what it does not cover: an image section mapped by hand,
 * many separate regions, free and re-allocate at one address, the cost of flipping
 * protections, and TLS slot reads through gs, which compilers inline.
 *
 * Build: x86_64-w64-mingw32-clang -O1 -Wall exec_region_probe.c -o exec-region-probe.exe -lntdll
 * Run the same .exe on MacFinals, Windows and Proton. Run "exec-region-probe.exe d3d" to
 * create a D3D12 device first. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NTSTATUS NTAPI NtMapViewOfSection(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);

/* void f(void *out): out[0]=gs:[0x48] tid, out[1]=gs:[0x08] StackBase */
static const unsigned char gsread[] = {
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00,  /* mov rax, gs:[0x48] */
    0x48, 0x89, 0x01,                                      /* mov [rcx], rax     */
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x08, 0x00, 0x00, 0x00,  /* mov rax, gs:[0x08] */
    0x48, 0x89, 0x41, 0x08,                                /* mov [rcx+8], rax   */
    0xc3,                                                  /* ret                */
};
typedef void (*gsfn)(void *);

__attribute__((noinline, section(".text$gsprobe"))) static void gs_in_image(void **out) {
    __asm__ volatile("mov %%gs:0x48,%0" : "=r"(out[0]));
    __asm__ volatile("mov %%gs:0x08,%0" : "=r"(out[1]));
}

static int failures;
static void check(const char *name, gsfn f) {
    void *got[2] = {0};
    unsigned char *teb = (unsigned char *)NtCurrentTeb();
    int same;
    f(got);
    same = got[0] == *(void **)(teb + 0x48) && got[1] == *(void **)(teb + 0x08);
    printf("%-46s %s\n", name, same ? "same" : "DIFF");
    if (!same) failures++;
}

static void *new_rx(void *at) {
    DWORD old;
    void *p = VirtualAlloc(at, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return NULL;
    memcpy(p, gsread, sizeof gsread);
    return VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old) ? p : NULL;
}

static void hand_mapped_image(void) {
    WCHAR path[MAX_PATH];
    SIZE_T size = 0;
    void *view = NULL;
    NTSTATUS status;
    HANDLE file, section;
    GetModuleFileNameW(NULL, path, MAX_PATH);
    file = CreateFileW(path, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    section = CreateFileMappingW(file, NULL, PAGE_EXECUTE_READ | SEC_IMAGE, 0, 0, NULL);
    if (!section) { printf("hand-mapped image: no section (%lu)\n", GetLastError()); failures++; return; }
    status = NtMapViewOfSection(section, GetCurrentProcess(), &view, 0, 0, NULL, &size, 1 /* ViewShare */, 0, PAGE_EXECUTE_READ);
    if (status < 0) { printf("hand-mapped image: status %08lx\n", (unsigned long)status); failures++; return; }
    /* the routine is position independent and has no relocations */
    check("image section mapped by hand",
          (gsfn)((char *)view + ((char *)gs_in_image - (char *)GetModuleHandleW(NULL))));
}

static void many_regions(int count) {
    void *first = NULL, *last = NULL;
    char name[64];
    LARGE_INTEGER a, b, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < count; i++) {
        void *p = new_rx(NULL);
        if (!p) { printf("allocation %d failed (%lu)\n", i, GetLastError()); failures++; return; }
        if (!first) first = p;
        last = p;
    }
    QueryPerformanceCounter(&b);
    snprintf(name, sizeof name, "first of %d regions", count);
    check(name, (gsfn)first);
    snprintf(name, sizeof name, "last of %d regions", count);
    check(name, (gsfn)last);
    printf("  %.1f us per alloc + protect\n", (b.QuadPart - a.QuadPart) * 1e6 / freq.QuadPart / count);
}

static void reuse(void) {
    void *p = new_rx(NULL), *q;
    if (!p) { failures++; return; }
    check("before free", (gsfn)p);
    VirtualFree(p, 0, MEM_RELEASE);
    q = new_rx(p);
    if (q != p) { printf("re-allocate at %p failed: got %p (%lu)\n", p, q, GetLastError()); failures++; return; }
    check("free, then re-allocate at the same address", (gsfn)q);
}

static void flips(void) {
    void *p = new_rx(NULL);
    DWORD old;
    LARGE_INTEGER a, b, freq;
    enum { count = 100000 };
    if (!p) { failures++; return; }
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < count; i++) {
        VirtualProtect(p, 0x1000, PAGE_READWRITE, &old);
        VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old);
    }
    QueryPerformanceCounter(&b);
    printf("RW <-> RX flips: %.2f us per VirtualProtect\n", (b.QuadPart - a.QuadPart) * 1e6 / freq.QuadPart / (2 * count));
    check("after 200000 protection changes", (gsfn)p);
}

/* inlined TlsGetValue: gs:[0x1480 + 8*index], from this image and from dynamic code */
static void tls_slots(void) {
    /* mov rax, gs:[imm32]; ret, with rcx ignored */
    unsigned char code[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0, 0xc3};
    DWORD index = TlsAlloc(), old, offset;
    void *value = (void *)0x1234567812345678ull, *inlined = NULL, *dynamic, *p;
    if (index == TLS_OUT_OF_INDEXES) { failures++; return; }
    printf("first TlsAlloc in this test                    %lu\n", index);
    if (index >= 64) { printf("  no slot below 64: TLS reads through gs not tested\n"); return; }
    TlsSetValue(index, value);
    offset = 0x1480 + 8 * index;
    __asm__ volatile("movq %%gs:(%1),%0" : "=r"(inlined) : "r"((unsigned long long)offset));
    printf("%-46s %s\n", "TLS slot through gs, this image", inlined == value ? "same" : "DIFF");
    if (inlined != value) failures++;
    memcpy(code + 5, &offset, 4);
    p = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memcpy(p, code, sizeof code);
    VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old);
    dynamic = ((void *(*)(void))p)();
    printf("%-46s %s\n", "TLS slot through gs, dynamic code", dynamic == value ? "same" : "DIFF");
    if (dynamic != value) failures++;
    printf("%-46s %s\n", "TlsGetValue agrees", TlsGetValue(index) == value ? "same" : "DIFF");
    if (TlsGetValue(index) != value) failures++;
}

/* how many separate regions keep working: new regions until one reads DIFF */
static void capacity(int limit) {
    unsigned char *teb = (unsigned char *)NtCurrentTeb();
    for (int i = 1; i <= limit; i++) {
        void *got[2] = {0}, *p = new_rx(NULL);
        if (!p) { printf("allocation %d failed (%lu)\n", i, GetLastError()); return; }
        ((gsfn)p)(got);
        if (got[0] != *(void **)(teb + 0x48)) { printf("region %d at %p reads DIFF\n", i, p); return; }
        if (!(i % 10000)) printf("  %d ok\n", i);
    }
    printf("all %d regions same\n", limit);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2 && !strcmp(argv[1], "capacity")) { capacity(atoi(argv[2])); return 0; }
    if (argc > 1 && !strcmp(argv[1], "d3d")) {
        static const GUID iid = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
        HMODULE d = LoadLibraryA("d3d12.dll");
        HRESULT (WINAPI *create)(void *, int, const GUID *, void **) = d ? (void *)GetProcAddress(d, "D3D12CreateDevice") : NULL;
        void *dev = NULL;
        HRESULT hr = create ? create(NULL, 0xb000, &iid, &dev) : E_FAIL;
        printf("d3d12 device %s (hr %08lx)\n", dev ? "created" : "NOT created", hr);
    }
    tls_slots();
    hand_mapped_image();
    reuse();
    flips();
    many_regions(1000);
    many_regions(20000);
    printf("== %d failures\n", failures);
    return failures != 0;
}
