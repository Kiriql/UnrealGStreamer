#include "Components/GstPipelineComponent.h"
#include "Pipeline/IGstPipeline.h"
#include "Pipeline/GstSafeDestroy.h"
#include "Core/GStreamerLog.h"

#include "GameFramework/Actor.h"
#include "RenderingThread.h"

extern "C" int GstEncoderApplySettings(
    void* PipelineRaw,
    const char* ElementName,
    int BitrateKbps,
    int GopSize,
    int RateControlMode);

UGstPipelineComponent::UGstPipelineComponent()
{
}

void UGstPipelineComponent::BeginPlay()
{
    Super::BeginPlay();
    if (PipelineAutostart)
    {
        StartPipeline();
    }
}

void UGstPipelineComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopPipeline();
    Super::EndPlay(EndPlayReason);
}

void UGstPipelineComponent::UninitializeComponent()
{
    ResetState();
    Super::UninitializeComponent();
}

void UGstPipelineComponent::ResetState()
{
    GstSafeDestroy(Pipeline);
}

FString UGstPipelineComponent::ResolvePipelineString() const
{
    // is-live=true is only meaningful for real-time streaming (UDP/RTP / live display).
    // For file recording it causes the pipeline to use a real-time clock and changes
    // EOS / pre-roll behavior, which breaks muxer finalization. Use a per-preset source.
    const TCHAR* SrcLive    = TEXT("appsrc name=ueapp is-live=true format=time");
    const TCHAR* SrcNonLive = TEXT("appsrc name=ueapp format=time");
    const TCHAR* Src = SrcNonLive;

    switch (Preset)
    {
    case EGstPipelinePreset::Custom:
        return PipelineConfig;

    case EGstPipelinePreset::Display_D3D12:
        return FString::Printf(TEXT("%s ! videoconvert ! d3d12videosink sync=false"), SrcLive);

    case EGstPipelinePreset::H264_UdpRtp:
        return FString::Printf(
            TEXT("%s ! d3d12convert ! d3d12h264enc name=enc ! h264parse config-interval=1 ! rtph264pay pt=96 ! udpsink host=%s port=%d sync=false"),
            SrcLive, *StreamHost, StreamPort);

    case EGstPipelinePreset::H264_Fakesink:
        return FString::Printf(TEXT("%s ! d3d12convert ! d3d12h264enc name=enc ! h264parse ! fakesink sync=false"), Src);

    case EGstPipelinePreset::H264_FileMp4:
        return FString::Printf(
            TEXT("%s ! d3d12convert ! d3d12h264enc name=enc ! h264parse ! mp4mux ! filesink location=\"%s\""),
            Src, *FileOutputPath);
    }
    return PipelineConfig;
}

