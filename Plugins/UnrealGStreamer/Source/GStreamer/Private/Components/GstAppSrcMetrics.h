#pragma once

#include "CoreMinimal.h"

#include <atomic>

struct FFrameTimings
{
    double GpuReadbackMs = 0.0;
    double EndToEndLatencyMs = 0.0;
    double PushMs = 0.0;
    uint64 FrameId = 0;
    int32 QueueDepthAtSubmit = 0;
};

class FGstAppSrcMetrics
{
public:
    static constexpr int32 WindowSize = 60;

    void RecordFrame(const FFrameTimings& Timings)
    {
        FScopeLock Lock(&Mx);
        if (Window.Num() >= WindowSize)
        {
            Window.RemoveAt(0);
        }
        Window.Add(Timings);
    }

    void RecordWallTickSeconds(double Seconds)
    {
        FScopeLock Lock(&Mx);
        if (WallTickSeconds.Num() >= WindowSize)
        {
            WallTickSeconds.RemoveAt(0);
        }
        WallTickSeconds.Add(Seconds);
    }

    struct FSummary
    {
        double GpuReadbackMeanMs = 0.0;
        double GpuReadbackP95Ms = 0.0;
        double EndToEndMeanMs = 0.0;
        double EndToEndP95Ms = 0.0;
        double PushMeanMs = 0.0;
        double PushP95Ms = 0.0;
        double Fps = 0.0;
        int32 MaxQueueDepth = 0;
        int32 SampleCount = 0;
    };

    FSummary Summarize()
    {
        FScopeLock Lock(&Mx);
        FSummary S;
        S.SampleCount = Window.Num();
        if (Window.Num() == 0) return S;

        TArray<double> Gpu; Gpu.Reserve(Window.Num());
        TArray<double> E2E; E2E.Reserve(Window.Num());
        TArray<double> Push; Push.Reserve(Window.Num());
        double SumG = 0.0, SumE = 0.0, SumP = 0.0;
        int32 MaxQ = 0;
        for (const FFrameTimings& T : Window)
        {
            Gpu.Add(T.GpuReadbackMs);
            E2E.Add(T.EndToEndLatencyMs);
            Push.Add(T.PushMs);
            SumG += T.GpuReadbackMs;
            SumE += T.EndToEndLatencyMs;
            SumP += T.PushMs;
            if (T.QueueDepthAtSubmit > MaxQ) MaxQ = T.QueueDepthAtSubmit;
        }
        S.GpuReadbackMeanMs = SumG / Window.Num();
        S.EndToEndMeanMs = SumE / Window.Num();
        S.PushMeanMs = SumP / Window.Num();
        S.MaxQueueDepth = MaxQ;

        Gpu.Sort();
        E2E.Sort();
        Push.Sort();
        const int32 IdxP95 = FMath::Clamp(FMath::RoundToInt(0.95f * (Gpu.Num() - 1)), 0, Gpu.Num() - 1);
        S.GpuReadbackP95Ms = Gpu[IdxP95];
        S.EndToEndP95Ms = E2E[IdxP95];
        S.PushP95Ms = Push[IdxP95];

        if (WallTickSeconds.Num() >= 2)
        {
            double SumDelta = 0.0;
            for (int32 i = 1; i < WallTickSeconds.Num(); ++i)
            {
                SumDelta += (WallTickSeconds[i] - WallTickSeconds[i - 1]);
            }
            const double Mean = SumDelta / (WallTickSeconds.Num() - 1);
            S.Fps = (Mean > 0.0) ? (1.0 / Mean) : 0.0;
        }
        return S;
    }

    std::atomic<uint64> FrameIdAtomic{0};
    std::atomic<uint64> FramesPushed{0};
    std::atomic<uint64> FramesDropped{0};

private:
    FCriticalSection Mx;
    TArray<FFrameTimings> Window;
    TArray<double> WallTickSeconds;
};
