#pragma once

#include "CoreMinimal.h"
#include "Components/GstElementComponent.h"
#include "GstAppSrcComponent.generated.h"

class IGstPipeline;
class IGstAppSrc;
class FGstAppSrcBuffer;
class FTextureRenderTargetResource;

UCLASS(ClassGroup = (GStreamer), meta = (BlueprintSpawnableComponent))
class GSTREAMER_API UGstAppSrcComponent : public UGstElementComponent
{
    GENERATED_BODY()

public:
    UGstAppSrcComponent();

    virtual void UninitializeComponent() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    virtual void CbPipelineStart(IGstPipeline* Pipeline) override;
    virtual void CbPipelineStop() override;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    FString AppSrcName = TEXT("ueapp");

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite)
    bool AppSrcEnabled = true;

    UPROPERTY(Category = "GStreamer", EditAnywhere)
    TArray<FComponentReference> AppSrcCaptures;

    UPROPERTY(Category = "GStreamer", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1", ClampMax = "10"))
    int32 MaxQueueLength = 5;

    UPROPERTY(Category = "GStreamer|Metrics", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1"))
    int32 MetricsLogIntervalFrames = 60;

    UFUNCTION(Category = "GStreamer", BlueprintCallable)
    void SetCaptureInterval(float Interval);

protected:
    void ResetState();
    void PushBufferAsync(FTextureRenderTargetResource* TextureResource);

    friend class FGstAppSrcBuffer;
    FGstAppSrcBuffer* GetBuffer();
    void ReleaseBuffer(FGstAppSrcBuffer* Buffer);
    void DestroyBuffers();

    IGstAppSrc* AppSrc = nullptr;
    TArray<FGstAppSrcBuffer*> BufferPool;
    TArray<FGstAppSrcBuffer*> BufferQueue;
    FCriticalSection PoolMx;

    class FGstAppSrcMetrics* Metrics = nullptr;
    double LastTickWallSeconds = 0.0;
    int32 FramesSinceLastLog = 0;
};
