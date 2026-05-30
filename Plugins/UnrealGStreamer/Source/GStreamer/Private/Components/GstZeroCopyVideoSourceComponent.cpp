#include "Components/GstZeroCopyVideoSourceComponent.h"
#include "Pipeline/IGstAppSrc.h"
#include "Pipeline/IGstPipeline.h"
#include "Pipeline/GstSafeDestroy.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Core/GStreamerLog.h"
#include "ZeroCopy/IZeroCopyBackend.h"
#include "GstAppSrcMetrics.h"

#include "Components/SceneCaptureComponent2D.h"

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
    if (AppSrc) AppSrc->Disconnect();
    GstSafeDestroy(AppSrc);

    if (SharedRT)
    {
        SharedRT->ReleaseResource();
        SharedRT->MarkAsGarbage();
        SharedRT = nullptr;
    }

    if (Metrics) { delete Metrics; Metrics = nullptr; }
    bCapsSet = false;
    FramesSinceLastLog = 0;
}

void UGstZeroCopyVideoSourceComponent::EnsurePool()
{
    if (!SharedRT)
    {
        SharedRT = NewObject<UTextureRenderTarget2D>(this);
        SharedRT->ClearColor = FLinearColor::Black;
        SharedRT->bAutoGenerateMips = false;
        SharedRT->InitCustomFormat(Width, Height, PF_B8G8R8A8, true);
        // TODO: once D3D12 backend is real, hot-swap TextureRHI of SharedRT->GetResource() to our shared one.
    }
}

void UGstZeroCopyVideoSourceComponent::CbPipelineStart(IGstPipeline* Pipeline)
{
    ResetState();

    if (!AppSrcEnabled || AppSrcName.IsEmpty()) return;

    IZeroCopyBackend* Backend = IZeroCopyBackend::GetForCurrentPlatform();
    if (!Backend)
    {
        UE_LOG(LogGStreamer, Warning, TEXT("ZeroCopy backend unavailable — component idle. Use copy-path component instead."));
        return;
    }

    Metrics = new FGstAppSrcMetrics();

    AppSrc = IGstAppSrc::CreateInstance();
    const FTCHARToUTF8 NameUtf8(*AppSrcName);
    if (!AppSrc->Connect(Pipeline, NameUtf8.Get()))
    {
        UE_LOG(LogGStreamer, Error, TEXT("AppSrc '%s' not found in pipeline"), *AppSrcName);
        GstSafeDestroy(AppSrc);
        return;
    }

    EnsurePool();

    AActor* Owner = GetOwner();
    for (FComponentReference& Ref : AppSrcCaptures)
    {
        if (USceneCaptureComponent2D* Cap = Cast<USceneCaptureComponent2D>(Ref.GetComponent(Owner)))
        {
            Cap->TextureTarget = SharedRT;
        }
    }
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

    // TODO: signal fence, wrap GstMemory, push.
    // Skeleton stops here until D3D12Backend is real.
}
