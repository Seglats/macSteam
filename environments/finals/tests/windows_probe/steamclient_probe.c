/* steamclient_probe.c — does steamclient64.dll answer through lsteamclient.dll? (2026-11-09)
 *
 * Checks wine-steamclient-lsteamclient.patch inside a prefix the steamdriver-finals tool has
 * staged (NotProton's bridge in C:\Program Files (x86)\Steam, WINEDLLPATH pointing there):
 * loads steamclient64.dll the way steam_api64.dll does, then looks at where its CreateInterface
 * export jumps. Redirected: a `movabs rax, <address inside lsteamclient.dll>; jmp rax`.
 * Exit 0 when redirected, 1 when steamclient64.dll loaded unchanged, 2 when it did not load.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall environments/finals/tests/windows_probe/steamclient_probe.c \
 *     -o runtime/build/probe/steamclient-probe.exe
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int inside(HMODULE mod, const void *p)
{
    const BYTE *base = (const BYTE *)mod;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    return (const BYTE *)p >= base && (const BYTE *)p < base + nt->OptionalHeader.SizeOfImage;
}

int main(void)
{
    HMODULE sc = LoadLibraryA("C:\\Program Files (x86)\\Steam\\steamclient64.dll");
    HMODULE bridge = GetModuleHandleA("lsteamclient.dll");
    const BYTE *code;
    void *target;

    printf("steamclient64.dll %p, lsteamclient.dll %p\n", (void *)sc, (void *)bridge);
    if (!sc) { printf("steamclient64.dll did not load (error %lu)\n", GetLastError()); return 2; }
    code = (const BYTE *)GetProcAddress(sc, "CreateInterface");
    if (!code) { printf("no CreateInterface export\n"); return 2; }
    printf("CreateInterface %p: %02x %02x ... %02x %02x\n", (void *)code, code[0], code[1], code[10], code[11]);
    if (code[0] == 0x48 && code[1] == 0xb8 && code[10] == 0xff && code[11] == 0xe0) {
        memcpy(&target, code + 2, sizeof(target));
        printf("jumps to %p, %s lsteamclient.dll\n", target, bridge && inside(bridge, target) ? "inside" : "NOT inside");
        return bridge && inside(bridge, target) ? 0 : 1;
    }
    printf("not redirected\n");
    return 1;
}
