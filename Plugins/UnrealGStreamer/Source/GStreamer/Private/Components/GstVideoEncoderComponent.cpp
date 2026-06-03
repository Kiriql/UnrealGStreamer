#include "Components/GstVideoEncoderComponent.h"
#include "Pipeline/IGstPipeline.h"
#include "Core/GStreamerLog.h"

extern "C" int GstEncoderApplySettings(
    void* PipelineRaw,
    const char* ElementName,
    int BitrateKbps,
    int GopSize,
    int RateControlMode);

void UGstVideoEncoderComponent::CbPipelineStart(IGstPipeline* InPipeline)
{
    if (!InPipeline) return;

    const FTCHARToUTF8 NameUtf8(*ElementName);
    const int Rc = (int)RateControl;
    const int Ret = GstEncoderApplySettings(
        InPipeline->GetGPipeline(),
        NameUtf8.Get(),
        BitrateKbps,
        KeyframeIntervalFrames,
        Rc);

    if (Ret == -1)
    {
        UE_LOG(LogGStreamer, Warning,
            TEXT("VideoEncoder: element '%s' not found in pipeline '%s'"),
            *ElementName, *PipelineName);
    }
    else
    {
        UE_LOG(LogGStreamer, Log,
            TEXT("VideoEncoder '%s' configured: bitrate=%dkbps gop=%d rc=%d"),
            *ElementName, BitrateKbps, KeyframeIntervalFrames, Rc);
    }
}

void UGstVideoEncoderComponent::CbPipelineStop()
{
}
