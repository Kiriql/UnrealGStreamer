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

## License

To be decided before the first public release.
