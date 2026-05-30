# UnrealGStreamer

Two-way GStreamer integration for Unreal Engine 5.7. Stream Unreal cameras into GStreamer pipelines and feed GStreamer media into Unreal textures.

> **Status: early scaffolding.** The plugin currently loads the bundled GStreamer runtime and verifies required elements at startup. Source and sink components are not yet ported. Expect breaking changes.

🇷🇺 [Русская версия](README.ru.md)

## Supported platforms

| Platform | Status |
| --- | --- |
| Windows 64 | Scaffold (current) |
| Linux | Planned |
| macOS | Planned |
| Android | Planned |

iOS is out of scope — App Store policy and static-linking constraints make GStreamer integration impractical.

## Requirements

- Unreal Engine **5.7**
- GStreamer **1.24+** runtime (1.28.3 LGPL set is bundled in `Plugins/UnrealGStreamer/Source/ThirdParty/GStreamer/Win64/`)

You don't need a system GStreamer install to build or run on Windows — everything required ships with the plugin.

## Repository layout

```
GStreamerProject.uproject       Host UE project used for development and demos
Plugins/UnrealGStreamer/        The plugin itself
└── Source/
    ├── GStreamer/              Runtime module
    └── ThirdParty/GStreamer/   Bundled headers, .lib, .dll, plugins
```

The host project (`GStreamerProject`) is a thin shell used to develop and test the plugin. You can drop `Plugins/UnrealGStreamer/` into any other UE 5.7 project once standalone packaging is ready.

## What works today

- `GStreamer` runtime module loads on UE startup, delay-loads the bundled GStreamer DLLs, sets the plugin search path, calls `gst_init`, and reports the runtime version and available plugins in the `LogGStreamer` log category.
- Send copy-path components: `UGstPipelineComponent` (builds a pipeline from a `gst-launch`-style string) and `UGstAppSrcComponent` (reads a `SceneCaptureComponent2D`'s render target back to CPU and pushes the BGRA buffer into an `appsrc`). This is the baseline path that will later be compared against zero-copy.

## Performance baseline (copy path)

The send copy-path measures the cost of going GPU → CPU → GStreamer. `UGstAppSrcComponent` instruments every frame and emits a heartbeat line into `LogGStreamer` every `MetricsLogIntervalFrames` ticks (default 60). What is measured (mean + p95 over a rolling 60-frame window):

- `gpu` — wall time of `RHICmdList.ReadSurfaceData` itself, measured on the render thread inside the enqueued command. This is the cost we want zero-copy to eliminate.
- `e2e` — end-to-end latency from buffer submit on the game thread to `gst_app_src_push_buffer`. Currently dominated by the fence-polling interval (the component checks `FRenderCommandFence::IsFenceComplete` once per its own tick).
- `push` — wall time spent inside `gst_app_src_push_buffer`.
- `fps` — derived from intervals between component ticks.
- `queue=cur/max` — peak depth of the in-flight readback queue within the window, against `MaxQueueLength`.
- `pushed` / `dropped` — cumulative counters. `dropped` increments when the in-flight queue is full at submit time.

Example log line:

```
LogGStreamer: copy-path: fps=25.0 gpu=14.5ms p95=15.7 e2e=80.0ms p95=84.5 push=0.01ms p95=0.02 queue=2/5 pushed=600 dropped=0
```

Reference numbers (1920×1080 BGRA, NVIDIA RTX 4070 SUPER, Windows 11, UE 5.7, default tick interval 1/25): `gpu ≈ 14–15ms`, `push ≈ 0.01ms`. Concrete comparisons will be added once the zero-copy path is in place.

A plain-C log bridge forwards GStreamer's internal debug output into `LogGStreamer` so pipeline errors, warnings, and registration failures are visible in the standard UE log.

## License

To be decided before the first public release.
