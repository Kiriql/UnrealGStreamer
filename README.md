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

The send copy-path measures the cost of going GPU → CPU → GStreamer. `UGstAppSrcComponent` instruments every frame and emits a heartbeat line into `LogGStreamer` every `MetricsLogIntervalFrames` ticks (default 60). What is measured:

- `readback` — wall time from `RHICmdList.ReadSurfaceData` enqueue to `FRenderCommandFence` completion (ms, mean + p95 over a rolling 60-frame window).
- `push` — wall time spent inside `gst_app_src_push_buffer` (ms, mean + p95).
- `fps` — derived from intervals between component ticks.
- `queue=cur/max` — peak depth of the in-flight readback queue within the window, against `MaxQueueLength`.
- `pushed` / `dropped` — cumulative counters. `dropped` increments when the in-flight queue is full at submit time.

Example log line:

```
LogGStreamer: Verbose: copy-path: fps=24.9 readback=8.31ms p95=12.10 push=0.41ms p95=0.62 queue=2/5 pushed=600 dropped=0
```

Concrete numbers on a reference scene will be added once the zero-copy path is in place for direct comparison.

## License

To be decided before the first public release.
