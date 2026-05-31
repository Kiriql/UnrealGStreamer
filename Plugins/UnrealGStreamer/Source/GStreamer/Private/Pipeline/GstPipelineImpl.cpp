#include "Pipeline/IGstPipeline.h"
#include "Core/GstUtils.h"

#include <gst/gst.h>

#include <cstdio>
#include <cstring>
#include <string>

using GstUtils::SafeCopy;

class FGstPipelineImpl : public IGstPipeline
{
public:
    FGstPipelineImpl() {}
    ~FGstPipelineImpl() { Shutdown(); }

    virtual void Destroy() override;

    virtual bool Init(const char* Name, const char* Config, char* OutErrBuf, size_t ErrBufSize) override;
    virtual void Shutdown() override;
    virtual bool Start() override;
    virtual void Stop() override;

    virtual const char* GetName() const override { return m_Name.c_str(); }
    virtual void* GetGPipeline() override { return m_Pipeline; }
    virtual void* GetGBus() override { return m_Bus; }

    GstBusSyncReply OnBusMessage(GstMessage* Message);

private:
    std::string m_Name;
    GstElement* m_Pipeline = nullptr;
    GstBus* m_Bus = nullptr;
};

IGstPipeline* IGstPipeline::CreateInstance()
{
    return new FGstPipelineImpl();
}

void FGstPipelineImpl::Destroy()
{
    delete this;
}

#define GST_RELEASE(func, ptr) do { if (ptr) { func(ptr); ptr = nullptr; } } while (0)

static GstBusSyncReply BusSyncHandlerFunc(GstBus*, GstMessage* Message, gpointer UserData)
{
    return static_cast<FGstPipelineImpl*>(UserData)->OnBusMessage(Message);
}

bool FGstPipelineImpl::Init(const char* Name, const char* Config, char* OutErrBuf, size_t ErrBufSize)
{
    if (OutErrBuf && ErrBufSize) OutErrBuf[0] = '\0';

    if (m_Pipeline)
    {
        SafeCopy(OutErrBuf, ErrBufSize, "pipeline already initialized");
        return false;
    }

    m_Name = Name ? Name : "";

    GError* Error = nullptr;
    m_Pipeline = gst_parse_launch(Config, &Error);
    if (Error)
    {
        SafeCopy(OutErrBuf, ErrBufSize, Error->message);
        g_error_free(Error);
        Shutdown();
        return false;
    }
    if (!m_Pipeline)
    {
        SafeCopy(OutErrBuf, ErrBufSize, "gst_parse_launch returned null");
        return false;
    }

    m_Bus = gst_pipeline_get_bus(GST_PIPELINE(m_Pipeline));
    if (!m_Bus)
    {
        SafeCopy(OutErrBuf, ErrBufSize, "gst_pipeline_get_bus failed");
        Shutdown();
        return false;
    }

    // Sync handler fires on the thread that posts the message — no GMainLoop required.
    // We just log and never queue; returning GST_BUS_DROP consumes the message.
    gst_bus_set_sync_handler(m_Bus, BusSyncHandlerFunc, this, nullptr);
    return true;
}

void FGstPipelineImpl::Shutdown()
{
    if (m_Bus)
    {
        // Atomically detach the handler before tearing down. gst guarantees no NEW callbacks
        // after this; set_state(NULL) below drains streaming threads so any in-flight ones drop.
        gst_bus_set_sync_handler(m_Bus, nullptr, nullptr, nullptr);
    }

    Stop();

    GST_RELEASE(gst_object_unref, m_Bus);
    GST_RELEASE(gst_object_unref, m_Pipeline);
}

bool FGstPipelineImpl::Start()
{
    if (!m_Pipeline) return false;

    const GstStateChangeReturn Ret = gst_element_set_state(m_Pipeline, GST_STATE_PLAYING);
    if (Ret == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr("[GstPipeline][%s] set_state(PLAYING) FAILURE\n", m_Name.c_str());
        return false;
    }
    return true;
}

void FGstPipelineImpl::Stop()
{
    if (m_Pipeline)
    {
        gst_element_set_state(m_Pipeline, GST_STATE_NULL);
    }
}

GstBusSyncReply FGstPipelineImpl::OnBusMessage(GstMessage* Message)
{
    const int Type = GST_MESSAGE_TYPE(Message);
    switch (Type)
    {
    case GST_MESSAGE_EOS:
        g_print("[GstPipeline][%s] EOS\n", m_Name.c_str());
        break;

    case GST_MESSAGE_ERROR:
    {
        GError* Err = nullptr;
        gchar* Dbg = nullptr;
        gst_message_parse_error(Message, &Err, &Dbg);
        g_printerr("[GstPipeline][%s] ERROR from %s: %s (debug: %s)\n",
            m_Name.c_str(),
            GST_OBJECT_NAME(GST_MESSAGE_SRC(Message)),
            Err ? Err->message : "<null>",
            Dbg ? Dbg : "<null>");
        if (Err) g_error_free(Err);
        if (Dbg) g_free(Dbg);
        break;
    }

    case GST_MESSAGE_WARNING:
    {
        GError* Err = nullptr;
        gchar* Dbg = nullptr;
        gst_message_parse_warning(Message, &Err, &Dbg);
        g_printerr("[GstPipeline][%s] WARNING from %s: %s (debug: %s)\n",
            m_Name.c_str(),
            GST_OBJECT_NAME(GST_MESSAGE_SRC(Message)),
            Err ? Err->message : "<null>",
            Dbg ? Dbg : "<null>");
        if (Err) g_error_free(Err);
        if (Dbg) g_free(Dbg);
        break;
    }

    default:
        break;
    }

    return GST_BUS_DROP;
}
