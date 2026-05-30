#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GstElementComponent.generated.h"

class IGstPipeline;

UCLASS(ClassGroup = (GStreamer), Abstract)
class GSTREAMER_API UGstElementComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UGstElementComponent();

    virtual void CbPipelineStart(IGstPipeline* Pipeline) {}
    virtual void CbPipelineStop() {}

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    FString PipelineName;
};
