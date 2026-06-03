// gst-side TU: only <gst/*>, no UE headers (CLAUDE.md split TU rule).
#include <gst/gst.h>
#include <cstring>

// RateControlMode: 0 = Default (skip), 1 = CBR, 2 = VBR, 3 = CQP.
// Property name for rate-control differs across encoders but the d3d12 family uses "rc-mode"
// with string-typed enum values "cbr"/"vbr"/"cqp". We try those first; if the property doesn't
// exist g_object_set emits a warning but does not crash.
extern "C" int GstEncoderApplySettings(
    void* PipelineRaw,
    const char* ElementName,
    int BitrateKbps,
    int GopSize,
    int RateControlMode)
{
    if (!PipelineRaw || !ElementName || !*ElementName) return -1;

    GstElement* Pipeline = static_cast<GstElement*>(PipelineRaw);
    GstElement* Enc = gst_bin_get_by_name(GST_BIN(Pipeline), ElementName);
    if (!Enc) return -1;

    GObjectClass* Klass = G_OBJECT_GET_CLASS(Enc);

    if (BitrateKbps > 0 && g_object_class_find_property(Klass, "bitrate"))
    {
        g_object_set(Enc, "bitrate", (guint)BitrateKbps, NULL);
    }

    if (GopSize > 0)
    {
        if (g_object_class_find_property(Klass, "gop-size"))
            g_object_set(Enc, "gop-size", (gint)GopSize, NULL);
        else if (g_object_class_find_property(Klass, "key-int-max"))
            g_object_set(Enc, "key-int-max", (guint)GopSize, NULL);
    }

    if (RateControlMode > 0)
    {
        const char* RcStr = nullptr;
        switch (RateControlMode)
        {
        case 1: RcStr = "cbr"; break;
        case 2: RcStr = "vbr"; break;
        case 3: RcStr = "cqp"; break;
        default: break;
        }
        if (RcStr && g_object_class_find_property(Klass, "rc-mode"))
        {
            gst_util_set_object_arg(G_OBJECT(Enc), "rc-mode", RcStr);
        }
    }

    gst_object_unref(Enc);
    return 0;
}
