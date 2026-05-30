#include "Pipeline/IGstPipeline.h"

#include <gst/gst.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace
{
    void SafeCopy(char* Dst, size_t DstSize, const char* Src)
    {
        if (!Dst || DstSize == 0) return;
        if (!Src) { Dst[0] = '\0'; return; }
        size_t i = 0;
        for (; i + 1 < DstSize && Src[i]; ++i) Dst[i] = Src[i];
        Dst[i] = '\0';
    }
}

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

    void WorkerLoop();
    gboolean OnBusMessage(GstMessage* Message);

private:
    std::string m_Name;
    GstElement* m_Pipeline = nullptr;
    GstBus* m_Bus = nullptr;
    GMainLoop* m_Loop = nullptr;
    std::unique_ptr<std::thread> m_Worker;
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
static gboolean BusMessageFunc(GstBus*, GstMessage* Message, FGstPipelineImpl* Context) { return Context->OnBusMessage(Message); }
static void ThreadWorkerFunc(FGstPipelineImpl* Context) { Context->WorkerLoop(); }

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

    gst_bus_add_watch(m_Bus, (GstBusFunc)BusMessageFunc, this);
    return true;
}

void FGstPipelineImpl::Shutdown()
{
    Stop();

    if (m_Pipeline)
    {
        gst_element_set_state(m_Pipeline, GST_STATE_NULL);
    }

    GST_RELEASE(gst_object_unref, m_Bus);
    GST_RELEASE(gst_object_unref, m_Pipeline);
}

bool FGstPipelineImpl::Start()
{
    if (m_Loop)
    {
        return false;
    }

    m_Loop = g_main_loop_new(nullptr, FALSE);
    if (!m_Loop)
    {
        return false;
    }

    m_Worker.reset(new std::thread(ThreadWorkerFunc, this));
    return true;
}

void FGstPipelineImpl::Stop()
{
    if (m_Loop)
    {
        g_main_loop_quit(m_Loop);

        if (m_Worker && m_Worker->joinable())
        {
            m_Worker->join();
        }
        m_Worker.reset(nullptr);

        GST_RELEASE(g_main_loop_unref, m_Loop);
    }
}

void FGstPipelineImpl::WorkerLoop()
{
    gst_element_set_state(m_Pipeline, GST_STATE_PLAYING);
    g_main_loop_run(m_Loop);
    gst_element_set_state(m_Pipeline, GST_STATE_NULL);
}

gboolean FGstPipelineImpl::OnBusMessage(GstMessage* Message)
{
    const int Type = GST_MESSAGE_TYPE(Message);
    switch (Type)
    {
    case GST_MESSAGE_TAG:
    case GST_MESSAGE_BUFFERING:
        break;

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
        g_main_loop_quit(m_Loop);
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

    case GST_MESSAGE_STATE_CHANGED:
        break;

    default:
        break;
    }

    return TRUE;
}
