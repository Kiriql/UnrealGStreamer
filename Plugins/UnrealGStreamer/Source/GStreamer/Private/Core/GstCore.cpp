#include "GstCore.h"

#include <gst/gst.h>

namespace
{
	bool g_inited = false;
	GstCore::FLogCallback g_log_cb = nullptr;

	void GstLogFunc(GstDebugCategory* Category, GstDebugLevel Level,
		const gchar* File, const gchar* Function, gint Line,
		GObject* /*Object*/, GstDebugMessage* Message, gpointer /*UserData*/)
	{
		if (!g_log_cb) return;
		const char* CatName = Category ? gst_debug_category_get_name(Category) : "";
		const char* Msg = gst_debug_message_get(Message);
		g_log_cb((int)Level, CatName ? CatName : "", File ? File : "", (int)Line,
			Function ? Function : "", Msg ? Msg : "");
	}

	void SafeCopy(char* Dst, size_t Sz, const char* Src)
	{
		if (!Dst || Sz == 0) return;
		const char* Use = Src ? Src : "";
		size_t i = 0;
		for (; i + 1 < Sz && Use[i] != 0; ++i) Dst[i] = Use[i];
		Dst[i] = 0;
	}
}

namespace GstCore
{
	bool Init(char* OutErrBuf, size_t BufSize)
	{
		if (g_inited) return true;

		GError* Err = nullptr;
		gboolean Ok = gst_init_check(nullptr, nullptr, &Err);
		if (!Ok)
		{
			SafeCopy(OutErrBuf, BufSize, Err && Err->message ? Err->message : "<unknown>");
			if (Err) g_error_free(Err);
			return false;
		}
		g_inited = true;
		return true;
	}

	void Deinit()
	{
		if (g_inited)
		{
			if (g_log_cb)
			{
				gst_debug_remove_log_function(GstLogFunc);
				g_log_cb = nullptr;
			}
			gst_deinit();
			g_inited = false;
		}
	}

	bool IsInitialized()
	{
		return g_inited;
	}

	FVersion GetVersion()
	{
		guint Maj = 0, Min = 0, Mic = 0, Nan = 0;
		gst_version(&Maj, &Min, &Mic, &Nan);
		FVersion V;
		V.Major = static_cast<uint32_t>(Maj);
		V.Minor = static_cast<uint32_t>(Min);
		V.Micro = static_cast<uint32_t>(Mic);
		V.Nano  = static_cast<uint32_t>(Nan);
		return V;
	}

	void SetLogCallback(FLogCallback Cb)
	{
		if (g_log_cb && !Cb)
		{
			gst_debug_remove_log_function(GstLogFunc);
		}
		else if (!g_log_cb && Cb)
		{
			gst_debug_remove_log_function(gst_debug_log_default);
			gst_debug_add_log_function(GstLogFunc, nullptr, nullptr);
		}
		g_log_cb = Cb;
	}

	bool IsPluginAvailable(const char* Name, char* OutVersionBuf, size_t VersionBufSize)
	{
		if (!Name) return false;
		GstRegistry* Reg = gst_registry_get();
		GstPlugin* P = gst_registry_find_plugin(Reg, Name);
		if (!P) return false;

		SafeCopy(OutVersionBuf, VersionBufSize, gst_plugin_get_version(P));
		gst_object_unref(P);
		return true;
	}
}
