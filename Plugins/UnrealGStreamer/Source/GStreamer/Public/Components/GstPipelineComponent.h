#pragma once

#include "CoreMinimal.h"
#include "Components/GstElementComponent.h"
#include "GstPipelineComponent.generated.h"

class IGstPipeline;

UCLASS(ClassGroup = (GStreamer), meta = (BlueprintSpawnableComponent))
class GSTREAMER_API UGstPipelineComponent : public UGstElementComponent
{
    GENERATED_BODY()

public:
    UGstPipelineComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void UninitializeComponent() override;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite, meta = (MultiLine = true))
    FString PipelineConfig = TEXT("appsrc name=ueapp is-live=true format=time ! videoconvert ! d3d11videosink sync=false");

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    bool PipelineAutostart = true;

    UFUNCTION(Category = "GStreamer", BlueprintCallable)
    bool StartPipeline();

    UFUNCTION(Category = "GStreamer", BlueprintCallable)
    void StopPipeline();

protected:
    void ResetState();

    IGstPipeline* Pipeline = nullptr;
};
