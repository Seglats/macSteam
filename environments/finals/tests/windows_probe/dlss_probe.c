/*
 * What a game that offers DLSS sees: the DXGI adapters (name, vendor), a D3D12 device, NVAPI
 * initialising, NVIDIA's NGX registry key (NGXCore FullPath) and nvngx.dll loading from it
 * with its D3D12 entry points. Proves the upscaling switch (compat-tool/run
 * STEAMDRIVER_UPSCALE: D3DMetal's MetalFX, D3DM_ENABLE_METALFX) without a game.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/dlss_probe.c \
 *     -o runtime/build/probe/dlss-probe.exe -ld3d12 -ld3d11 -ld3d9 -ldxgi -luuid -ladvapi32 -lwintrust
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <softpub.h>
#include <wintrust.h>
#include <stdio.h>
#include <string.h>

typedef void *(__cdecl *nvapi_query)(unsigned int);
typedef int (__cdecl *nvapi_init)(void);

static void module_path(const char *name)
{
    char path[MAX_PATH] = "(not loaded)";
    HMODULE m = GetModuleHandleA(name);
    if (m) GetModuleFileNameA(m, path, sizeof(path));
    printf("%-14s %s\n", name, path);
}

/* NGX's C entry points (NVIDIA's nvsdk_ngx.h): results are NVSDK_NGX_Result, 1 = success,
 * 0xBAD0xxxx = failure. The parameters object stays opaque here. */
typedef unsigned int (__cdecl *ngx_init)(unsigned long long, const wchar_t *, IUnknown *, const void *, unsigned int);
typedef unsigned int (__cdecl *ngx_caps)(void **);
typedef unsigned int (__cdecl *ngx_shutdown)(void);

/* Initialises NGX on DEVICE the way a game's DLSS start-up does, asks for the capability
 * parameters and shuts down again. API ("D3D11"/"D3D12") picks the entry points. */
static int ngx_start(HMODULE ngx, const char *api, IUnknown *device)
{
    char name[64];
    ngx_init init;
    ngx_caps caps;
    ngx_shutdown shutdown;
    void *params = NULL;
    unsigned int r, c = 0;

    snprintf(name, sizeof(name), "NVSDK_NGX_%s_Init", api);
    init = (ngx_init)GetProcAddress(ngx, name);
    snprintf(name, sizeof(name), "NVSDK_NGX_%s_GetCapabilityParameters", api);
    caps = (ngx_caps)GetProcAddress(ngx, name);
    snprintf(name, sizeof(name), "NVSDK_NGX_%s_Shutdown", api);
    shutdown = (ngx_shutdown)GetProcAddress(ngx, name);
    if (!init || !caps) { printf("NGX %s: entry points missing\n", api); return 0; }
    r = init(0x1234, L"C:\\users\\Public", device, NULL, 0x15);
    if (r == 1) c = caps(&params);
    printf("NGX %s: Init %08x, GetCapabilityParameters %08x (%s)\n", api, r, c,
           r == 1 && c == 1 && params ? "DLSS can start" : "FAILED");
    if (r == 1 && c == 1 && params)
    {
        /* What a game reads next to decide whether to offer DLSS. NVSDK_NGX_Parameter is a C++
         * interface; with MSVC's layout (overloads grouped, in reverse declaration order) the
         * Get overloads sit at slots 8-15 and Get(const char *, int *) is slot 11,
         * Get(const char *, unsigned int *) slot 12. */
        typedef unsigned int (__cdecl *get_int)(void *, const char *, int *);
        static const char *keys[] = {"SuperSampling.Available", "SuperSampling.NeedsUpdatedDriver",
                                     "SuperSampling.MinDriverVersionMajor", "SuperSampling.MinDriverVersionMinor",
                                     "SuperSampling.FeatureInitResult"};
        void **vtbl = *(void ***)params;
        unsigned int k;
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
        {
            int v = -1;
            unsigned int g = ((get_int)vtbl[11])(params, keys[k], &v);
            printf("  %-38s %d (result %08x)\n", keys[k], v, g);
        }
    }
    if (r == 1 && shutdown) shutdown();
    return r == 1 && c == 1 && params;
}

