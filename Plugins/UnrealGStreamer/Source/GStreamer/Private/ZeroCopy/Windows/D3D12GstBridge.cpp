// gst-side TU: only <gst/*>, no UE headers (CLAUDE.md split TU rule, research-log 2026-05-30).
#include <gst/gst.h>
#include <gst/d3d12/gstd3d12.h>
#include <d3d12.h>

static GstD3D12Device* g_CachedDevice = nullptr;
static gint64 g_CachedLuid = 0;

static gint64 LuidToI64(const LUID& L)
{
    return (gint64)(((guint64)(guint32)L.HighPart << 32) | (guint32)L.LowPart);
}

extern "C" void* GstD3D12WrapResource(void* UeDeviceRaw, void* ResourceRaw, unsigned ArraySlice)
{
    if (!UeDeviceRaw || !ResourceRaw) return nullptr;

    ID3D12Device* UeDevice = static_cast<ID3D12Device*>(UeDeviceRaw);
    ID3D12Resource* Resource = static_cast<ID3D12Resource*>(ResourceRaw);

    const gint64 Luid = LuidToI64(UeDevice->GetAdapterLuid());
    if (!g_CachedDevice || g_CachedLuid != Luid)
    {
        if (g_CachedDevice) { gst_object_unref(g_CachedDevice); g_CachedDevice = nullptr; }
        g_CachedDevice = gst_d3d12_device_new_for_adapter_luid(Luid);
        g_CachedLuid = Luid;
    }
    if (!g_CachedDevice)
    {
        g_printerr("[GstD3D12Bridge] gst_d3d12_device_new_for_adapter_luid(%lld) returned null\n", (long long)Luid);
        return nullptr;
    }

    ID3D12Device* GstDev = gst_d3d12_device_get_device_handle(g_CachedDevice);
    if (GstDev != UeDevice)
    {
        g_printerr("[GstD3D12Bridge] WARNING: device handle mismatch ue=%p gst=%p (same LUID)\n", UeDevice, GstDev);
    }

    Resource->AddRef();
    GstMemory* Mem = gst_d3d12_allocator_alloc_wrapped(
        nullptr,
        g_CachedDevice,
        Resource,
        (guint)ArraySlice,
        nullptr,
        nullptr);
    if (!Mem)
    {
        Resource->Release();
        g_printerr("[GstD3D12Bridge] gst_d3d12_allocator_alloc_wrapped returned null\n");
        return nullptr;
    }
    return Mem;
}

extern "C" void* GstD3D12WrapResourceWithFence(
    void* UeDeviceRaw, void* ResourceRaw, unsigned ArraySlice,
    void* FenceRaw, unsigned long long FenceValue)
{
    void* MemRaw = GstD3D12WrapResource(UeDeviceRaw, ResourceRaw, ArraySlice);
    if (!MemRaw || !FenceRaw) return MemRaw;
    GstMemory* Mem = static_cast<GstMemory*>(MemRaw);
    ID3D12Fence* Fence = static_cast<ID3D12Fence*>(FenceRaw);
    gst_d3d12_memory_set_fence(GST_D3D12_MEMORY_CAST(Mem), Fence, (guint64)FenceValue, FALSE);
    return MemRaw;
}

extern "C" void GstD3D12BridgeShutdown()
{
    if (g_CachedDevice) { gst_object_unref(g_CachedDevice); g_CachedDevice = nullptr; }
    g_CachedLuid = 0;
}
