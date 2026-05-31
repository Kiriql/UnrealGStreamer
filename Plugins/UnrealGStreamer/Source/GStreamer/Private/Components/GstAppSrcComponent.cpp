#include "Components/GstAppSrcComponent.h"
#include "Pipeline/IGstAppSrc.h"
#include "Pipeline/IGstPipeline.h"
#include "Pipeline/GstSafeDestroy.h"
#include "Core/GStreamerLog.h"
#include "GstAppSrcMetrics.h"

#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "Components/SceneCaptureComponent2D.h"
#include "RenderCommandFence.h"
#include "RHICommandList.h"

class FGstAppSrcBuffer;

// Shared state between component and any in-flight buffers gst still holds.
// Lives as TSharedPtr — component drops its ref on ResetState, gst-side
// buffers keep it alive until DestroyNotify fires.
struct FGstAppSrcBufferPool
{
    FCriticalSection Mx;
    bool bAlive = true;
    TArray<FGstAppSrcBuffer*> Pool;
};

class FGstAppSrcBuffer : public IGstAppSrcBuffer
{
public:
    FGstAppSrcBuffer(TSharedPtr<FGstAppSrcBufferPool, ESPMode::ThreadSafe> InPool)
        : SharedPool(MoveTemp(InPool))
    {
        ColorBuffer.Reserve(1920 * 1080);
    }

    virtual void Release() override
    {
        if (!SharedPool)
        {
            delete this;
            return;
        }
        bool bSelfDestruct = false;
        {
            FScopeLock Lock(&SharedPool->Mx);
            if (SharedPool->bAlive)
            {
                SharedPool->Pool.Add(this);
            }
            else
            {
                bSelfDestruct = true;
            }
        }
        if (bSelfDestruct)
        {
            // SharedPool member drops its ref inside the destructor — that runs after the lock
            // is released, so the FGstAppSrcBufferPool can be freed cleanly if we hold the last ref.
            delete this;
        }
    }

    virtual void* GetDataPtr() override { return ColorBuffer.GetData(); }
    virtual size_t GetDataSize() override { return ColorBuffer.Num() * sizeof(FColor); }

    TSharedPtr<FGstAppSrcBufferPool, ESPMode::ThreadSafe> SharedPool;
    TArray<FColor> ColorBuffer;
    FRenderCommandFence Fence;
    uint64 FrameId = 0;
    double SubmitWallSeconds = 0.0;
    double GpuStartSeconds = 0.0;
    double GpuEndSeconds = 0.0;
    int32 QueueDepthAtSubmit = 0;
};

UGstAppSrcComponent::UGstAppSrcComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 1.0f / 25.0f;
}

void UGstAppSrcComponent::UninitializeComponent()
{
    ResetState();
    Super::UninitializeComponent();
}

void UGstAppSrcComponent::ResetState()
{
    if (AppSrc)
    {
        AppSrc->Disconnect();
    }
    GstSafeDestroy(AppSrc);

    // BufferQueue entries are game-thread-owned (haven't been pushed to gst yet — fence still pending).
    // Safe to FlushRenderingCommands and delete directly.
    if (BufferQueue.Num() > 0)
    {
        FlushRenderingCommands();
        for (FGstAppSrcBuffer* B : BufferQueue) delete B;
        BufferQueue.Reset();
    }

    // Pool may still be referenced by gst-in-flight buffers. Mark dead and drain our copy;
    // any later Release() from gst will self-destruct via the bAlive check.
    if (Pool)
    {
        FScopeLock Lock(&Pool->Mx);
        Pool->bAlive = false;
        for (FGstAppSrcBuffer* B : Pool->Pool) delete B;
        Pool->Pool.Reset();
    }
    Pool.Reset();

    if (Metrics) { delete Metrics; Metrics = nullptr; }
    LastTickWallSeconds = 0.0;
    FramesSinceLastLog = 0;
    bCapsSet = false;
}

void UGstAppSrcComponent::CbPipelineStart(IGstPipeline* Pipeline)
{
    ResetState();

    if (!AppSrcEnabled || AppSrcName.IsEmpty())
    {
        return;
    }

    Pool = MakeShared<FGstAppSrcBufferPool, ESPMode::ThreadSafe>();
    Metrics = new FGstAppSrcMetrics();

    AppSrc = IGstAppSrc::CreateInstance();
    const FTCHARToUTF8 NameUtf8(*AppSrcName);
    if (!AppSrc->Connect(Pipeline, NameUtf8.Get()))
    {
        UE_LOG(LogGStreamer, Error, TEXT("AppSrc '%s' not found in pipeline"), *AppSrcName);
        GstSafeDestroy(AppSrc);
    }
}

void UGstAppSrcComponent::CbPipelineStop()
{
    ResetState();
}

void UGstAppSrcComponent::SetCaptureInterval(float Interval)
{
    PrimaryComponentTick.TickInterval = Interval;
}

void UGstAppSrcComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!AppSrc || !Metrics)
    {
        return;
    }

    const double NowSec = FPlatformTime::Seconds();
    Metrics->RecordWallTickSeconds(NowSec);
    LastTickWallSeconds = NowSec;

    const uint64 FrameIndex = Metrics->FrameIdAtomic.fetch_add(1, std::memory_order_relaxed) + 1;

    AActor* Owner = GetOwner();
    for (FComponentReference& ComponentReference : AppSrcCaptures)
    {
        USceneCaptureComponent2D* CaptureComponent = Cast<USceneCaptureComponent2D>(ComponentReference.GetComponent(Owner));
        if (!CaptureComponent) continue;

        UTextureRenderTarget2D* TextureTarget = CaptureComponent->TextureTarget;
        if (!TextureTarget) continue;

        FTextureRenderTargetResource* TextureResource = TextureTarget->GameThread_GetRenderTargetResource();
        if (TextureResource)
        {
            PushBufferAsync(TextureResource, FrameIndex);
        }
    }

    if (BufferQueue.Num() > 0)
    {
        FGstAppSrcBuffer* Buffer = BufferQueue[0];
        if (Buffer->Fence.IsFenceComplete())
        {
            BufferQueue.RemoveAt(0);

            const double NowEnd = FPlatformTime::Seconds();
            const double GpuMs = (Buffer->GpuEndSeconds > 0.0 && Buffer->GpuStartSeconds > 0.0)
                ? (Buffer->GpuEndSeconds - Buffer->GpuStartSeconds) * 1000.0
                : 0.0;
            const double E2EMs = (NowEnd - Buffer->SubmitWallSeconds) * 1000.0;

            const double PushStart = FPlatformTime::Seconds();
            const bool bOk = AppSrc->PushBuffer(Buffer);
            const double PushMs = (FPlatformTime::Seconds() - PushStart) * 1000.0;

            if (bOk)
            {
                Metrics->FramesPushed.fetch_add(1, std::memory_order_relaxed);
            }

            FFrameTimings T;
            T.GpuReadbackMs = GpuMs;
            T.EndToEndLatencyMs = E2EMs;
            T.PushMs = PushMs;
            T.FrameId = Buffer->FrameId;
            T.QueueDepthAtSubmit = Buffer->QueueDepthAtSubmit;
            Metrics->RecordFrame(T);
        }
    }

    if (++FramesSinceLastLog >= MetricsLogIntervalFrames)
    {
        FramesSinceLastLog = 0;
        const auto S = Metrics->Summarize();
        if (S.SampleCount > 0)
        {
            UE_LOG(LogGStreamer, Log,
                TEXT("copy-path: fps=%.1f gpu=%.2fms p95=%.2f e2e=%.2fms p95=%.2f push=%.2fms p95=%.2f queue=%d/%d pushed=%llu dropped=%llu"),
                S.Fps, S.GpuReadbackMeanMs, S.GpuReadbackP95Ms,
                S.EndToEndMeanMs, S.EndToEndP95Ms,
                S.PushMeanMs, S.PushP95Ms,
                S.MaxQueueDepth, MaxQueueLength,
                (unsigned long long)Metrics->FramesPushed.load(std::memory_order_relaxed),
                (unsigned long long)Metrics->FramesDropped.load(std::memory_order_relaxed));
        }
    }
}

void UGstAppSrcComponent::PushBufferAsync(FTextureRenderTargetResource* TextureResource, uint64 FrameIndex)
{
    if (BufferQueue.Num() >= MaxQueueLength)
    {
        if (Metrics != nullptr)
        {
            Metrics->FramesDropped.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    const FIntPoint Size = TextureResource->GetSizeXY();
    const FIntRect InRect = FIntRect(0, 0, Size.X, Size.Y);

    if (!bCapsSet)
    {
        const float TickInterval = PrimaryComponentTick.TickInterval > 0.0f ? PrimaryComponentTick.TickInterval : (1.0f / 25.0f);
        const int32 FpsNum = FMath::Max(1, FMath::RoundToInt(1.0f / TickInterval));
        FString CapsStr = FString::Printf(TEXT("video/x-raw,format=BGRA,width=%d,height=%d,framerate=%d/1"),
            Size.X, Size.Y, FpsNum);
        const FTCHARToUTF8 CapsUtf8(*CapsStr);
        if (AppSrc->SetCaps(CapsUtf8.Get()))
        {
            UE_LOG(LogGStreamer, Log, TEXT("AppSrc caps set: %s"), *CapsStr);
            bCapsSet = true;
        }
        else
        {
            UE_LOG(LogGStreamer, Error, TEXT("AppSrc SetCaps failed (giving up): %s"), *CapsStr);
            bCapsSet = true; // don't retry every frame
        }
    }
    const FReadSurfaceDataFlags InFlags = FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX);

    FGstAppSrcBuffer* Buffer = AcquireBuffer();
    Buffer->ColorBuffer.SetNum(Size.X * Size.Y);
    Buffer->FrameId = FrameIndex;
    Buffer->QueueDepthAtSubmit = BufferQueue.Num();
    Buffer->SubmitWallSeconds = FPlatformTime::Seconds();
    Buffer->GpuStartSeconds = 0.0;
    Buffer->GpuEndSeconds = 0.0;
    BufferQueue.Add(Buffer);

    struct FReadSurfaceContext
    {
        FGstAppSrcBuffer* Buffer;
        FRenderTarget* SrcRenderTarget;
        FIntRect Rect;
        FReadSurfaceDataFlags Flags;
    };

    FReadSurfaceContext Context{ Buffer, TextureResource, InRect, InFlags };

    ENQUEUE_RENDER_COMMAND(GstReadSurfaceCommand)(
        [Context](FRHICommandListImmediate& RHICmdList)
        {
            Context.Buffer->GpuStartSeconds = FPlatformTime::Seconds();
            RHICmdList.ReadSurfaceData(
                Context.SrcRenderTarget->GetRenderTargetTexture(),
                Context.Rect,
                Context.Buffer->ColorBuffer,
                Context.Flags);
            Context.Buffer->GpuEndSeconds = FPlatformTime::Seconds();
        });

    Buffer->Fence.BeginFence();
}

FGstAppSrcBuffer* UGstAppSrcComponent::AcquireBuffer()
{
    if (Pool)
    {
        FScopeLock Lock(&Pool->Mx);
        if (Pool->Pool.Num() > 0)
        {
            return Pool->Pool.Pop();
        }
    }
    return new FGstAppSrcBuffer(Pool);
}
