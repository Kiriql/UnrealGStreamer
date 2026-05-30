#include "Components/GstPipelineComponent.h"
#include "Pipeline/IGstPipeline.h"
#include "Pipeline/GstSafeDestroy.h"
#include "Core/GStreamerLog.h"

#include "GameFramework/Actor.h"

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

bool UGstPipelineComponent::StartPipeline()
{
    if (Pipeline)
    {
        UE_LOG(LogGStreamer, Warning, TEXT("Pipeline '%s' already started"), *PipelineName);
        return false;
    }
    if (PipelineConfig.IsEmpty())
    {
        UE_LOG(LogGStreamer, Error, TEXT("Pipeline '%s' has empty PipelineConfig"), *PipelineName);
        return false;
    }

    Pipeline = IGstPipeline::CreateInstance();

    char ErrBuf[512] = {0};
    const FTCHARToUTF8 NameUtf8(*PipelineName);
    const FTCHARToUTF8 ConfigUtf8(*PipelineConfig);

    if (!Pipeline->Init(NameUtf8.Get(), ConfigUtf8.Get(), ErrBuf, sizeof(ErrBuf)))
    {
        UE_LOG(LogGStreamer, Error, TEXT("gst_parse_launch failed: %s"), UTF8_TO_TCHAR(ErrBuf));
        GstSafeDestroy(Pipeline);
        return false;
    }

    if (AActor* Owner = GetOwner())
    {
        TInlineComponentArray<UGstElementComponent*> Components;
        Owner->GetComponents(Components);
        for (UGstElementComponent* Comp : Components)
        {
            if (Comp->PipelineName == PipelineName)
            {
                Comp->CbPipelineStart(Pipeline);
            }
        }
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
    if (Pipeline)
    {
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
    }
    ResetState();
}
