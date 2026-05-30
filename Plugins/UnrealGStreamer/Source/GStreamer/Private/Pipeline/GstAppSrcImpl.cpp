#include "Pipeline/IGstAppSrc.h"
#include "Pipeline/IGstPipeline.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <string>

class FGstAppSrcImpl : public IGstAppSrc
{
public:
    FGstAppSrcImpl() {}
    ~FGstAppSrcImpl() { Disconnect(); }

    virtual void Destroy() override;

    virtual bool Connect(IGstPipeline* Pipeline, const char* ElementName) override;
    virtual void Disconnect() override;
    virtual bool SetCaps(const char* CapsString) override;
    virtual bool PushBuffer(IGstAppSrcBuffer* Buffer) override;
    virtual bool PushSharedBuffer(void* GstMemoryRaw) override;

private:
    std::string m_Name;
    GstElement* m_AppSrc = nullptr;
};

IGstAppSrc* IGstAppSrc::CreateInstance()
{
    return new FGstAppSrcImpl();
}

void FGstAppSrcImpl::Destroy()
{
    delete this;
}

bool FGstAppSrcImpl::Connect(IGstPipeline* Pipeline, const char* ElementName)
{
    if (m_AppSrc)
    {
        return false;
    }
    if (!Pipeline || !ElementName)
    {
        return false;
    }

    GstElement* PipelineEl = static_cast<GstElement*>(Pipeline->GetGPipeline());
    if (!PipelineEl)
    {
        return false;
    }

    m_Name = ElementName;
    m_AppSrc = gst_bin_get_by_name(GST_BIN(PipelineEl), ElementName);
    if (!m_AppSrc)
    {
        return false;
    }

    g_object_set(m_AppSrc, "emit-signals", TRUE, nullptr);
    return true;
}

void FGstAppSrcImpl::Disconnect()
{
    if (m_AppSrc)
    {
        gst_app_src_end_of_stream(GST_APP_SRC(m_AppSrc));
        g_object_set(m_AppSrc, "emit-signals", FALSE, nullptr);
        gst_object_unref(m_AppSrc);
        m_AppSrc = nullptr;
    }
}

bool FGstAppSrcImpl::SetCaps(const char* CapsString)
{
    if (!m_AppSrc || !CapsString) return false;
    GstCaps* Caps = gst_caps_from_string(CapsString);
    if (!Caps)
    {
        g_printerr("[GstAppSrc][%s] gst_caps_from_string failed for: %s\n", m_Name.c_str(), CapsString);
        return false;
    }
    gst_app_src_set_caps(GST_APP_SRC(m_AppSrc), Caps);
    gst_caps_unref(Caps);
    return true;
}

static void DestroyNotifyHandler(gpointer Data)
{
    static_cast<IGstAppSrcBuffer*>(Data)->Release();
}

bool FGstAppSrcImpl::PushBuffer(IGstAppSrcBuffer* Buffer)
{
    if (!m_AppSrc || !Buffer)
    {
        return false;
    }

    void* Data = Buffer->GetDataPtr();
    size_t DataSize = Buffer->GetDataSize();
    if (!Data || DataSize == 0)
    {
        Buffer->Release();
        return false;
    }

    GstBuffer* BufferObj = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        Data, DataSize, 0, DataSize,
        Buffer, DestroyNotifyHandler);

    const GstFlowReturn Result = gst_app_src_push_buffer(GST_APP_SRC(m_AppSrc), BufferObj);
    if (Result != GST_FLOW_OK)
    {
        g_printerr("[GstAppSrc][%s] push_buffer failed: %d (%s)\n",
            m_Name.c_str(), (int)Result, gst_flow_get_name(Result));
        return false;
    }
    return true;
}

bool FGstAppSrcImpl::PushSharedBuffer(void* GstMemoryRaw)
{
    if (!m_AppSrc || !GstMemoryRaw)
    {
        if (GstMemoryRaw) gst_memory_unref(static_cast<GstMemory*>(GstMemoryRaw));
        return false;
    }

    GstMemory* Mem = static_cast<GstMemory*>(GstMemoryRaw);
    GstBuffer* BufferObj = gst_buffer_new();
    gst_buffer_append_memory(BufferObj, Mem);

    const GstFlowReturn Result = gst_app_src_push_buffer(GST_APP_SRC(m_AppSrc), BufferObj);
    if (Result != GST_FLOW_OK)
    {
        g_printerr("[GstAppSrc][%s] push_shared_buffer failed: %d (%s)\n",
            m_Name.c_str(), (int)Result, gst_flow_get_name(Result));
        return false;
    }
    return true;
}
