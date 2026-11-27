/*
 * SteamDriver nvapi64 layer: Apple's NVAPI (Game Porting Toolkit, nvapi64.dll, loaded unchanged
 * as nvapi64_gptk.dll) with one correction, the driver version.
 *
 * With MetalFX upscaling on, D3DMetal presents an NVIDIA GPU and registers its driver version
 * in the display class key (32.0.16.1656, i.e. NVIDIA 616.56; branch r616_41), and DXGI and
 * Direct3D 9 report that one. Apple's NVAPI alone answers NvAPI_SYS_GetDriverAndBranchVersion
 * and NvAPI_GetDisplayDriverVersion with 576.02. Deadlock's minimum is 581.08: with 576.02 it
 * turns asynchronous shader creation off and doesn't offer DLSS, although Apple's NGX reports
 * DLSS available (tests/windows_probe/dlss_probe.c). This layer answers those two queries with
 * the registered version, so all of D3DMetal's sources agree. Everything else, including every
 * other NVAPI function and nvapi_Direct_GetMethod, is Apple's, unchanged.
 *
 * The version is taken only when exactly one display adapter is registered and its provider is
 * NVIDIA; otherwise Apple's answer stays as it is. The compatibility tool loads NVAPI only in
 * upscaling launches (compat-tool/run), so the layer exists in no other launch.
 *
 * Built by build.sh with MinGW-w64 GCC and marked as a Wine builtin, like d3d12_layer.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#define ID_SYS_GetDriverAndBranchVersion 0x2926aaad
#define ID_GetDisplayDriverVersion       0xf951a4d1
#define NVAPI_OK 0

typedef void *(__cdecl *query_fn)(unsigned int);
typedef int (__cdecl *driver_branch_fn)(unsigned int *, char *);

/* NV_DISPLAY_DRIVER_VERSION (nvapi.h), version 1. */
typedef struct
{
    unsigned int version;
    unsigned int drvVersion;
    unsigned int bldChangeListNum;
    char szBuildBranchString[64];
    char szAdapterString[64];
} display_driver_version;
typedef int (__cdecl *display_driver_fn)(void *, display_driver_version *);

/* ---- debug output: Wine's channel model (as d3d12_layer/layer.c), channel macfinals_nv ----
 * warn: an NVAPI function a game asks for that Apple's NVAPI doesn't have (once per function);
 * trace: every function asked for, and the corrected driver versions. */
struct dbg_channel { unsigned char flags; char name[15]; };
enum { DBCL_FIXME, DBCL_ERR, DBCL_WARN, DBCL_TRACE };
static struct dbg_channel dbch = { 0xff, "macfinals_nv" };
static unsigned char (__cdecl *p_dbg_get_channel_flags)(struct dbg_channel *);
static int (__cdecl *p_dbg_header)(int, struct dbg_channel *, const char *);
static int (__cdecl *p_dbg_output)(const char *);

static int dbg_on(int cls)
{
    if (!p_dbg_get_channel_flags)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        p_dbg_header = (void *)GetProcAddress(ntdll, "__wine_dbg_header");
        p_dbg_output = (void *)GetProcAddress(ntdll, "__wine_dbg_output");
        p_dbg_get_channel_flags = (void *)GetProcAddress(ntdll, "__wine_dbg_get_channel_flags");
        if (!p_dbg_get_channel_flags) return 0;
    }
    return !!(p_dbg_get_channel_flags(&dbch) & (1 << cls));
}

static void __attribute__((format(printf, 3, 4))) dbg_printf(int cls, const char *function, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (p_dbg_header && p_dbg_output && p_dbg_header(cls, &dbch, function) != -1) p_dbg_output(buf);
}
#define WARN(...)  do { if (dbg_on(DBCL_WARN))  dbg_printf(DBCL_WARN,  __func__, __VA_ARGS__); } while (0)
#define TRACE(...) do { if (dbg_on(DBCL_TRACE)) dbg_printf(DBCL_TRACE, __func__, __VA_ARGS__); } while (0)

