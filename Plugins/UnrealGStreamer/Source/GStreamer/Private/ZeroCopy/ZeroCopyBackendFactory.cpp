#include "IZeroCopyBackend.h"
#include "Core/GStreamerLog.h"
#include "DynamicRHI.h"
#include "RHI.h"

#if PLATFORM_WINDOWS
extern IZeroCopyBackend* CreateD3D12ZeroCopyBackend();
#endif

IZeroCopyBackend* IZeroCopyBackend::GetForCurrentPlatform()
{
    static IZeroCopyBackend* Cached = nullptr;
    static bool bTried = false;
    if (bTried) return Cached;
    bTried = true;

#if PLATFORM_WINDOWS
    if (FString(GDynamicRHI ? GDynamicRHI->GetName() : TEXT("")).Contains(TEXT("D3D12")))
    {
        Cached = CreateD3D12ZeroCopyBackend();
        if (Cached)
        {
            FString Err;
            if (!Cached->Init(Err))
            {
                UE_LOG(LogGStreamer, Warning, TEXT("ZeroCopy backend init failed: %s"), *Err);
                delete Cached;
                Cached = nullptr;
            }
            else
            {
                UE_LOG(LogGStreamer, Log, TEXT("ZeroCopy backend: %s"), Cached->GetBackendName());
            }
        }
    }
    else
    {
        UE_LOG(LogGStreamer, Log, TEXT("ZeroCopy: D3D12 RHI required on Windows, current RHI is not D3D12 — falling back to copy path"));
    }
#else
    UE_LOG(LogGStreamer, Log, TEXT("ZeroCopy: backend not implemented for this platform yet — falling back to copy path"));
#endif

    return Cached;
}
