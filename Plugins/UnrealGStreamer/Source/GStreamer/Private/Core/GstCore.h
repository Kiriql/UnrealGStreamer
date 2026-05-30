#pragma once

#include <cstdint>
#include <cstddef>

namespace GstCore
{
	struct FVersion
	{
		uint32_t Major = 0;
		uint32_t Minor = 0;
		uint32_t Micro = 0;
		uint32_t Nano  = 0;
	};

	bool Init(char* OutErrBuf, size_t BufSize);
	void Deinit();
	bool IsInitialized();

	FVersion GetVersion();
	bool IsPluginAvailable(const char* Name, char* OutVersionBuf, size_t VersionBufSize);

	// Plain-C log bridge: gst-side calls UE-registered callback. Level is GstDebugLevel (1=ERROR..7=MEMDUMP).
	typedef void (*FLogCallback)(int Level, const char* Category, const char* File, int Line, const char* Function, const char* Message);
	void SetLogCallback(FLogCallback Cb);
}
