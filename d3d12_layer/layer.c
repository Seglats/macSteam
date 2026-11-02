/*
 * MacFinals d3d12 layer: sits in front of GPTK's d3d12.dll (installed unchanged as
 * d3d12_gptk.dll) and makes one DXIL construct well defined before D3DMetal translates it
 * (dxil_rewrite.c, tests/rosetta_nt/R15_SKIN_CURVE.md). Every export except
 * D3D12CreateDevice and D3D12GetInterface are forwarded (d3d12.def). The device is wrapped;
 * all its methods forward to GPTK's device except pipeline creation, which passes shaders
 * through the rewriter first. Unchanged shaders are passed through as they are.
 *
 * MACFINALS_SKIN_FIX=0 disables the rewrite (the layer then only forwards).
 * MACFINALS_D3D12_LAYER_LOG=1 logs every rewritten shader (to Wine's debug output).
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxil_rewrite.h"

struct layer_device
{
    ID3D12Device10 iface;
    ID3D12Device10 *real;
    int version;                /* highest ID3D12DeviceN the real device implements */
};

#define REAL(This) (((struct layer_device *)(This))->real)

/* The four GPTK methods the overrides below hand off to. */
static HRESULT fwd_CreateGraphicsPipelineState(ID3D12Device10 *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
        REFIID riid, void **out)
{
    return REAL(This)->lpVtbl->CreateGraphicsPipelineState(REAL(This), desc, riid, out);
}

static HRESULT fwd_CreateComputePipelineState(ID3D12Device10 *This, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
        REFIID riid, void **out)
{
    return REAL(This)->lpVtbl->CreateComputePipelineState(REAL(This), desc, riid, out);
}

static HRESULT fwd_CreatePipelineState(ID3D12Device10 *This, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
        REFIID riid, void **out)
{
    return REAL(This)->lpVtbl->CreatePipelineState(REAL(This), desc, riid, out);
}

static HRESULT fwd_CreatePipelineLibrary(ID3D12Device10 *This, const void *blob, SIZE_T size, REFIID riid, void **out)
{
    return REAL(This)->lpVtbl->CreatePipelineLibrary(REAL(This), blob, size, riid, out);
}

/* The wrapper's vtable: every slot is a generic forwarding stub (device_thunks below), then
 * the methods the layer handles are overridden in C. Stubs make the wrapper correct for
 * ID3D12Device11 and later, whose vtables MinGW's and Wine's headers do not describe. */
#define DEVICE_SLOTS 128
static union { ID3D12Device10Vtbl v; void *slot[DEVICE_SLOTS]; } layer_vtbl;
_Static_assert(sizeof(ID3D12Device10Vtbl) == 79 * sizeof(void *), "ID3D12Device10 has 79 methods");

/* Stub i: this (rcx) becomes GPTK's device, then jump to GPTK's slot i. Every COM method
 * takes this in rcx (a struct return's hidden pointer goes in rdx), so one stub fits any
 * signature. No stack change, so no unwind data is needed. 16 bytes each. */
__asm__(
    ".text\n"
    ".balign 16\n"
    ".globl device_thunks\n"
    "device_thunks:\n"
    ".set slot, 0\n"
    ".rept 128\n"
    ".balign 16\n"
    "movq 8(%rcx), %rcx\n"           /* struct layer_device: iface at 0, real at 8 */
    "movq (%rcx), %rax\n"
    "jmpq *slot*8(%rax)\n"
    ".set slot, slot+1\n"
    ".endr\n");
