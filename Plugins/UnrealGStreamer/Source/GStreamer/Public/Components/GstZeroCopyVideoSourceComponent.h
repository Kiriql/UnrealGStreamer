#pragma once

#include "CoreMinimal.h"
#include "Components/GstElementComponent.h"
#include "RHIResources.h"
#include "ZeroCopy/IZeroCopyBackend.h"
#include "GstZeroCopyVideoSourceComponent.generated.h"

class IGstPipeline;
class IGstAppSrc;
class UTextureRenderTarget2D;

UCLASS(ClassGroup = (GStreamer), meta = (BlueprintSpawnableComponent))
class GSTREAMER_API UGstZeroCopyVideoSourceComponent : public UGstElementComponent
{
    GENERATED_BODY()

public:
    UGstZeroCopyVideoSourceComponent();

    virtual void UninitializeComponent() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    virtual void CbPipelineStart(IGstPipeline* Pipeline) override;
    virtual void CbPipelineStop() override;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    FString AppSrcName = TEXT("ueapp");

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    bool AppSrcEnabled = true;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "64", ClampMax = "8192"))
    int32 Width = 1920;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "64", ClampMax = "8192"))
    int32 Height = 1080;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1", ClampMax = "120"))
    int32 FrameRate = 25;

    /** If set, wraps this RT's ID3D12Resource directly into GstMemory each frame (true zero-copy from SceneCapture).
     *  If null, falls back to a synthetic HSV clear for pipeline debugging. */
    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    TObjectPtr<UTextureRenderTarget2D> SourceRenderTarget = nullptr;

    UPROPERTY(Category = "GStreamer|Metrics", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1"))
    int32 MetricsLogIntervalFrames = 60;

    UFUNCTION(Category = "GStreamer", BlueprintCallable)
    void SetCaptureInterval(float Interval);

protected:
    void ResetState();

    IGstAppSrc* AppSrc = nullptr;

    FZeroCopyTextureHandle SharedHandle;
    FTextureRHIRef SharedTextureRHI;

    bool bCapsSet = false;
    int32 FramesSinceLastLog = 0;
    class FGstAppSrcMetrics* Metrics = nullptr;
};