/* Authenticode check of a file on disk, the way NVIDIA's NGX loader verifies nvngx.dll. */
static LONG signature_ok(const char *file)
{
    WCHAR wide[MAX_PATH + 16];
    WINTRUST_FILE_INFO info = {sizeof(info)};
    WINTRUST_DATA data = {sizeof(data)};
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    MultiByteToWideChar(CP_ACP, 0, file, -1, wide, MAX_PATH + 16);
    info.pcwszFilePath = wide;
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &info;
    data.dwStateAction = WTD_STATEACTION_IGNORE;
    return WinVerifyTrust(NULL, &action, &data);
}

/* "dlss-probe.exe d3d11": DX11 only (DXMT mode: DXMT's DXGI with Apple's d3d12 is not a pair a
 * game uses). */
int main(int argc, char **argv)
{
    int d3d11_only = argc > 1 && !strcmp(argv[1], "d3d11");
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    ID3D12Device *device = NULL;
    ID3D11Device *d3d11 = NULL;
    char path[MAX_PATH] = "", dll[MAX_PATH + 16];
    DWORD size = sizeof(path);
    HMODULE nvapi, ngx;
    LONG trust;
    HRESULT hr;
    UINT i;
    int ok = 1;

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    printf("CreateDXGIFactory1 hr=%08lx\n", hr);
    for (i = 0; SUCCEEDED(hr) && IDXGIFactory1_EnumAdapters1(factory, i, &adapter) == S_OK; i++)
    {
        DXGI_ADAPTER_DESC1 d;
        LARGE_INTEGER umd;
        IDXGIAdapter1_GetDesc1(adapter, &d);
        printf("adapter %u: %ls vendor %04x device %04x\n", i, d.Description, d.VendorId, d.DeviceId);
        /* The driver version games compare against minimums (Source 2 warns "out of date"). */
        if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd)))
            printf("  DXGI driver version %u.%u.%u.%u\n", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                   HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        else printf("  DXGI driver version: CheckInterfaceSupport failed\n");
        if (i == 0 && !d3d11_only)
        {
            hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&device);
            printf("D3D12CreateDevice hr=%08lx\n", hr);
            if (FAILED(hr)) ok = 0;
        }
        IDXGIAdapter1_Release(adapter);
    }
    {
        /* Where Deadlock reads the driver version (its console: "Determined driver version"). */
        IDirect3D9 *d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
        D3DADAPTER_IDENTIFIER9 id;
        if (d3d9 && SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d9, 0, 0, &id)))
            printf("D3D9 adapter: vendor %04lx device %04lx, driver version %u.%u.%u.%u\n", id.VendorId, id.DeviceId,
                   HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
                   HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart));
        else printf("D3D9 adapter identifier: not available\n");
        if (d3d9) IDirect3D9_Release(d3d9);
    }
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d3d11, NULL, NULL);
    printf("D3D11CreateDevice hr=%08lx\n", hr);
    if (FAILED(hr)) { d3d11 = NULL; ok = 0; }
    else
    {
        /* Source 2 turns asynchronous shader creation off ("old driver that might crash")
         * when the driver can't create objects from several threads at once. */
        D3D11_FEATURE_DATA_THREADING t = {0};
        hr = ID3D11Device_CheckFeatureSupport(d3d11, D3D11_FEATURE_THREADING, &t, sizeof(t));
        printf("D3D11 threading: concurrent creates %d, command lists %d (hr=%08lx)\n",
               t.DriverConcurrentCreates, t.DriverCommandLists, hr);
        {
            /* How engines detect PIX/Nsight: GetStatus() is TRUE while a capture tool is
             * attached (Deadlock: "PIX/NSIGHT WAS detected", then DLSS not available). */
            ID3D11DeviceContext *ctx = NULL;
            ID3DUserDefinedAnnotation *ann = NULL;
            ID3D11Device_GetImmediateContext(d3d11, &ctx);
            if (ctx && SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3DUserDefinedAnnotation, (void **)&ann)))
            {
                printf("D3D11 annotation GetStatus (TRUE = capture tool attached): %d\n",
                       ID3DUserDefinedAnnotation_GetStatus(ann));
                ID3DUserDefinedAnnotation_Release(ann);
            }
            else printf("D3D11 annotation interface: not available\n");
            if (ctx) ID3D11DeviceContext_Release(ctx);
        }
    }
    {
        /* PIX's programmatic-capture interface: only a graphics debugger provides it. */
        static const GUID iid_graphics_analysis =
            {0x9f251514, 0x9d4d, 0x4902, {0x9d, 0x60, 0x18, 0x98, 0x8a, 0xb7, 0xd4, 0xb5}};
        typedef HRESULT (WINAPI *debug1_fn)(UINT, REFIID, void **);
        debug1_fn debug1 = (debug1_fn)GetProcAddress(GetModuleHandleA("dxgi.dll"), "DXGIGetDebugInterface1");
        IUnknown *ga = NULL;
        hr = debug1 ? debug1(0, &iid_graphics_analysis, (void **)&ga) : E_NOTIMPL;
        printf("DXGIGetDebugInterface1(IDXGraphicsAnalysis) hr=%08lx %s\n", hr,
               SUCCEEDED(hr) ? "(looks like a graphics debugger is attached)" : "(no debugger)");
        if (ga) IUnknown_Release(ga);
    }
    module_path("d3d12.dll");
    module_path("d3d11.dll");
    module_path("dxgi.dll");

    nvapi = LoadLibraryA("nvapi64.dll");
    if (nvapi)
    {
        nvapi_query query = (nvapi_query)GetProcAddress(nvapi, "nvapi_QueryInterface");
        nvapi_init init = query ? (nvapi_init)query(0x0150e828) : NULL;
        printf("nvapi64.dll loaded; NvAPI_Initialize %s\n", init ? (init() == 0 ? "OK" : "failed") : "missing");
        if (query)
        {
            /* NvAPI_SYS_GetDriverAndBranchVersion(NvU32 *version, char branch[64]) */
            typedef int (__cdecl *nvapi_driver)(unsigned int *, char *);
            nvapi_driver drv = (nvapi_driver)query(0x2926aaad);
            unsigned int version = 0;
            char branch[64] = "";
            int r = drv ? drv(&version, branch) : -1;
            printf("  NvAPI driver version %u.%02u, branch %s (status %d)\n", version / 100, version % 100, branch, r);
        }
    }
    else printf("nvapi64.dll not loadable (error %lu)\n", GetLastError());
    module_path("nvapi64.dll");

    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath",
                     RRF_RT_REG_SZ, NULL, path, &size) == ERROR_SUCCESS)
    {
        printf("NGXCore FullPath %s\n", path);
        snprintf(dll, sizeof(dll), "%s\\nvngx.dll", path);
        trust = signature_ok(dll);
        printf("nvngx.dll signature (WinVerifyTrust, as a game's NGX loader checks it): %08lx %s\n",
               trust, trust == 0 ? "valid" : "REJECTED");
        if (trust) ok = 0;
        ngx = LoadLibraryA(dll);
        printf("nvngx.dll from it: %s; NVSDK_NGX_D3D12_Init %s\n", ngx ? "loaded" : "not loadable",
               ngx && GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init") ? "present" : "missing");
        if (!ngx) ok = 0;
        /* What a game's DLSS start-up does next: NGX on its own device, then the capabilities. */
        if (ngx && device && !ngx_start(ngx, "D3D12", (IUnknown *)device)) ok = 0;
        if (ngx && d3d11 && !ngx_start(ngx, "D3D11", (IUnknown *)d3d11)) ok = 0;
    }
    else { printf("NGXCore FullPath not set\n"); ok = 0; }

    if (d3d11) ID3D11Device_Release(d3d11);
    if (device) ID3D12Device_Release(device);
    if (factory) IDXGIFactory1_Release(factory);
    printf("%s\n", ok ? "DLSS PATH COMPLETE" : "DLSS PATH INCOMPLETE");
    return ok ? 0 : 1;
}