bool UGstPipelineComponent::StartPipeline()
{
    if (Pipeline)
    {
        UE_LOG(LogGStreamer, Warning, TEXT("Pipeline '%s' already started"), *PipelineName);
        return false;
    }

    const FString Resolved = ResolvePipelineString();
    if (Resolved.IsEmpty())
    {
        UE_LOG(LogGStreamer, Error, TEXT("Pipeline '%s' resolved to empty string (Preset=%d)"),
            *PipelineName, (int32)Preset);
        return false;
    }

    UE_LOG(LogGStreamer, Log, TEXT("Pipeline '%s' launching: %s"), *PipelineName, *Resolved);

    Pipeline = IGstPipeline::CreateInstance();

    char ErrBuf[512] = {0};
    const FTCHARToUTF8 NameUtf8(*PipelineName);
    const FTCHARToUTF8 ConfigUtf8(*Resolved);

    if (!Pipeline->Init(NameUtf8.Get(), ConfigUtf8.Get(), ErrBuf, sizeof(ErrBuf)))
    {
        UE_LOG(LogGStreamer, Error, TEXT("gst_parse_launch failed: %s"), UTF8_TO_TCHAR(ErrBuf));
        GstSafeDestroy(Pipeline);
        return false;
    }

    if (!EncoderElementName.IsEmpty())
    {
        const FTCHARToUTF8 EncNameUtf8(*EncoderElementName);
        const int Rc = (int)RateControl;
        const int Ret = GstEncoderApplySettings(
            Pipeline->GetGPipeline(), EncNameUtf8.Get(),
            BitrateKbps, KeyframeIntervalFrames, Rc);
        if (Ret == 0)
        {
            UE_LOG(LogGStreamer, Log,
                TEXT("Encoder '%s' configured: bitrate=%dkbps gop=%d rc=%d"),
                *EncoderElementName, BitrateKbps, KeyframeIntervalFrames, Rc);
        }
        else if (Preset != EGstPipelinePreset::Custom && Preset != EGstPipelinePreset::Display_D3D12)
        {
            UE_LOG(LogGStreamer, Warning,
                TEXT("Encoder element '%s' not found in pipeline '%s'"),
                *EncoderElementName, *PipelineName);
        }
    }

    int32 BoundCount = 0;
    if (AActor* Owner = GetOwner())
    {
        TInlineComponentArray<UGstElementComponent*> Components;
        Owner->GetComponents(Components);
        for (UGstElementComponent* Comp : Components)
        {
            if (Comp == this) continue;
            if (Comp->PipelineName == PipelineName)
            {
                Comp->CbPipelineStart(Pipeline);
                ++BoundCount;
            }
        }
    }
    if (BoundCount == 0)
    {
        UE_LOG(LogGStreamer, Warning,
            TEXT("Pipeline '%s' started with 0 bound GstElementComponents on the owning actor. "
                 "Check that AppSrc/AppSink components have matching PipelineName."),
            *PipelineName);
    }

    if (!Pipeline->Start())
    {
        UE_LOG(LogGStreamer, Error, TEXT("Pipeline '%s' failed to start"), *PipelineName);
        StopPipeline();
        return false;
    }

    UE_LOG(LogGStreamer, Log, TEXT("Pipeline '%s' started"), *PipelineName);
    return true;
}

void UGstPipelineComponent::StopPipeline()
{
    if (!Pipeline) return;

    UE_LOG(LogGStreamer, Log, TEXT("Pipeline '%s' stopping (sending EOS, waiting for drain)..."), *PipelineName);

    // Ensure render thread has finished pushing any in-flight buffers into appsrc before EOS,
    // otherwise EOS races a buffer that gets dropped by appsrc-already-EOS'd.
    FlushRenderingCommands();

    const double T0 = FPlatformTime::Seconds();
    const int DrainResult = Pipeline->SendEosAndWaitDrain(5000);
    const double DrainMs = (FPlatformTime::Seconds() - T0) * 1000.0;
    switch (DrainResult)
    {
    case 1:
        UE_LOG(LogGStreamer, Log, TEXT("Pipeline '%s' EOS drained in %.0fms"), *PipelineName, DrainMs);
        break;
    case 0:
        UE_LOG(LogGStreamer, Warning, TEXT("Pipeline '%s' EOS wait timed out after %.0fms — muxer output may be truncated"),
            *PipelineName, DrainMs);
        break;
    default:
        UE_LOG(LogGStreamer, Warning, TEXT("Pipeline '%s' EOS drain skipped/failed (%.0fms)"), *PipelineName, DrainMs);
        break;
    }

    if (AActor* Owner = GetOwner())
    {
        TInlineComponentArray<UGstElementComponent*> Components;
        Owner->GetComponents(Components);
        for (UGstElementComponent* Comp : Components)
        {
            if (Comp->PipelineName == PipelineName)
            {
                Comp->CbPipelineStop();
            }
        }
    }
    ResetState();
    UE_LOG(LogGStreamer, Log, TEXT("Pipeline '%s' stopped"), *PipelineName);
}
