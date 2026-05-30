#include "Components/GstZeroCopyVideoSourceComponent.h"
#include "Pipeline/IGstAppSrc.h"
#include "Pipeline/IGstPipeline.h"
#include "Pipeline/GstSafeDestroy.h"
#include "Core/GStreamerLog.h"
#include "ZeroCopy/IZeroCopyBackend.h"
#include "GstAppSrcMetrics.h"

#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "ClearQuad.h"

UGstZeroCopyVideoSourceComponent::UGstZeroCopyVideoSourceComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickInterval = 1.0f / 25.0f;
}

void UGstZeroCopyVideoSourceComponent::UninitializeComponent()
{
    ResetState();
    Super::UninitializeComponent();
}

void UGstZeroCopyVideoSourceComponent::ResetState()
{
    FlushRenderingCommands();

    if (AppSrc) AppSrc->Disconnect();
    GstSafeDestroy(AppSrc);

    if (SharedHandle.IsValid())
    {
        if (IZeroCopyBackend* Backend = IZeroCopyBackend::GetForCurrentPlatform())
        {
            Backend->FreeSharedTexture(SharedHandle);
        }
        SharedHandle = {};
    }
    SharedTextureRHI.SafeRelease();

    if (Metrics) { delete Metrics; Metrics = nullptr; }
    bCapsSet = false;
    FramesSinceLastLog = 0;
}

void UGstZeroCopyVideoSourceComponent::CbPipelineStart(IGstPipeline* Pipeline)
{
    ResetState();

    if (!AppSrcEnabled || AppSrcName.IsEmpty()) return;

    IZeroCopyBackend* Backend = IZeroCopyBackend::GetForCurrentPlatform();
    if (!Backend)
    {
        UE_LOG(LogGStreamer, Warning, TEXT("ZeroCopy backend unavailable — component idle."));
        return;
    }

    const bool bUseSource = (SourceRenderTarget != nullptr);

    if (bUseSource)
    {
        Width = SourceRenderTarget->SizeX;
        Height = SourceRenderTarget->SizeY;
    }

    {
        FString Err;
        if (!Backend->AllocSharedTexture(Width, Height, EGstZeroCopyFormat::BGRA8, SharedHandle, SharedTextureRHI, Err))
        {
            UE_LOG(LogGStreamer, Error, TEXT("AllocSharedTexture failed: %s"), *Err);
            return;
        }
    }

    AppSrc = IGstAppSrc::CreateInstance();
    const FTCHARToUTF8 NameUtf8(*AppSrcName);
    if (!AppSrc->Connect(Pipeline, NameUtf8.Get()))
    {
        UE_LOG(LogGStreamer, Error, TEXT("AppSrc '%s' not found in pipeline"), *AppSrcName);
        GstSafeDestroy(AppSrc);
        if (SharedHandle.IsValid()) { Backend->FreeSharedTexture(SharedHandle); SharedHandle = {}; }
        SharedTextureRHI.SafeRelease();
        return;
    }

    const FString Caps = FString::Printf(
        TEXT("video/x-raw(memory:D3D12Memory),format=BGRA,width=%d,height=%d,framerate=%d/1"),
        Width, Height, FrameRate);
    const FTCHARToUTF8 CapsUtf8(*Caps);
    if (!AppSrc->SetCaps(CapsUtf8.Get()))
    {
        UE_LOG(LogGStreamer, Error, TEXT("SetCaps failed for: %s"), *Caps);
    }
    else
    {
        bCapsSet = true;
        UE_LOG(LogGStreamer, Log, TEXT("ZeroCopy '%s' caps=%s mode=%s"),
            *AppSrcName, *Caps, bUseSource ? TEXT("SourceRT") : TEXT("Synthetic"));
    }

    Metrics = new FGstAppSrcMetrics();
    PrimaryComponentTick.TickInterval = 1.0f / FMath::Max(1, FrameRate);
}

void UGstZeroCopyVideoSourceComponent::CbPipelineStop()
{
    ResetState();
}

void UGstZeroCopyVideoSourceComponent::SetCaptureInterval(float Interval)
{
    PrimaryComponentTick.TickInterval = Interval;
}

void UGstZeroCopyVideoSourceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!AppSrc || !Metrics) return;

    IGstAppSrc* AppSrcCopy = AppSrc;
    const uint64 FrameIndex = Metrics->FrameIdAtomic.fetch_add(1) + 1;

    if (!SharedTextureRHI.IsValid() || !SharedHandle.IsValid()) return;

    FZeroCopyTextureHandle HandleCopy = SharedHandle;
    FTextureRHIRef DstTex = SharedTextureRHI;
    const int32 W = Width;
    const int32 H = Height;

    if (SourceRenderTarget)
    {
        FTextureResource* SrcRes = SourceRenderTarget->GetResource();
        if (!SrcRes) return;
        FTextureRHIRef SrcTex = SrcRes->GetTextureRHI();
        if (!SrcTex.IsValid()) return;

        ENQUEUE_RENDER_COMMAND(GstZcCopyAndPush)(
            [AppSrcCopy, HandleCopy, SrcTex, DstTex](FRHICommandListImmediate& RHICmdList)
            {
                FRHITransitionInfo Pre[] = {
                    FRHITransitionInfo(SrcTex, ERHIAccess::SRVMask, ERHIAccess::CopySrc),
                    FRHITransitionInfo(DstTex, ERHIAccess::Unknown,  ERHIAccess::CopyDest),
                };
                RHICmdList.Transition(MakeArrayView(Pre, 2));

                RHICmdList.CopyTexture(SrcTex, DstTex, FRHICopyTextureInfo());

                FRHITransitionInfo Post[] = {
                    FRHITransitionInfo(DstTex, ERHIAccess::CopyDest, ERHIAccess::CopySrc),
                    FRHITransitionInfo(SrcTex, ERHIAccess::CopySrc,  ERHIAccess::SRVMask),
                };
                RHICmdList.Transition(MakeArrayView(Post, 2));

                IZeroCopyBackend* Backend = IZeroCopyBackend::GetForCurrentPlatform();
                if (!Backend) return;
                void* GstMem = Backend->WrapExternalTextureAsGstMemoryWithFence(DstTex.GetReference(), RHICmdList);
                if (!GstMem) return;
                AppSrcCopy->PushSharedBuffer(GstMem);
            });
    }
    else
    {
        ENQUEUE_RENDER_COMMAND(GstZcClearAndPush)(
            [AppSrcCopy, HandleCopy, DstTex, W, H, FrameIndex](FRHICommandListImmediate& RHICmdList)
            {
                const float Hue = FMath::Fmod(FrameIndex / 60.0f, 1.0f);
                const FLinearColor ClearColor = FLinearColor::MakeFromHSV8(
                    (uint8)(Hue * 255.0f), 200, 220);

                RHICmdList.Transition(FRHITransitionInfo(DstTex, ERHIAccess::Unknown, ERHIAccess::RTV));
                FRHIRenderPassInfo RPInfo(DstTex, ERenderTargetActions::DontLoad_Store);
                RHICmdList.BeginRenderPass(RPInfo, TEXT("GstZcClear"));
                RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, (float)W, (float)H, 1.0f);
                DrawClearQuad(RHICmdList, ClearColor);
                RHICmdList.EndRenderPass();
                RHICmdList.Transition(FRHITransitionInfo(DstTex, ERHIAccess::RTV, ERHIAccess::CopySrc));

                IZeroCopyBackend* Backend = IZeroCopyBackend::GetForCurrentPlatform();
                if (!Backend) return;
                void* GstMem = Backend->WrapAsGstMemory(HandleCopy, 0);
                if (!GstMem) return;
                AppSrcCopy->PushSharedBuffer(GstMem);
            });
    }

    Metrics->FramesPushed.fetch_add(1);
    if (++FramesSinceLastLog >= MetricsLogIntervalFrames)
    {
        UE_LOG(LogGStreamer, Log, TEXT("zerocopy: pushed=%llu frame=%llu"),
            (unsigned long long)Metrics->FramesPushed.load(),
            (unsigned long long)FrameIndex);
        FramesSinceLastLog = 0;
    }
}
