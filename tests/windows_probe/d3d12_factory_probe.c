/*
 * Creates a D3D12 device both ways a game can: D3D12CreateDevice, and the Agility SDK route
 * D3D12GetInterface(CLSID_D3D12DeviceFactory) -> ID3D12DeviceFactory::CreateDevice, so a
 * layer in front of d3d12.dll can be seen (in its log) intercepting both paths.
 *
 *   x86_64-w64-mingw32-gcc -O1 -Wall tests/windows_probe/d3d12_factory_probe.c \
 *     -o runtime/build/probe/d3d12-factory-probe.exe -ld3d12 -luuid
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>

DEFINE_GUID(CLSID_D3D12DeviceFactory_, 0x114863bf, 0xc386, 0x4aee, 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55);
DEFINE_GUID(IID_ID3D12Device12_, 0x5af5c532, 0x4c91, 0x4cd0, 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5);

int main(void)
{
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HRESULT (WINAPI *create)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    HRESULT (WINAPI *get_interface)(REFCLSID, REFIID, void **);
    ID3D12DeviceFactory *factory = NULL;
    ID3D12Device *dev = NULL;
    HRESULT hr;
    int fails = 0;

    printf("d3d12.dll %p\n", (void *)d3d12);
    if (!d3d12) return 2;
    create = (void *)GetProcAddress(d3d12, "D3D12CreateDevice");
    get_interface = (void *)GetProcAddress(d3d12, "D3D12GetInterface");

    hr = create(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev);
    printf("D3D12CreateDevice: hr %08lx device %p\n", hr, (void *)dev);
    if (dev)
    {
        /* What THE FINALS does next: ask for ID3D12Device12 and use the device through it.
         * Through a layer, the answer should be the layer's own object, and plain methods
         * (including one returning a struct) must still reach the real device. */
        ID3D12Device *dev12 = NULL;
        ID3D12CommandQueue *queue = NULL;
        D3D12_COMMAND_QUEUE_DESC qdesc = {0};
        LUID luid0 = ID3D12Device_GetAdapterLuid(dev), luid12;

        hr = ID3D12Device_QueryInterface(dev, &IID_ID3D12Device12_, (void **)&dev12);
        printf("  QueryInterface(ID3D12Device12): hr %08lx %p (%s object)\n", hr, (void *)dev12,
               dev12 == dev ? "same" : "different");
        if (dev12)
        {
            luid12 = ID3D12Device_GetAdapterLuid(dev12);
            printf("  through Device12: nodes %u, adapter LUID %08lx:%08lx (%s)\n", ID3D12Device_GetNodeCount(dev12),
                   luid12.HighPart, luid12.LowPart,
                   luid12.LowPart == luid0.LowPart && luid12.HighPart == luid0.HighPart ? "matches" : "MISMATCH");
            hr = ID3D12Device_CreateCommandQueue(dev12, &qdesc, &IID_ID3D12CommandQueue, (void **)&queue);
            printf("  through Device12: CreateCommandQueue hr %08lx\n", hr);
            if (queue) ID3D12CommandQueue_Release(queue);
            else fails++;
            if (luid12.LowPart != luid0.LowPart || luid12.HighPart != luid0.HighPart) fails++;
            ID3D12Device_Release(dev12);
        }
        ID3D12Device_Release(dev);
        dev = NULL;
    }
    else fails++;

    if (!get_interface) { printf("D3D12GetInterface: not exported\n"); return 1; }
    hr = get_interface(&CLSID_D3D12DeviceFactory_, &IID_ID3D12DeviceFactory, (void **)&factory);
    printf("D3D12GetInterface(DeviceFactory): hr %08lx factory %p\n", hr, (void *)factory);
    if (factory)
    {
        hr = ID3D12DeviceFactory_CreateDevice(factory, NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev);
        printf("ID3D12DeviceFactory::CreateDevice: hr %08lx device %p\n", hr, (void *)dev);
        if (dev) ID3D12Device_Release(dev);
        else fails++;
        ID3D12DeviceFactory_Release(factory);
    }
    else printf("  (GPTK has no device factory; the game cannot be using this route)\n");
    return fails;
}
