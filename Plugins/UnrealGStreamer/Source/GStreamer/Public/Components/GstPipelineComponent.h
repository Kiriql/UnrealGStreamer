#pragma once

#include "CoreMinimal.h"
#include "Components/GstElementComponent.h"
#include "GstPipelineComponent.generated.h"

class IGstPipeline;

UENUM(BlueprintType)
enum class EGstPipelinePreset : uint8
{
    Custom            UMETA(DisplayName = "Custom (write PipelineConfig)"),
    Display_D3D12     UMETA(DisplayName = "Display: BGRA -> d3d12videosink"),
    H264_Fakesink     UMETA(DisplayName = "Encode: H.264 -> fakesink (benchmark)"),
    H264_FileMp4      UMETA(DisplayName = "Encode: H.264 -> MP4 file"),
    H264_UdpRtp       UMETA(DisplayName = "Encode: H.264 -> UDP/RTP (127.0.0.1:5000)"),
};

UENUM(BlueprintType)
enum class EGstEncoderRateControl : uint8
{
    Default UMETA(DisplayName = "Default (don't touch)"),
    CBR     UMETA(DisplayName = "CBR (constant bitrate)"),
    VBR     UMETA(DisplayName = "VBR (variable bitrate)"),
    CQP     UMETA(DisplayName = "CQP (constant quantizer)"),
};

UCLASS(ClassGroup = (GStreamer), meta = (BlueprintSpawnableComponent))
class GSTREAMER_API UGstPipelineComponent : public UGstElementComponent
{
    GENERATED_BODY()

public:
    UGstPipelineComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void UninitializeComponent() override;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    EGstPipelinePreset Preset = EGstPipelinePreset::Custom;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite, meta = (MultiLine = true,
        EditCondition = "Preset == EGstPipelinePreset::Custom", EditConditionHides))
    FString PipelineConfig = TEXT("appsrc name=ueapp is-live=true format=time ! videoconvert ! d3d12videosink sync=false");

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite,
        meta = (EditCondition = "Preset == EGstPipelinePreset::H264_FileMp4"))
    FString FileOutputPath = TEXT("ue_stream.mp4");

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    bool PipelineAutostart = true;

    /** Name of the encoder element in the pipeline. Encoder presets use "enc"; for Custom set this to your encoder's name. Empty = skip encoder configuration. */
    UPROPERTY(Category = "GStreamer|Encoder", EditAnywhere, BlueprintReadWrite)
    FString EncoderElementName = TEXT("enc");

    UPROPERTY(Category = "GStreamer|Encoder", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0"))
    int32 BitrateKbps = 8000;

    UPROPERTY(Category = "GStreamer|Encoder", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0"))
    int32 KeyframeIntervalFrames = 60;

    UPROPERTY(Category = "GStreamer|Encoder", EditAnywhere, BlueprintReadWrite)
    EGstEncoderRateControl RateControl = EGstEncoderRateControl::Default;

    UFUNCTION(Category = "GStreamer", BlueprintCallable)
    bool StartPipeline();

    UFUNCTION(Category = "GStreamer", BlueprintCallable)
    void StopPipeline();

    /** Resolves Preset + parameters to a gst-parse-launch string. Public for tooling/inspection. */
    FString ResolvePipelineString() const;

protected:
    void ResetState();

    IGstPipeline* Pipeline = nullptr;
};