/* Function ids already reported as missing, so each is warned about once. */
static unsigned int missing[64];
static LONG missing_count;
static void report_missing(unsigned int id)
{
    LONG i, n = missing_count;
    for (i = 0; i < n && i < 64; i++) if (missing[i] == id) return;
    if (n < 64) { missing[n] = id; InterlockedIncrement(&missing_count); }
    WARN("NVAPI function %#x asked for; Apple's NVAPI doesn't have it\n", id);
}

static query_fn real_query;
static driver_branch_fn real_driver_branch;
static display_driver_fn real_display_driver;
static unsigned int registered;     /* NVIDIA version * 100 (61656 for 616.56), 0 = none */
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

/* The NVIDIA driver number of the display driver's registered version: the last five digits of
 * the Windows version (32.0.16.1656 -> 6 and 1656 -> 61656, i.e. 616.56). */
static unsigned int registry_nvidia_version(void)
{
    static const WCHAR class_key[] = L"System\\CurrentControlSet\\Control\\Class\\"
            L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    WCHAR name[16], provider[128], version[64];
    unsigned int a, b, c, d, found = 0, adapters = 0;
    HKEY class, adapter;
    DWORD size, i;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_key, 0, KEY_READ, &class))
        return 0;
    for (i = 0; !RegEnumKeyW(class, i, name, ARRAYSIZE(name)); ++i)
    {
        if (RegOpenKeyExW(class, name, 0, KEY_READ, &adapter))
            continue;
        size = sizeof(version);
        if (!RegQueryValueExW(adapter, L"DriverVersion", NULL, NULL, (BYTE *)version, &size))
        {
            ++adapters;
            size = sizeof(provider);
            if (!RegQueryValueExW(adapter, L"ProviderName", NULL, NULL, (BYTE *)provider, &size)
                    && !wcsncmp(provider, L"NVIDIA", 6)
                    && swscanf(version, L"%u.%u.%u.%u", &a, &b, &c, &d) == 4 && d < 10000)
                found = (c % 10) * 10000 + d;
        }
        RegCloseKey(adapter);
    }
    RegCloseKey(class);
    return adapters == 1 ? found : 0;
}

static BOOL CALLBACK init(INIT_ONCE *o, void *param, void **context)
{
    HMODULE gptk = LoadLibraryW(L"nvapi64_gptk.dll");
    if (gptk && gptk != GetModuleHandleW(L"nvapi64.dll"))
        real_query = (query_fn)GetProcAddress(gptk, "nvapi_QueryInterface");
    registered = registry_nvidia_version();
    return TRUE;
}

static int __cdecl driver_branch(unsigned int *version, char *branch)
{
    int r = real_driver_branch(version, branch);
    if (r == NVAPI_OK && version && registered)
    {
        TRACE("driver version %u -> %u\n", *version, registered);
        *version = registered;
    }
    return r;
}

static int __cdecl display_driver(void *display, display_driver_version *out)
{
    int r = real_display_driver(display, out);
    if (r == NVAPI_OK && out && registered) out->drvVersion = registered;
    return r;
}

__declspec(dllexport) void *__cdecl nvapi_QueryInterface(unsigned int id)
{
    void *p;

    InitOnceExecuteOnce(&once, init, NULL, NULL);
    if (!real_query)
        return NULL;
    if (!(p = real_query(id)))
    {
        report_missing(id);
        return NULL;
    }
    TRACE("%#x -> %p\n", id, p);
    if (id == ID_SYS_GetDriverAndBranchVersion)
    {
        real_driver_branch = (driver_branch_fn)p;
        return (void *)driver_branch;
    }
    if (id == ID_GetDisplayDriverVersion)
    {
        real_display_driver = (display_driver_fn)p;
        return (void *)display_driver;
    }
    return p;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
