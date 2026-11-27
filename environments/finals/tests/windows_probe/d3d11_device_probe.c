/*
 * Creates a D3D11 device and a swap chain on a small window, clears and presents a few frames,
 * and prints which d3d11.dll / dxgi.dll answered and the feature level, so a renderer switch
 * (compat-tool/run STEAMDRIVER_DX11: DXMT or D3DMetal) can be proven without a game.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/d3d11_device_probe.c \
 *     -o runtime/build/probe/d3d11-device-probe.exe -ld3d11 -ldxgi -luuid -lgdi32
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>

static void module_path(const char *name)
{
    char path[MAX_PATH] = "(not loaded)";
    HMODULE m = GetModuleHandleA(name);
    if (m) GetModuleFileNameA(m, path, sizeof(path));
    printf("%-14s %s\n", name, path);
}

int main(void)
{
    WNDCLASSA wc = {0};
    DXGI_SWAP_CHAIN_DESC sd = {0};
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swapchain = NULL;
    ID3D11Texture2D *back = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D_FEATURE_LEVEL level = 0;
    const float color[4] = {0.1f, 0.4f, 0.8f, 1.0f};
    HWND hwnd;
    HRESULT hr;
    int i;

    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d11probe";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("d3d11probe", "d3d11 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         100, 100, 320, 240, NULL, NULL, wc.hInstance, NULL);

    sd.BufferCount = 2;
    sd.BufferDesc.Width = 320;
    sd.BufferDesc.Height = 240;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                       D3D11_SDK_VERSION, &sd, &swapchain, &device, &level, &context);
    printf("D3D11CreateDeviceAndSwapChain hr=%08lx feature level %x.%x\n", hr, level >> 12, (level >> 8) & 0xf);
    module_path("d3d11.dll");
    module_path("dxgi.dll");
    module_path("winemetal.dll");
    if (FAILED(hr)) return 1;

    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &rtv);
    printf("render target hr=%08lx\n", hr);
    if (FAILED(hr)) return 1;
    for (i = 0; i < 30; i++)
    {
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, color);
        hr = IDXGISwapChain_Present(swapchain, 1, 0);
        if (FAILED(hr)) break;
    }
    printf("present x%d hr=%08lx\n", i, hr);

    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(back);
    IDXGISwapChain_Release(swapchain);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    DestroyWindow(hwnd);
    printf("%s\n", i == 30 ? "PASS" : "FAIL");
    return i == 30 ? 0 : 1;
}
