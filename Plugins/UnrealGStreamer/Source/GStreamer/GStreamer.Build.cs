using UnrealBuildTool;
using System.IO;
using System.Collections.Generic;

public class GStreamer : ModuleRules
{
	public GStreamer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Projects",
			"RHI",
			"RenderCore",
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("D3D12RHI");
			AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
			PublicSystemLibraries.AddRange(new string[] { "d3d12.lib", "dxgi.lib" });
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			SetupGStreamerWin64();
		}
	}

	private void SetupGStreamerWin64()
	{
		string ThirdPartyRoot = Path.Combine(ModuleDirectory, "..", "ThirdParty", "GStreamer", "Win64");
		string IncRoot = Path.Combine(ThirdPartyRoot, "include");
		string LibRoot = Path.Combine(ThirdPartyRoot, "lib");
		string BinRoot = Path.Combine(ThirdPartyRoot, "bin");

		PublicSystemIncludePaths.AddRange(new string[]
		{
			Path.Combine(IncRoot, "gstreamer-1.0"),
			Path.Combine(IncRoot, "glib-2.0"),
			Path.Combine(IncRoot, "glib-2.0-config"),
		});

		string[] ImportLibs =
		{
			"gstreamer-1.0.lib",
			"gstbase-1.0.lib",
			"gstapp-1.0.lib",
			"gstvideo-1.0.lib",
			"gstd3d12-1.0.lib",
			"gobject-2.0.lib",
			"glib-2.0.lib",
		};
		foreach (string Lib in ImportLibs)
		{
			PublicAdditionalLibraries.Add(Path.Combine(LibRoot, Lib));
		}

		string[] RuntimeDlls =
		{
			"gstreamer-1.0-0.dll",
			"gstbase-1.0-0.dll",
			"gstapp-1.0-0.dll",
			"gstvideo-1.0-0.dll",
			"gstaudio-1.0-0.dll",
			"gstpbutils-1.0-0.dll",
			"gsttag-1.0-0.dll",
			"gstcodecparsers-1.0-0.dll",
			"gstcodecs-1.0-0.dll",
			"gstd3dshader-1.0-0.dll",
			"gstd3d11-1.0-0.dll",
			"gstd3d12-1.0-0.dll",
			"gstdxva-1.0-0.dll",
			"gobject-2.0-0.dll",
			"glib-2.0-0.dll",
			"gmodule-2.0-0.dll",
			"gio-2.0-0.dll",
			"intl-8.dll",
			"ffi-7.dll",
			"z-1.dll",
			"orc-0.4-0.dll",
			"pcre2-8-0.dll",
		};
		foreach (string Dll in RuntimeDlls)
		{
			string Src = Path.Combine(BinRoot, Dll);
			RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", Dll), Src, StagedFileType.NonUFS);
			PublicDelayLoadDLLs.Add(Dll);
		}

		List<string> GstPluginNames = new List<string>
		{
			"gstcoreelements.dll",
			"gstapp.dll",
			"gstvideoconvertscale.dll",
			"gstplayback.dll",
			"gsttypefindfunctions.dll",
			"gstvideotestsrc.dll",
			"gstaudiotestsrc.dll",
			"gstaudioconvert.dll",
			"gstaudioresample.dll",
			"gstautodetect.dll",
			"gstd3d11.dll",
			"gstd3d12.dll",
		};
		string PluginsBin = Path.Combine(BinRoot, "gstreamer-1.0");
		foreach (string GstPlugin in GstPluginNames)
		{
			string Src = Path.Combine(PluginsBin, GstPlugin);
			RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", "gstreamer-1.0", GstPlugin), Src, StagedFileType.NonUFS);
		}
	}
}
