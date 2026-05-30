#pragma once

#include "CoreMinimal.h"

#include <atomic>

struct FFrameTimings
{
    double ReadbackMs = 0.0;
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
        double ReadbackMeanMs = 0.0;
        double ReadbackP95Ms = 0.0;
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

        TArray<double> Readback; Readback.Reserve(Window.Num());
        TArray<double> Push; Push.Reserve(Window.Num());
        double SumR = 0.0, SumP = 0.0;
        int32 MaxQ = 0;
        for (const FFrameTimings& T : Window)
        {
            Readback.Add(T.ReadbackMs);
            Push.Add(T.PushMs);
            SumR += T.ReadbackMs;
            SumP += T.PushMs;
            if (T.QueueDepthAtSubmit > MaxQ) MaxQ = T.QueueDepthAtSubmit;
        }
        S.ReadbackMeanMs = SumR / Window.Num();
        S.PushMeanMs = SumP / Window.Num();
        S.MaxQueueDepth = MaxQ;

        Readback.Sort();
        Push.Sort();
        const int32 IdxP95 = FMath::Clamp(FMath::RoundToInt(0.95f * (Readback.Num() - 1)), 0, Readback.Num() - 1);
        S.ReadbackP95Ms = Readback[IdxP95];
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
