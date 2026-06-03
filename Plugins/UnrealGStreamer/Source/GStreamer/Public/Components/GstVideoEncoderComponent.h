#pragma once

#include "CoreMinimal.h"
#include "Components/GstElementComponent.h"
#include "GstVideoEncoderComponent.generated.h"

class IGstPipeline;

UENUM(BlueprintType)
enum class EGstEncoderRateControl : uint8
{
    Default UMETA(DisplayName = "Default (don't touch)"),
    CBR     UMETA(DisplayName = "CBR (constant bitrate)"),
    VBR     UMETA(DisplayName = "VBR (variable bitrate)"),
    CQP     UMETA(DisplayName = "CQP (constant quantizer)"),
};

/**
 * Applies common GObject properties (bitrate, gop-size, rc-mode) to a gst encoder element
 * that lives in the same pipeline. Element is looked up by ElementName ("enc" by default —
 * matches the name used by the encoder presets in UGstPipelineComponent).
 *
 * Works with d3d12h264enc / d3d12h265enc / d3d12av1enc and any other encoder exposing the
 * same property names. Missing properties produce a GStreamer warning but don't fail startup.
 */
UCLASS(ClassGroup = (GStreamer), meta = (BlueprintSpawnableComponent))
class GSTREAMER_API UGstVideoEncoderComponent : public UGstElementComponent
{
    GENERATED_BODY()

public:
    virtual void CbPipelineStart(IGstPipeline* InPipeline) override;
    virtual void CbPipelineStop() override;

    UPROPERTY(Category = "GStreamer|Encoder", EditAnywhere, BlueprintReadWrite)
    FString ElementName = TEXT("enc");

    UPROPERTY(Category = "GStreamer|Encoder", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0"))
    int32 BitrateKbps = 8000;

    UPROPERTY(Category = "GStreamer|Encoder", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0"))
    int32 KeyframeIntervalFrames = 60;

    UPROPERTY(Category = "GStreamer|Encoder", EditAnywhere, BlueprintReadWrite)
    EGstEncoderRateControl RateControl = EGstEncoderRateControl::Default;
};