extern const char device_thunks[];
_Static_assert(DEVICE_SLOTS == 128, "device_thunks has 128 stubs");
static HMODULE gptk;
static HRESULT (WINAPI *real_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static int enabled = 1, verbose;
static LONG rewritten_shaders, rewritten_reads, cbuffer_shaders, unknown_streams;

/* Lines go to Wine's debug output (the stream WINEDEBUG traces use), prefixed with pid:tid
 * like Wine's own. A process started by Steam has no Windows stderr, so writing there would
 * lose them. Nothing reaches the game. */
static void log_line(const char *fmt, ...)
{
    static int (CDECL *dbg_output)(const char *);
    static LONG resolved;
    char buf[600];
    int n;
    va_list ap;

    if (!InterlockedCompareExchange(&resolved, 1, 0))
        dbg_output = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_dbg_output");
    n = snprintf(buf, sizeof(buf), "%04lx:%04lx:macfinals-d3d12: ", GetCurrentProcessId(), GetCurrentThreadId());
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (dbg_output) dbg_output(buf);
    else OutputDebugStringA(buf);
}

static void log_guid(const char *what, REFGUID g)
{
    log_line("%s {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n", what, g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* Logs a GUID the first time it is seen (bounded; later ones are not logged). */
static void log_guid_once(const char *what, REFGUID g)
{
    static GUID seen[64];
    static LONG count;
    static SRWLOCK lock = SRWLOCK_INIT;
    LONG i;
    BOOL fresh = TRUE;

    AcquireSRWLockExclusive(&lock);
    for (i = 0; i < count; i++) if (IsEqualGUID(&seen[i], g)) { fresh = FALSE; break; }
    if (fresh && count < 64) seen[count++] = *g;
    else fresh = FALSE;
    ReleaseSRWLockExclusive(&lock);
    if (fresh) log_guid(what, g);
}

/* ---- rewrite cache, keyed by the DXBC checksum; entries live for the process ---- */

struct cache_entry { BYTE digest[16]; void *blob; SIZE_T size; BOOL used; };
#define CACHE_SIZE 32768
static struct cache_entry *cache;
static CRITICAL_SECTION cache_cs;

static struct cache_entry *cache_find(const BYTE *digest, BOOL *found)
{
    unsigned slot = (digest[0] | digest[1] << 8 | digest[2] << 16) % CACHE_SIZE, probe;
    for (probe = 0; probe < CACHE_SIZE; probe++)
    {
        struct cache_entry *e = &cache[(slot + probe) % CACHE_SIZE];
        if (!e->used) { *found = FALSE; return e; }
        if (!memcmp(e->digest, digest, 16)) { *found = TRUE; return e; }
    }
    *found = FALSE;
    return NULL;                        /* full */
}

/* Returns the rewritten bytecode (owned by the cache) or NULL to keep the original. */
static const void *rewrite_bytecode(const void *code, SIZE_T size, SIZE_T *out_size)
{
    const BYTE *digest = (const BYTE *)code + 4;
    struct cache_entry *e;
    void *out = NULL;
    size_t new_size = 0;
    unsigned reads, cb_reads;
    BOOL found;

    if (!enabled || !cache || !code || size < 32 || memcmp(code, "DXBC", 4)) return NULL;
    EnterCriticalSection(&cache_cs);
    e = cache_find(digest, &found);
    if (found) { *out_size = e->size; LeaveCriticalSection(&cache_cs); return e->blob; }
    LeaveCriticalSection(&cache_cs);

    if ((reads = dxil_rewrite_container(code, size, &out, &new_size, &cb_reads)))
    {
        LONG n = InterlockedIncrement(&rewritten_shaders);
        LONG cb = cb_reads ? InterlockedIncrement(&cbuffer_shaders) : cbuffer_shaders;
        InterlockedExchangeAdd(&rewritten_reads, (LONG)reads);
        if (verbose || n == 1 || !(n % 25) || (cb_reads && cb == 1))
            log_line("shader %02x%02x%02x%02x: %u array read(s) made defined, %u of them now read the "
                     "constant buffer directly (%ld shaders so far, %ld with buffer reads)\n",
                     digest[0], digest[1], digest[2], digest[3], reads, cb_reads, n, cb);
    }

    EnterCriticalSection(&cache_cs);
    e = cache_find(digest, &found);
    if (found) { free(out); out = e->blob; new_size = e->size; }          /* another thread won */
    else if (e) { memcpy(e->digest, digest, 16); e->blob = out; e->size = new_size; e->used = TRUE; }
    /* cache full: the rewritten copy is used but not kept (it leaks; never seen in practice) */
    LeaveCriticalSection(&cache_cs);
    *out_size = new_size;
    return out;
}

/* First use of each pipeline-creation path, so a run shows which ones the game uses. */
static void note_path(LONG *flag, const char *name)
{
    if (!InterlockedExchange(flag, 1)) log_line("first %s through the layer\n", name);
}

static void fix_shader(D3D12_SHADER_BYTECODE *bc)
{
    SIZE_T size;
    const void *p;
    if (!bc->pShaderBytecode || !bc->BytecodeLength) return;
    if ((p = rewrite_bytecode(bc->pShaderBytecode, bc->BytecodeLength, &size)))
    {
        bc->pShaderBytecode = p;
        bc->BytecodeLength = size;
    }
}

/* ---- intercepted device methods ---- */

/* Newer than MinGW's d3d12.h. THE FINALS asks for both (seen in the layer log). */
DEFINE_GUID(IID_ID3D12Device11_, 0x5405c344, 0xd457, 0x444e, 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39);
DEFINE_GUID(IID_ID3D12Device12_, 0x5af5c532, 0x4c91, 0x4cd0, 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5);

static const GUID *const device_iids[] =
{
    &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3,
    &IID_ID3D12Device4, &IID_ID3D12Device5, &IID_ID3D12Device6, &IID_ID3D12Device7,
    &IID_ID3D12Device8, &IID_ID3D12Device9, &IID_ID3D12Device10,
    &IID_ID3D12Device11_, &IID_ID3D12Device12_,
};

static HRESULT STDMETHODCALLTYPE layer_QueryInterface(ID3D12Device10 *This, REFIID riid, void **out)
{
    struct layer_device *dev = (struct layer_device *)This;
    int i;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ID3D12Object))
        goto ours;
    for (i = 0; i <= dev->version; i++)
        if (IsEqualGUID(riid, device_iids[i])) goto ours;
    /* anything else (newer device versions, debug and vendor interfaces) comes from GPTK;
     * a newer device version handed out here bypasses the layer, so say so */
    {
        HRESULT hr = dev->real->lpVtbl->QueryInterface(dev->real, riid, out);
        log_guid_once(SUCCEEDED(hr) ? "device QueryInterface answered by GPTK, unwrapped:"
                                    : "device QueryInterface refused by GPTK:", riid);
        return hr;
    }
ours:
    dev->real->lpVtbl->AddRef(dev->real);
    *out = &dev->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE layer_CreateGraphicsPipelineState(ID3D12Device10 *This,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    static LONG seen;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC copy;
    note_path(&seen, "CreateGraphicsPipelineState");
    if (!desc) return fwd_CreateGraphicsPipelineState(This, desc, riid, out);
    copy = *desc;
    fix_shader(&copy.VS); fix_shader(&copy.PS); fix_shader(&copy.DS);
    fix_shader(&copy.HS); fix_shader(&copy.GS);
    return fwd_CreateGraphicsPipelineState(This, &copy, riid, out);
}

static HRESULT STDMETHODCALLTYPE layer_CreateComputePipelineState(ID3D12Device10 *This,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    static LONG seen;
    D3D12_COMPUTE_PIPELINE_STATE_DESC copy;
    note_path(&seen, "CreateComputePipelineState");
    if (!desc) return fwd_CreateComputePipelineState(This, desc, riid, out);
    copy = *desc;
    fix_shader(&copy.CS);
    return fwd_CreateComputePipelineState(This, &copy, riid, out);
}

/* D3D12_RT_FORMAT_ARRAY, which MinGW's d3d12.h does not define. */
struct rt_format_array { DXGI_FORMAT formats[8]; UINT count; };

/* Payload size and alignment of each pipeline-state stream subobject type. */
static int subobject_layout(UINT type, size_t *size, size_t *align)
{
#define L(t) (*size = sizeof(t), *align = __alignof__(t), 1)
    switch (type)
    {
    case 0: return L(ID3D12RootSignature *);
    case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25: return L(D3D12_SHADER_BYTECODE);
    case 7: return L(D3D12_STREAM_OUTPUT_DESC);
    case 8: return L(D3D12_BLEND_DESC);
    case 9: return L(UINT);
    case 10: return L(D3D12_RASTERIZER_DESC);
    case 11: return L(D3D12_DEPTH_STENCIL_DESC);
    case 12: return L(D3D12_INPUT_LAYOUT_DESC);
    case 13: return L(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
    case 14: return L(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
    case 15: return L(struct rt_format_array);
    case 16: return L(DXGI_FORMAT);
    case 17: return L(DXGI_SAMPLE_DESC);
    case 18: return L(UINT);                         /* node mask */
    case 19: return L(D3D12_CACHED_PIPELINE_STATE);
    case 20: return L(D3D12_PIPELINE_STATE_FLAGS);
    case 21: return L(D3D12_DEPTH_STENCIL_DESC1);
    case 22: return L(D3D12_VIEW_INSTANCING_DESC);
    }
    return 0;
#undef L
}

static HRESULT STDMETHODCALLTYPE layer_CreatePipelineState(ID3D12Device10 *This,
        const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **out)
{
    D3D12_PIPELINE_STATE_STREAM_DESC copy;
    BYTE *buf;
    size_t off = 0;
    HRESULT hr;
    static LONG seen;

    note_path(&seen, "CreatePipelineState (stream)");
    if (!enabled || !desc || !desc->pPipelineStateSubobjectStream || !desc->SizeInBytes)
        return fwd_CreatePipelineState(This, desc, riid, out);
    if (!(buf = malloc(desc->SizeInBytes))) return fwd_CreatePipelineState(This, desc, riid, out);
    memcpy(buf, desc->pPipelineStateSubobjectStream, desc->SizeInBytes);
    while (off + sizeof(UINT) <= desc->SizeInBytes)
    {
        UINT type;
        size_t size, align, payload;
        memcpy(&type, buf + off, sizeof(type));
        if (!subobject_layout(type, &size, &align))
        {
            /* unknown subobject: create the pipeline exactly as the game asked */
            if (InterlockedIncrement(&unknown_streams) == 1)
                log_line("pipeline stream with subobject type %u left unchanged\n", type);
            free(buf);
            return fwd_CreatePipelineState(This, desc, riid, out);
        }
        payload = (off + sizeof(UINT) + align - 1) & ~(align - 1);
        if (payload + size > desc->SizeInBytes) break;
        if ((type >= 1 && type <= 6) || type == 24 || type == 25)
            fix_shader((D3D12_SHADER_BYTECODE *)(buf + payload));
        off = (payload + size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
    }
    copy.SizeInBytes = desc->SizeInBytes;
    copy.pPipelineStateSubobjectStream = buf;
    hr = fwd_CreatePipelineState(This, &copy, riid, out);
    free(buf);
    return hr;
}

static HRESULT STDMETHODCALLTYPE layer_CreatePipelineLibrary(ID3D12Device10 *This, const void *blob,
        SIZE_T size, REFIID riid, void **out)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        log_line("game uses a pipeline library; pipelines loaded from it are not rewritten\n");
    return fwd_CreatePipelineLibrary(This, blob, size, riid, out);
}

/* ---- device wrapping, shared by D3D12CreateDevice and ID3D12DeviceFactory ---- */

/* Wraps a device GPTK created; consumes the caller's reference to base. */
static HRESULT wrap_device(ID3D12Device *base, REFIID riid, void **out, const char *via)
{
    struct layer_device *dev;
    HRESULT hr;
    int i;

    if (!(dev = calloc(1, sizeof(*dev)))) { ID3D12Device_Release(base); return E_OUTOFMEMORY; }
    for (i = (int)(sizeof(device_iids) / sizeof(device_iids[0])) - 1; i >= 0; i--)
    {
        void *p;
        if (SUCCEEDED(ID3D12Device_QueryInterface(base, device_iids[i], &p)))
        {
            dev->real = p;
            dev->version = i;
            break;
        }
    }
    ID3D12Device_Release(base);         /* dev->real holds its own reference */
    if (!dev->real) { free(dev); return E_NOINTERFACE; }
    dev->iface.lpVtbl = &layer_vtbl.v;
    log_line("device created via %s, wrapped as ID3D12Device%d (shader rewrite %s)\n", via, dev->version,
             enabled ? "on" : "off");
    /* The wrapper is never freed: AddRef/Release forward to GPTK's device, and a game
     * creates few devices. */
    hr = layer_QueryInterface(&dev->iface, riid, out);
    ID3D12Device10_Release(&dev->iface);
    return hr;
}

/* ---- ID3D12DeviceFactory (Agility SDK path: D3D12GetInterface(CLSID_D3D12DeviceFactory)) ---- */

struct layer_factory
{
    ID3D12DeviceFactory iface;
    ID3D12DeviceFactory *real;
    LONG refs;
};

static inline struct layer_factory *impl_factory(ID3D12DeviceFactory *iface)
{
    return (struct layer_factory *)iface;
}

static HRESULT STDMETHODCALLTYPE factory_QueryInterface(ID3D12DeviceFactory *iface, REFIID riid, void **out)
{
    struct layer_factory *f = impl_factory(iface);
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ID3D12DeviceFactory))
    {
        InterlockedIncrement(&f->refs);
        *out = iface;
        return S_OK;
    }
    log_guid_once("device factory QueryInterface passed to GPTK unwrapped:", riid);
    return f->real->lpVtbl->QueryInterface(f->real, riid, out);
}

static ULONG STDMETHODCALLTYPE factory_AddRef(ID3D12DeviceFactory *iface)
{
    return InterlockedIncrement(&impl_factory(iface)->refs);
}

static ULONG STDMETHODCALLTYPE factory_Release(ID3D12DeviceFactory *iface)
{
    struct layer_factory *f = impl_factory(iface);
    ULONG refs = InterlockedDecrement(&f->refs);
    if (!refs)
    {
        f->real->lpVtbl->Release(f->real);
        free(f);
    }
    return refs;
}

static HRESULT STDMETHODCALLTYPE factory_InitializeFromGlobalState(ID3D12DeviceFactory *iface)
{
    struct layer_factory *f = impl_factory(iface);
    return f->real->lpVtbl->InitializeFromGlobalState(f->real);
}

static HRESULT STDMETHODCALLTYPE factory_ApplyToGlobalState(ID3D12DeviceFactory *iface)
{
    struct layer_factory *f = impl_factory(iface);
    return f->real->lpVtbl->ApplyToGlobalState(f->real);
}

static HRESULT STDMETHODCALLTYPE factory_SetFlags(ID3D12DeviceFactory *iface, D3D12_DEVICE_FACTORY_FLAGS flags)
{
    struct layer_factory *f = impl_factory(iface);
    return f->real->lpVtbl->SetFlags(f->real, flags);
}

static D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE factory_GetFlags(ID3D12DeviceFactory *iface)
{
    struct layer_factory *f = impl_factory(iface);
    return f->real->lpVtbl->GetFlags(f->real);
}

static HRESULT STDMETHODCALLTYPE factory_GetConfigurationInterface(ID3D12DeviceFactory *iface, REFCLSID clsid,
        REFIID riid, void **out)
{
    struct layer_factory *f = impl_factory(iface);
    return f->real->lpVtbl->GetConfigurationInterface(f->real, clsid, riid, out);
}

static HRESULT STDMETHODCALLTYPE factory_EnableExperimentalFeatures(ID3D12DeviceFactory *iface, UINT count,
        const IID *iids, void *configs, UINT *sizes)
{
    struct layer_factory *f = impl_factory(iface);
    return f->real->lpVtbl->EnableExperimentalFeatures(f->real, count, iids, configs, sizes);
}

static HRESULT STDMETHODCALLTYPE factory_CreateDevice(ID3D12DeviceFactory *iface, IUnknown *adapter,
        D3D_FEATURE_LEVEL level, REFIID riid, void **out)
{
    struct layer_factory *f = impl_factory(iface);
    ID3D12Device *base;
    HRESULT hr;

    if (!out) return f->real->lpVtbl->CreateDevice(f->real, adapter, level, riid, out);  /* capability query */
    if (FAILED(hr = f->real->lpVtbl->CreateDevice(f->real, adapter, level, &IID_ID3D12Device, (void **)&base)))
        return hr;
    return wrap_device(base, riid, out, "ID3D12DeviceFactory::CreateDevice");
}

static ID3D12DeviceFactoryVtbl factory_vtbl =
{
    factory_QueryInterface, factory_AddRef, factory_Release,
    factory_InitializeFromGlobalState, factory_ApplyToGlobalState, factory_SetFlags, factory_GetFlags,
    factory_GetConfigurationInterface, factory_EnableExperimentalFeatures, factory_CreateDevice,
};

/* ---- exports ---- */

static HRESULT (WINAPI *real_get_interface)(REFCLSID, REFIID, void **);

static BOOL CALLBACK init_once(INIT_ONCE *once, void *param, void **context)
{
    const char *v;
    HMODULE self = NULL;
    int i;

    if ((v = getenv("MACFINALS_SKIN_FIX")) && !strcmp(v, "0")) enabled = 0;
    if ((v = getenv("MACFINALS_D3D12_LAYER_LOG")) && *v && strcmp(v, "0")) verbose = 1;
    InitializeCriticalSection(&cache_cs);
    cache = calloc(CACHE_SIZE, sizeof(*cache));
    for (i = 0; i < DEVICE_SLOTS; i++) layer_vtbl.slot[i] = (void *)(device_thunks + 16 * i);
    layer_vtbl.v.QueryInterface = layer_QueryInterface;
    layer_vtbl.v.CreateGraphicsPipelineState = layer_CreateGraphicsPipelineState;
    layer_vtbl.v.CreateComputePipelineState = layer_CreateComputePipelineState;
    layer_vtbl.v.CreatePipelineState = layer_CreatePipelineState;
    layer_vtbl.v.CreatePipelineLibrary = layer_CreatePipelineLibrary;
    if (!(gptk = LoadLibraryW(L"d3d12_gptk.dll")))
    {
        log_line("d3d12_gptk.dll not found (error %lu)\n", GetLastError());
        return TRUE;
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)init_once, &self);
    if (gptk == self)
    {
        log_line("d3d12_gptk.dll resolved to this layer; check the runtime layout\n");
        return TRUE;
    }
    real_create_device = (void *)GetProcAddress(gptk, "D3D12CreateDevice");
    real_get_interface = (void *)GetProcAddress(gptk, "D3D12GetInterface");
    log_line("layer %p, gptk %p, shader rewrite %s, per-shader log %s\n", (void *)self, (void *)gptk,
             enabled ? "on" : "off (MACFINALS_SKIN_FIX=0)", verbose ? "on" : "off");
    return TRUE;
}

static void init(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, init_once, NULL, NULL);
}

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **out)
{
    ID3D12Device *base;
    HRESULT hr;

    init();
    if (!real_create_device) return E_FAIL;
    if (!out) return real_create_device(adapter, level, riid, out);      /* capability query */
    if (FAILED(hr = real_create_device(adapter, level, &IID_ID3D12Device, (void **)&base))) return hr;
    return wrap_device(base, riid, out, "D3D12CreateDevice");
}

HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void **out)
{
    struct layer_factory *f;
    HRESULT hr;

    init();
    if (!real_get_interface) return E_FAIL;
    log_guid_once("D3D12GetInterface clsid", clsid);
    log_guid_once("D3D12GetInterface riid", riid);
    hr = real_get_interface(clsid, riid, out);
    if (FAILED(hr) || !out || !*out || !IsEqualGUID(riid, &IID_ID3D12DeviceFactory)) return hr;
    if (!(f = malloc(sizeof(*f)))) return hr;     /* keep GPTK's factory rather than fail the call */
    f->iface.lpVtbl = &factory_vtbl;
    f->real = *out;
    f->refs = 1;
    *out = &f->iface;
    log_line("device factory wrapped\n");
    return hr;
}
