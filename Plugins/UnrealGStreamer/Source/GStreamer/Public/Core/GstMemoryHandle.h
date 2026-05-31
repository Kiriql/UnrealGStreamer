#pragma once

// Opaque forward-declaration. In gst-side .cpp TUs this is reinterpret_cast'd
// to GstMemory*; in UE-side .cpp TUs it stays opaque. Keeps GStreamer types
// out of headers per the split-TU rule (see CLAUDE.md, research-log 2026-05-30).
struct FGstMemoryHandle;
