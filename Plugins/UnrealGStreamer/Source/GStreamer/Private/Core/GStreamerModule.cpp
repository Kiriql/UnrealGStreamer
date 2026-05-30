#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"

#include "Core/GStreamerLog.h"
#include "GstCore.h"

DEFINE_LOG_CATEGORY(LogGStreamer);

static void GstLogBridge(int Level, const char* Category, const char* /*File*/, int /*Line*/, const char* /*Function*/, const char* Message)
{
	// GstDebugLevel: 1=ERROR, 2=WARNING, 3=FIXME, 4=INFO, 5=DEBUG, 6=LOG, 7=TRACE, 8=MEMDUMP
	const FString Cat(UTF8_TO_TCHAR(Category));
	const FString Msg(UTF8_TO_TCHAR(Message));
	switch (Level)
	{
	case 1: UE_LOG(LogGStreamer, Error,   TEXT("[%s] %s"), *Cat, *Msg); break;
	case 2: UE_LOG(LogGStreamer, Warning, TEXT("[%s] %s"), *Cat, *Msg); break;
	case 3: UE_LOG(LogGStreamer, Warning, TEXT("[FIXME][%s] %s"), *Cat, *Msg); break;
	case 4: UE_LOG(LogGStreamer, Log,     TEXT("[%s] %s"), *Cat, *Msg); break;
	default: UE_LOG(LogGStreamer, Verbose, TEXT("[%s] %s"), *Cat, *Msg); break;
	}
}

namespace
{
	const TCHAR* const GBundledDllNames[] =
	{
		TEXT("glib-2.0-0.dll"),
		TEXT("gmodule-2.0-0.dll"),
		TEXT("gobject-2.0-0.dll"),
		TEXT("gio-2.0-0.dll"),
		TEXT("intl-8.dll"),
		TEXT("ffi-7.dll"),
		TEXT("z-1.dll"),
		TEXT("orc-0.4-0.dll"),
		TEXT("pcre2-8-0.dll"),
		TEXT("gstreamer-1.0-0.dll"),
		TEXT("gstbase-1.0-0.dll"),
		TEXT("gstapp-1.0-0.dll"),
		TEXT("gstvideo-1.0-0.dll"),
		TEXT("gstaudio-1.0-0.dll"),
		TEXT("gstpbutils-1.0-0.dll"),
		TEXT("gsttag-1.0-0.dll"),
		TEXT("gstcodecparsers-1.0-0.dll"),
		TEXT("gstcodecs-1.0-0.dll"),
		TEXT("gstd3dshader-1.0-0.dll"),
		TEXT("gstd3d11-1.0-0.dll"),
		TEXT("gstdxva-1.0-0.dll"),
	};

	const char* const GRequiredGstPlugins[] =
	{
		"coreelements",
		"app",
		"videoconvertscale",
		"playback",
		"typefindfunctions",
	};

	constexpr uint32 GMinGstMajor = 1;
	constexpr uint32 GMinGstMinor = 24;
}

class FGStreamerModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealGStreamer"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogGStreamer, Error, TEXT("UnrealGStreamer: plugin descriptor not found via IPluginManager"));
			return;
		}

		const FString BinDir = FPaths::Combine(Plugin->GetBaseDir(),
			TEXT("Source"), TEXT("ThirdParty"), TEXT("GStreamer"), TEXT("Win64"), TEXT("bin"));
		const FString GstPluginsDir = FPaths::Combine(BinDir, TEXT("gstreamer-1.0"));

		FPlatformProcess::PushDllDirectory(*BinDir);
		PushedDllDir = BinDir;

		for (const TCHAR* DllName : GBundledDllNames)
		{
			const FString FullPath = FPaths::Combine(BinDir, FString(DllName));
			void* Handle = FPlatformProcess::GetDllHandle(*FullPath);
			if (!Handle)
			{
				UE_LOG(LogGStreamer, Error, TEXT("Failed to load %s from %s"), DllName, *BinDir);
				return;
			}
			LoadedDlls.Add(Handle);
		}

		FPlatformMisc::SetEnvironmentVar(TEXT("GST_PLUGIN_SYSTEM_PATH"), *GstPluginsDir);
		FPlatformMisc::SetEnvironmentVar(TEXT("GST_PLUGIN_PATH"), *GstPluginsDir);

		GstCore::SetLogCallback(&GstLogBridge);

		char ErrBuf[512] = {};
		if (!GstCore::Init(ErrBuf, sizeof(ErrBuf)))
		{
			UE_LOG(LogGStreamer, Error, TEXT("gst_init_check failed: %s"), UTF8_TO_TCHAR(ErrBuf));
			return;
		}

		const GstCore::FVersion V = GstCore::GetVersion();
		UE_LOG(LogGStreamer, Log, TEXT("GStreamer runtime version %u.%u.%u.%u"),
			V.Major, V.Minor, V.Micro, V.Nano);

		if (V.Major < GMinGstMajor || (V.Major == GMinGstMajor && V.Minor < GMinGstMinor))
		{
			UE_LOG(LogGStreamer, Warning,
				TEXT("GStreamer version %u.%u below required minimum %u.%u — proceeding, but features may not work"),
				V.Major, V.Minor, GMinGstMajor, GMinGstMinor);
		}

		int32 Missing = 0;
		char VerBuf[64];
		for (const char* Name : GRequiredGstPlugins)
		{
			VerBuf[0] = 0;
			if (GstCore::IsPluginAvailable(Name, VerBuf, sizeof(VerBuf)))
			{
				UE_LOG(LogGStreamer, Log, TEXT("Plugin '%s' v%s found"),
					UTF8_TO_TCHAR(Name), UTF8_TO_TCHAR(VerBuf));
			}
			else
			{
				UE_LOG(LogGStreamer, Warning, TEXT("Plugin '%s' NOT found in registry (searched: %s)"),
					UTF8_TO_TCHAR(Name), *GstPluginsDir);
				++Missing;
			}
		}

		UE_LOG(LogGStreamer, Log, TEXT("UnrealGStreamer startup OK (missing required plugins: %d)"), Missing);
	}

	virtual void ShutdownModule() override
	{
		GstCore::Deinit();

		for (void* Handle : LoadedDlls)
		{
			FPlatformProcess::FreeDllHandle(Handle);
		}
		LoadedDlls.Empty();

		if (!PushedDllDir.IsEmpty())
		{
			FPlatformProcess::PopDllDirectory(*PushedDllDir);
			PushedDllDir.Empty();
		}

		UE_LOG(LogGStreamer, Log, TEXT("UnrealGStreamer shutdown"));
	}

private:
	TArray<void*> LoadedDlls;
	FString PushedDllDir;
};

IMPLEMENT_MODULE(FGStreamerModule, GStreamer)
