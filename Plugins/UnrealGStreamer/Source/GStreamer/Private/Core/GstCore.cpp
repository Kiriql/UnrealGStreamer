#include "GstCore.h"

#include <gst/gst.h>

namespace
{
	bool g_inited = false;

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
