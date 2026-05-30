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

class FGstAppSrcBuffer : public IGstAppSrcBuffer
{
public:
    FGstAppSrcBuffer(UGstAppSrcComponent* InOwner) : Owner(InOwner)
    {
        ColorBuffer.Reserve(1920 * 1080);
    }

    virtual void Release() override { Owner->ReleaseBuffer(this); }
    virtual void* GetDataPtr() override { return ColorBuffer.GetData(); }
    virtual size_t GetDataSize() override { return ColorBuffer.Num() * sizeof(FColor); }

    UGstAppSrcComponent* Owner;
    TArray<FColor> ColorBuffer;
    FRenderCommandFence Fence;
    uint64 FrameId = 0;
    double ReadbackStartSeconds = 0.0;
    double ReadbackMs = 0.0;
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
    DestroyBuffers();
    if (Metrics) { delete Metrics; Metrics = nullptr; }
    LastTickWallSeconds = 0.0;
    FramesSinceLastLog = 0;
}

void UGstAppSrcComponent::CbPipelineStart(IGstPipeline* Pipeline)
{
    ResetState();

    if (!AppSrcEnabled || AppSrcName.IsEmpty())
    {
        return;
    }

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
            PushBufferAsync(TextureResource);
        }
    }

    if (BufferQueue.Num() > 0)
    {
        FGstAppSrcBuffer* Buffer = BufferQueue[0];
        if (Buffer->Fence.IsFenceComplete())
        {
            BufferQueue.RemoveAt(0);

            Buffer->ReadbackMs = (FPlatformTime::Seconds() - Buffer->ReadbackStartSeconds) * 1000.0;

            const double PushStart = FPlatformTime::Seconds();
            const bool bOk = AppSrc->PushBuffer(Buffer);
            const double PushMs = (FPlatformTime::Seconds() - PushStart) * 1000.0;

            if (bOk)
            {
                Metrics->FramesPushed.fetch_add(1, std::memory_order_relaxed);
            }

            FFrameTimings T;
            T.ReadbackMs = Buffer->ReadbackMs;
            T.PushMs = PushMs;
            T.FrameId = Buffer->FrameId;
            T.QueueDepthAtSubmit = Buffer->QueueDepthAtSubmit;
            Metrics->RecordFrame(T);
        }
    }

    Metrics->FrameIdAtomic.fetch_add(1, std::memory_order_relaxed);

    if (++FramesSinceLastLog >= MetricsLogIntervalFrames)
    {
        FramesSinceLastLog = 0;
        const auto S = Metrics->Summarize();
        if (S.SampleCount > 0)
        {
            UE_LOG(LogGStreamer, Verbose,
                TEXT("copy-path: fps=%.1f readback=%.2fms p95=%.2f push=%.2fms p95=%.2f queue=%d/%d pushed=%llu dropped=%llu"),
                S.Fps, S.ReadbackMeanMs, S.ReadbackP95Ms, S.PushMeanMs, S.PushP95Ms,
                S.MaxQueueDepth, MaxQueueLength,
                (unsigned long long)Metrics->FramesPushed.load(std::memory_order_relaxed),
                (unsigned long long)Metrics->FramesDropped.load(std::memory_order_relaxed));
        }
    }
}

void UGstAppSrcComponent::PushBufferAsync(FTextureRenderTargetResource* TextureResource)
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
    const FReadSurfaceDataFlags InFlags = FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX);

    FGstAppSrcBuffer* Buffer = GetBuffer();
    Buffer->ColorBuffer.SetNum(Size.X * Size.Y);
    Buffer->FrameId = Metrics != nullptr ? Metrics->FrameIdAtomic.load(std::memory_order_relaxed) : 0;
    Buffer->QueueDepthAtSubmit = BufferQueue.Num();
    Buffer->ReadbackStartSeconds = FPlatformTime::Seconds();
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
            RHICmdList.ReadSurfaceData(
                Context.SrcRenderTarget->GetRenderTargetTexture(),
                Context.Rect,
                Context.Buffer->ColorBuffer,
                Context.Flags);
        });

    Buffer->Fence.BeginFence();
}

FGstAppSrcBuffer* UGstAppSrcComponent::GetBuffer()
{
    {
        FScopeLock Lock(&PoolMx);
        if (BufferPool.Num() > 0)
        {
            return BufferPool.Pop();
        }
    }
    return new FGstAppSrcBuffer(this);
}

void UGstAppSrcComponent::ReleaseBuffer(FGstAppSrcBuffer* Buffer)
{
    FScopeLock Lock(&PoolMx);
    BufferPool.Add(Buffer);
}

void UGstAppSrcComponent::DestroyBuffers()
{
    if (BufferQueue.Num() > 0)
    {
        FlushRenderingCommands();
        for (FGstAppSrcBuffer* B : BufferQueue) delete B;
        BufferQueue.Reset();
    }
    {
        FScopeLock Lock(&PoolMx);
        for (FGstAppSrcBuffer* B : BufferPool) delete B;
        BufferPool.Reset();
    }
}
