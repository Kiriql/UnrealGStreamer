#include "../IZeroCopyBackend.h"
#include "Core/GStreamerLog.h"

#if PLATFORM_WINDOWS

#include "ID3D12DynamicRHI.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "Windows/HideWindowsPlatformTypes.h"

using Microsoft::WRL::ComPtr;

namespace
{
    DXGI_FORMAT ToDxgi(EGstZeroCopyFormat F)
    {
        switch (F)
        {
        case EGstZeroCopyFormat::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case EGstZeroCopyFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    EPixelFormat ToPixelFormat(EGstZeroCopyFormat F)
    {
        switch (F)
        {
        case EGstZeroCopyFormat::BGRA8: return PF_B8G8R8A8;
        case EGstZeroCopyFormat::RGBA8: return PF_R8G8B8A8;
        }
        return PF_B8G8R8A8;
    }
}

class FD3D12ZeroCopyBackend : public IZeroCopyBackend
{
public:
    virtual const TCHAR* GetBackendName() const override { return TEXT("D3D12"); }

    virtual bool Init(FString& OutError) override
    {
        if (!IsRHID3D12())
        {
            OutError = TEXT("Current RHI is not D3D12");
            return false;
        }
        DynamicRHI = GetID3D12DynamicRHI();
        Device = DynamicRHI ? DynamicRHI->RHIGetDevice(0) : nullptr;
        if (!Device)
        {
            OutError = TEXT("ID3D12DynamicRHI::RHIGetDevice(0) returned null");
            return false;
        }

        HRESULT Hr = Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(Fence.GetAddressOf()));
        if (FAILED(Hr))
        {
            OutError = FString::Printf(TEXT("ID3D12Device::CreateFence failed: 0x%08x"), Hr);
            return false;
        }

        FenceValue = 0;
        NextHandleId = 1;
        return true;
    }

    virtual void Shutdown() override
    {
        for (auto& Kv : Handles)
        {
            if (Kv.Value.Resource) Kv.Value.Resource->Release();
        }
        Handles.Empty();
        Fence.Reset();
        Device = nullptr;
        DynamicRHI = nullptr;
    }

    virtual bool AllocSharedTexture(int32 Width, int32 Height, EGstZeroCopyFormat Format,
        FZeroCopyTextureHandle& OutHandle, FTextureRHIRef& OutRHITexture, FString& OutError) override
    {
        if (!Device || !DynamicRHI)
        {
            OutError = TEXT("Backend not initialized");
            return false;
        }

        D3D12_HEAP_PROPERTIES HeapProps{};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC Desc{};
        Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Alignment = 0;
        Desc.Width = static_cast<UINT64>(Width);
        Desc.Height = static_cast<UINT>(Height);
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels = 1;
        Desc.Format = ToDxgi(Format);
        Desc.SampleDesc.Count = 1;
        Desc.SampleDesc.Quality = 0;
        Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE ClearVal{};
        ClearVal.Format = Desc.Format;
        ClearVal.Color[0] = 0.0f;
        ClearVal.Color[1] = 0.0f;
        ClearVal.Color[2] = 0.0f;
        ClearVal.Color[3] = 1.0f;

        ID3D12Resource* Resource = nullptr;
        HRESULT Hr = Device->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_SHARED,
            &Desc,
            D3D12_RESOURCE_STATE_COMMON,
            &ClearVal,
            IID_PPV_ARGS(&Resource));
        if (FAILED(Hr) || !Resource)
        {
            OutError = FString::Printf(TEXT("CreateCommittedResource(SHARED) failed: 0x%08x"), Hr);
            return false;
        }

        OutRHITexture = DynamicRHI->RHICreateTexture2DFromResource(
            ToPixelFormat(Format),
            TexCreate_RenderTargetable | TexCreate_ShaderResource,
            FClearValueBinding::Black,
            Resource);
        if (!OutRHITexture.IsValid())
        {
            Resource->Release();
            OutError = TEXT("RHICreateTexture2DFromResource returned null");
            return false;
        }

        const uint64 Id = NextHandleId++;
        FEntry Entry;
        Entry.Resource = Resource;
        Entry.Width = Width;
        Entry.Height = Height;
        Entry.Format = Format;
        Handles.Add(Id, Entry);

        OutHandle.Id = Id;
        OutHandle.Native = Resource;
        return true;
    }

    virtual void FreeSharedTexture(FZeroCopyTextureHandle Handle) override
    {
        if (FEntry* Entry = Handles.Find(Handle.Id))
        {
            if (Entry->Resource) Entry->Resource->Release();
            Handles.Remove(Handle.Id);
        }
    }

    virtual uint64 SignalReady(FZeroCopyTextureHandle Handle) override
    {
        // TODO: signal Fence via RHISignalManualFence from a render command.
        return ++FenceValue;
    }

    virtual void* WrapAsGstMemory(FZeroCopyTextureHandle, uint64) override
    {
        // TODO next task: wrap via gst_d3d12_allocator_alloc_wrapped.
        return nullptr;
    }

private:
    struct FEntry
    {
        ID3D12Resource* Resource = nullptr;
        int32 Width = 0;
        int32 Height = 0;
        EGstZeroCopyFormat Format = EGstZeroCopyFormat::BGRA8;
    };

    ID3D12DynamicRHI* DynamicRHI = nullptr;
    ID3D12Device* Device = nullptr;
    ComPtr<ID3D12Fence> Fence;
    uint64 FenceValue = 0;

    uint64 NextHandleId = 1;
    TMap<uint64, FEntry> Handles;
};

IZeroCopyBackend* CreateD3D12ZeroCopyBackend()
{
    return new FD3D12ZeroCopyBackend();
}

#endif // PLATFORM_WINDOWS
