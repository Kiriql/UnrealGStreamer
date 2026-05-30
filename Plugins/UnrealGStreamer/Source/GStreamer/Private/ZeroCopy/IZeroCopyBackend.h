#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"

enum class EGstZeroCopyFormat : uint8
{
    BGRA8 = 0,
    RGBA8 = 1,
};

struct FZeroCopyTextureHandle
{
    uint64 Id = 0;
    void* Native = nullptr;

    bool IsValid() const { return Id != 0; }
};

class IZeroCopyBackend
{
public:
    static IZeroCopyBackend* GetForCurrentPlatform();

    virtual ~IZeroCopyBackend() = default;

    virtual const TCHAR* GetBackendName() const = 0;
    virtual bool Init(FString& OutError) = 0;
    virtual void Shutdown() = 0;

    virtual bool AllocSharedTexture(
        int32 Width, int32 Height, EGstZeroCopyFormat Format,
        FZeroCopyTextureHandle& OutHandle,
        FTextureRHIRef& OutRHITexture,
        FString& OutError) = 0;

    virtual void FreeSharedTexture(FZeroCopyTextureHandle Handle) = 0;

    virtual uint64 SignalReady(FZeroCopyTextureHandle Handle) = 0;

    virtual void* WrapAsGstMemory(FZeroCopyTextureHandle Handle, uint64 FenceValue) = 0;

    virtual void* WrapExternalTextureAsGstMemory(class FRHITexture* Texture) = 0;

    /** Wraps an externally-owned FRHITexture and attaches a GPU fence the gst-side queue
     *  will wait on before reading. Returns GstMemory* (opaque). */
    virtual void* WrapExternalTextureAsGstMemoryWithFence(
        class FRHITexture* Texture, class FRHICommandListImmediate& RHICmdList) = 0;
};
