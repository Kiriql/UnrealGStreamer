# UnrealGStreamer

Two-way GStreamer integration for Unreal Engine 5.7. Stream Unreal cameras into GStreamer pipelines and feed GStreamer media into Unreal textures.

> **Status: early scaffolding.** Send copy-path is working on Windows. Send zero-copy (hybrid GPU-only) is working on Windows with D3D12. Receive components and additional platforms are not yet ported. Expect breaking changes.

🇷🇺 [Русская версия](README.ru.md)

## Supported platforms

| Platform | Status |
| --- | --- |
| Windows 64 | Send: copy-path + zero-copy (D3D12) |
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
- Send copy-path components: `UGstPipelineComponent` (builds a pipeline from a `gst-launch`-style string) and `UGstAppSrcComponent` (reads a `SceneCaptureComponent2D`'s render target back to CPU and pushes the BGRA buffer into an `appsrc`).
- Send zero-copy component (Windows, D3D12): `UGstZeroCopyVideoSourceComponent`. Wraps GPU memory as `GstMemory` via the GStreamer `d3d12` plugin and pushes into `appsrc` with no CPU readback. See architecture below.

## Send copy-path (baseline)

`UGstAppSrcComponent` measures the cost of going GPU → CPU → GStreamer and emits a heartbeat line into `LogGStreamer` every `MetricsLogIntervalFrames` ticks (default 60). What is measured (mean + p95 over a rolling 60-frame window):

- `gpu` — wall time of `RHICmdList.ReadSurfaceData` itself, measured on the render thread inside the enqueued command.
- `e2e` — end-to-end latency from buffer submit on the game thread to `gst_app_src_push_buffer`. Currently dominated by the fence-polling interval (the component checks `FRenderCommandFence::IsFenceComplete` once per its own tick).
- `push` — wall time spent inside `gst_app_src_push_buffer`.
- `fps` — derived from intervals between component ticks.
- `queue=cur/max` — peak depth of the in-flight readback queue within the window, against `MaxQueueLength`.
- `pushed` / `dropped` — cumulative counters.

Example log line:

```
LogGStreamer: copy-path: fps=25.0 gpu=14.5ms p95=15.7 e2e=80.0ms p95=84.5 push=0.01ms p95=0.02 queue=2/5 pushed=600 dropped=0
```

## Send zero-copy (Windows, D3D12)

`UGstZeroCopyVideoSourceComponent` keeps the frame on the GPU end-to-end: the only memory that crosses the UE → GStreamer boundary is a wrapped pointer to an existing `ID3D12Resource`, synchronized with the GStreamer queue through an `ID3D12Fence`. No `ReadSurfaceData`. No `memcpy`.

### Architecture

- `IZeroCopyBackend` (`Private/ZeroCopy/IZeroCopyBackend.h`) — platform-agnostic contract: `AllocSharedTexture`, `WrapExternalTextureAsGstMemoryWithFence`, `FreeSharedTexture`.
- `FD3D12ZeroCopyBackend` (`Private/ZeroCopy/Windows/D3D12Backend.cpp`) — Windows implementation. Holds a single `ID3D12Fence` reused across frames, monotonically incremented per push.
- `D3D12GstBridge.cpp` — separate translation unit (no UE headers, only `<gst/gst.h>` + `<gst/d3d12/gstd3d12.h>` + `<d3d12.h>`) that wraps an `ID3D12Resource` as `GstMemory` via `gst_d3d12_allocator_alloc_wrapped` and attaches the fence with `gst_d3d12_memory_set_fence`. UE/GStreamer headers stay in different TUs to avoid the `GError` symbol collision between glib and UE Core.
- `UGstZeroCopyVideoSourceComponent` runs on Tick on the game thread, allocates a single shared `B8G8R8A8_UNORM` resource on pipeline start, then enqueues a render command per frame: transition source RT → CopySrc, transition shared → CopyDest, `RHICopyTexture(UE RT → shared)`, signal the fence on UE's graphics queue via `RHIRunOnQueue` + `ID3D12CommandQueue::Signal`, wrap the shared resource as `GstMemory` with that fence value, push.

The component has two modes:

- **`SourceRT` mode** — production mode. Bind a `UTextureRenderTarget2D` that some `USceneCaptureComponent2D` is rendering into, and the component will copy that RT into the shared resource each frame.
- **Synthetic mode** — debugging mode used when `SourceRenderTarget` is null. The component draws a moving HSV color into the shared resource itself. Useful for verifying the GStreamer side of the pipeline without a scene capture.

### Why one GPU copy, not literally zero

The honest answer is that wrapping UE's own render target directly is not possible on UE 5.7 D3D12:

- UE allocates all render targets as `B8G8R8A8_TYPELESS` for sRGB/linear view flexibility.
- The pre-built SRV heap the GStreamer `d3d12` plugin builds inside `alloc_wrapped` is created with the resource's intrinsic format. D3D12 view validation rejects TYPELESS — invalid call, device removed.
- The alternative — subclassing `FTextureRenderTarget2DResource` to inject a typed resource — is blocked by missing `ENGINE_API` exports on its constructor and several virtuals, so the linker can't satisfy the vtable from outside the Engine module.

Both Epic's Pixel Streaming and the mature [Spout-DX12 plugin](https://github.com/GPUbrainStorm/UE5_Spout2_DX12) hit the same wall and made the same call: copy UE's RT into an owned, typed resource and hand that off. From the TensorWorks post-mortem on Pixel Streaming:

> the nanosecond-scale cost saving observed in our testing doesn't warrant the added complexity of handling the case where NVENC fails to free up an active framebuffer in time for it to be drawn to

Same conclusion here. The numbers below show why.

### Baseline numbers

Hardware: NVIDIA RTX 4070 SUPER, Windows 11, UE 5.7, GStreamer 1.28.3. Pipeline: `appsrc name=ueapp ! d3d12videosink sync=false`. Source: `SceneCaptureComponent2D` rendering 1920×1080 at 25 fps into a `UTextureRenderTarget2D`. Steady-state over ~30 seconds:

| Metric | Copy-path | Zero-copy (hybrid) |
| --- | --- | --- |
| `gpu` (readback) | ≈14 ms mean, p95 ≈16 ms | **0** (no readback) |
| `push` (`gst_app_src_push_buffer`) | ≈0.4 ms mean | **≈0.008 ms** mean, p95 ≈0.01 ms |
| `fps` | 25.0 | 25.0 |
| `e2e` (submit → push) | ≈80 ms (fence polling) | n/a in this iteration |

Example log line from the zero-copy heartbeat:

```
LogGStreamer: hybrid: fps=25.0 push=0.008ms p95=0.009 pushed=1199
```

The GPU readback is the entire reason zero-copy exists. Eliminating it is the headline number.

## Pipeline presets and encoder

`UGstPipelineComponent` exposes a `Preset` dropdown so you don't have to hand-write a `gst-launch` string for common cases. Pick `Custom` to write your own in `PipelineConfig`. Presets that write to a file use `FileOutputPath`.

| Preset | Pipeline (resolved) |
| --- | --- |
| `Display: BGRA -> d3d12videosink` | `appsrc … ! videoconvert ! d3d12videosink sync=false` |
| `Encode: H.264 -> fakesink` | `appsrc … ! d3d12upload ! d3d12h264enc name=enc ! h264parse ! fakesink sync=false` |
| `Encode: H.264 -> MP4 file` | `… ! d3d12h264enc name=enc ! h264parse ! mp4mux ! filesink location=<FileOutputPath>` |
| `Encode: H.264 -> UDP/RTP (127.0.0.1:5000)` | `… ! d3d12h264enc name=enc ! h264parse config-interval=1 ! rtph264pay pt=96 ! udpsink host=127.0.0.1 port=5000` |

> **Why H.264 only?** The upstream `d3d12` plugin ships H.264 encoder only — there is no `d3d12h265enc` / `d3d12av1enc` element in GStreamer yet (decoders exist, encoders don't). Hardware H.265/AV1 on Windows is reachable via `nvh265enc` / `mfh265enc` / `nvav1enc`, but those take sysmem (or D3D11) input, which forces a GPU↔CPU readback and breaks the zero-copy path that's the point of this plugin. H.265/AV1 presets will land as a one-line addition once `d3d12h265enc` / `d3d12av1enc` reach upstream.

Encoder presets name the encoder element `enc`. `UGstPipelineComponent` exposes the common encoder properties directly so you don't need a separate component:

- `BitrateKbps` — target bitrate (encoders that don't support the `bitrate` property are skipped silently).
- `KeyframeIntervalFrames` — `gop-size` (or `key-int-max` on encoders that use that name).
- `RateControl` — `CBR` / `VBR` / `CQP` mapped onto the encoder's `rc-mode` property where available.

For a `Custom` pipeline, name your encoder element `enc` (or change `EncoderElementName` on the component) and the same settings apply. Set `EncoderElementName` to empty to skip the configuration step entirely.

### Sanity-check the encoder

The `H264_UdpRtp` preset is the easiest way to confirm the encoder is actually emitting a valid stream. From any machine on the loopback:

```
ffplay -fflags nobuffer -flags low_delay -protocol_whitelist file,udp,rtp -i sdp.txt
```

…where `sdp.txt` describes the RTP stream (`m=video 5000 RTP/AVP 96`, `a=rtpmap:96 H264/90000`). `H264_FileMp4` writes a `ue_stream.mp4` you can open in any player.

### Diagnostics

Log bridge forwards GStreamer's internal debug output into `LogGStreamer` so pipeline errors, warnings, and registration failures show up in the standard UE log. For D3D12-level diagnostics during development, run the editor with `-d3ddebug -dred` — the GStreamer plugin does not enable those by default.

## License

To be decided before the first public release.
