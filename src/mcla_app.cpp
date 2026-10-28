#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <filesystem>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

REXCVAR_DEFINE_STRING(mcla_gpu_backend, "any", "MCLA",
                      "Graphics backend for the Xenos GPU plugin: any, vulkan or d3d12");


std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher);

// Started with no options (a double-click on mcla.exe), the game should just
// run: find the extracted game next to the executable or, as the build leaves
// it, in the repository a few folders up, and load the Xenos GPU plugin.
void MclaApp::OnConfigurePaths(rex::PathConfig& paths) {
  // --reset-settings: the saved settings (mcla.toml, the menus' choices) put aside before they are read, so every
  // option is back at its default: the way out of a choice the game cannot start with (a render size too large
  // for the card). The file is kept as mcla.toml.reset-<n>. The settings menu's RESET TO DEFAULTS does the same.
#ifdef _WIN32
  if (std::wstring(GetCommandLineW()).find(L"--reset-settings") != std::wstring::npos) {
    std::error_code error;
    if (std::filesystem::exists(paths.config_path, error)) {
      std::filesystem::path aside;
      for (int n = 1; n < 100; ++n) {
        aside = paths.config_path;
        aside += ".reset-" + std::to_string(n);
        if (!std::filesystem::exists(aside, error)) {
          break;
        }
      }
      std::filesystem::rename(paths.config_path, aside, error);
      REXLOG_INFO("settings reset: {} put aside as {}", paths.config_path.string(), aside.filename().string());
    }
  }
#endif
  if (!paths.game_data_root.empty()) {
    return;
  }
  std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
  for (int level = 0; level < 5 && !folder.empty(); ++level) {
    std::error_code error;
    if (std::filesystem::exists(folder / "game" / "default.xex", error)) {
      paths.game_data_root = folder / "game";
      return;
    }
    if (folder == folder.parent_path()) {
      break;
    }
    folder = folder.parent_path();
  }
}

void MclaConfigureGpuDefaults();  // perf_options.cpp
bool VrRequested();               // perf_options.cpp

void MclaApp::OnPreSetup(rex::RuntimeConfig& config) {
  config.audio_factory = &CreateMclaAudioSystem;
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }

  // Left to the SDK when "any"; it then loads the plugin named by --gpu_plugin
  // with its own default backend.
  std::string backend = REXCVAR_GET(mcla_gpu_backend);
  if (backend == "any" && VrRequested()) {
    backend = "vulkan";  // VR runs on the Vulkan backend only
  }
  if (backend != "any") {
    config.graphics = rex::system::LoadGpuPlugin("xenos", backend);
    if (!config.graphics) {
      REXLOG_ERROR("could not load the xenos GPU plugin with backend '{}'", backend);
    }
  }
  MclaConfigureGpuDefaults();  // the GPU plugin's cvars exist from here, and the GPU is not started yet
}

void MclaApp::OnPostSetup() {
  ConfigureFrameTiming();


  // The game probes for loose city data on a "t:" drive that retail consoles
  // do not have. Give it a device so the probes fail with "not found" rather
  // than "no such device". LARecomp does the same.
  if (runtime() && runtime()->file_system()) {
    runtime()->file_system()->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");
  }
}
