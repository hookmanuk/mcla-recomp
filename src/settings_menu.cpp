// In-game settings menu.
//
// Adds submenus to the Settings tab of the game's own pause menu, with rows
// bound to this project's options. The rows are objects of the game's own
// option classes (checkbox and value rows), so they look and behave like the
// rest of the menu.
//
// Ported from LARecomp's src/mc_engine/pause_menu.cpp with its author's
// permission; the reverse engineering of the menu system is theirs. See
// config/settings_menu.toml for what each hook address is. Left out: their
// carbon-fibre, cutscene, language and native-renderer tabs.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <vector>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/windowed_app_context.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_motion_blur, true, "MCLA", "Motion blur");
REXCVAR_DEFINE_BOOL(mcla_depth_of_field, true, "MCLA", "Depth of field blur");
// Picture options, applied by the renderer (rexglue command_processor) to the shaders' constants.
REXCVAR_DEFINE_STRING(mcla_car_windows, "game", "MCLA",
                      "Car window glass: game, light (half its haze) or clear");
REXCVAR_DEFINE_BOOL(mcla_sun_glow, true, "MCLA", "The glow around the sun");
REXCVAR_DEFINE_BOOL(mcla_bloom, true, "MCLA", "Bloom (glow around bright areas)");
REXCVAR_DEFINE_BOOL(mcla_colour_grade, true, "MCLA", "The game's warm colour grade (off: neutral colours)");
REXCVAR_DEFINE_BOOL(mcla_settings_menu, true, "MCLA",
                    "Add the settings submenus to the pause menu");

// Set by a host that keeps the settings file somewhere other than next to
// the executable (PS5).
const char* g_mcla_settings_path = nullptr;

namespace rex::ui::vulkan::vr {
double VideoMemoryTotalGB();  // rexglue vr_twins
double VideoMemoryNeededGB(uint32_t scale_area);
namespace dlss {
bool Ready();  // rexglue vr_dlss
}  // namespace dlss
double RefreshHz();  // rexglue vr_openxr
void Recenter();
}  // namespace rex::ui::vulkan::vr

void VrApplyHeadsetSize();  // perf_options.cpp
bool MsaaOff();             // perf_options.cpp

namespace {

// --- Guest addresses ---------------------------------------------------------

constexpr uint32_t kStringTableGlobal = 0x8286D7FC;
constexpr uint32_t kMenuSystemGlobal = 0x8286D804;
constexpr uint32_t kAllocStateFn = 0x8268D578;
constexpr uint32_t kUIObjectCtorFn = 0x8263AD60;
constexpr uint32_t kUIMenuVtable = 0x82020654;
constexpr uint32_t kGuestMallocFn = 0x82130528;
constexpr uint32_t kStackPushFn = 0x8268F6A8;
constexpr uint32_t kStackPopFn = 0x8268DFF0;
constexpr uint32_t kIsButtonClickedFn = 0x82661508;
constexpr uint32_t kFindMovieFn = 0x821F9FB8;
constexpr uint32_t kSetFlashIntFn = 0x825EE0E0;
constexpr uint32_t kGetFlashObjFn = 0x825ED480;
constexpr uint32_t kSetFlashStrFn = 0x827227B8;

constexpr uint32_t kOptionRowCtorFn = 0x826349F0;  // (obj, nameKey, values, wrap)
constexpr uint32_t kLabelRowCtorFn = 0x82633D40;   // (obj, nameKey, childCap)
constexpr uint32_t kSetRowLabelFn = 0x8263B860;    // (obj, nameKey)
constexpr uint32_t kStrListVtable = 0x82091D04;    // {vtable, char** keys, count, stride}
constexpr uint32_t kToggleRowVtable = 0x8209788C;

constexpr uint32_t kStr_PAUSEMOVIE = 0x8201D028;
constexpr uint32_t kStr_menu = 0x82030A34;
constexpr uint32_t kStr_tabs_count = 0x8208D4DC;
constexpr uint32_t kStr_name = 0x8200CC24;

// Row object layout.
constexpr uint32_t kRowObjectSize = 240;
constexpr uint32_t kRowChecked = 208;    // u8, checkbox rows
constexpr uint32_t kRowHasBox = 209;     // u8
constexpr uint32_t kRowEnabled = 212;
constexpr uint32_t kRowProperty1 = 216;
constexpr uint32_t kRowSelect = 208;     // value rows: current value slot

// List view layout.
constexpr uint32_t kListRows = 176;         // row pointer array
constexpr uint32_t kListCountCap = 180;     // count:u16 then capacity:u16
constexpr uint32_t kListSelect = 184;
constexpr uint32_t kListTableSource = 192;
constexpr uint32_t kListStateSource = 196;
constexpr uint32_t kListScroll = 256;

// --- Guest memory helpers ----------------------------------------------------

uint8_t* Membase() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->virtual_membase() : nullptr;
}

uint32_t Read32(uint32_t ea) {
  const uint8_t* p = Membase() + ea;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t Read16(uint32_t ea) {
  const uint8_t* p = Membase() + ea;
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void Write32(uint32_t ea, uint32_t value) {
  uint8_t* p = Membase() + ea;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void Write8(uint32_t ea, uint8_t value) {
  Membase()[ea] = value;
}

template <typename... Args>
uint32_t CallGuest(uint32_t function_address, Args... args) {
  auto* runtime = rex::Runtime::instance();
  PPCFunc* function = runtime ? runtime->function_dispatcher()->GetFunction(function_address)
                              : nullptr;
  if (!function) {
    REXLOG_WARN("settings menu: guest function 0x{:08X} is not available", function_address);
    return 0;
  }
  return rex::ppc::GuestToHostFunction<uint32_t>(function, static_cast<uint32_t>(args)...);
}

uint32_t AllocGuestString(const char* text) {
  const size_t size = std::strlen(text) + 1;
  uint32_t buffer = CallGuest(kGuestMallocFn, uint32_t(size));
  if (buffer) {
    std::memcpy(Membase() + buffer, text, size);
  }
  return buffer;
}

// --- The game's string table -------------------------------------------------

// The game's string hash (sub_821C9790).
uint32_t HashString(const char* text) {
  uint32_t hash = 0;
  const bool quoted = (*text == '"');
  if (quoted) {
    ++text;
  }
  while (*text) {
    char c = *text;
    if (quoted && c == '"') {
      break;
    }
    ++text;
    uint8_t ch = static_cast<uint8_t>(c);
    if (ch >= 'A' && ch <= 'Z') {
      ch += 32;
    } else if (ch == '\\') {
      ch = '/';
    }
    uint32_t v = ch + hash;
    hash = ((1025u * v) >> 6) ^ (1025u * v);
  }
  return 32769u * (((9u * hash) >> 11) ^ (9u * hash));
}

uint32_t HashMapLookup(uint32_t map, uint32_t hash) {
  uint32_t buckets = Read32(map);
  uint16_t bucket_count = Read16(map + 4);
  if (!bucket_count || !buckets) {
    return 0;
  }
  for (uint32_t node = Read32(buckets + 4 * (hash % bucket_count)); node; node = Read32(node + 8)) {
    if (Read32(node) == hash) {
      return Read32(node + 4);
    }
  }
  return 0;
}

bool HashMapInsert(uint32_t map, uint32_t hash, uint32_t value) {
  uint32_t buckets = Read32(map);
  uint16_t bucket_count = Read16(map + 4);
  if (!bucket_count || !buckets) {
    return false;
  }
  uint32_t node = CallGuest(kGuestMallocFn, 12u);
  if (!node) {
    return false;
  }
  const uint32_t slot = buckets + 4 * (hash % bucket_count);
  Write32(node + 0, hash);
  Write32(node + 4, value);
  Write32(node + 8, Read32(slot));
  Write32(slot, node);
  return true;
}

// Sets the display text for a string-table key, creating the entry if needed.
// Text buffers are 132 bytes.
void SetStringTableText(const char* key, const char* text) {
  uint32_t table = Read32(kStringTableGlobal);
  if (!table) {
    return;
  }
  size_t length = std::strlen(text);
  if (length > 131) {
    length = 131;
  }
  const uint32_t hash = HashString(key);
  uint32_t buffer = HashMapLookup(table + 16, hash);
  if (!buffer) {
    buffer = CallGuest(kGuestMallocFn, 132u);
    if (!buffer || !HashMapInsert(table + 16, hash, buffer)) {
      return;
    }
  }
  std::memcpy(Membase() + buffer, text, length);
  Membase()[buffer + length] = 0;
}

// --- The game's menu state tree ----------------------------------------------

void DetachMenuItem(uint32_t child) {
  uint32_t parent = Read32(child + 32);
  if (!parent) {
    return;
  }
  uint32_t previous = Read32(child + 40);
  uint32_t next = Read32(child + 36);
  if (previous) {
    Write32(previous + 36, next);
  } else {
    Write32(parent + 44, next);
  }
  if (next) {
    Write32(next + 40, previous);
  }
  Write32(child + 32, 0);
  Write32(child + 36, 0);
  Write32(child + 40, 0);
}

// Reimplements sub_8268CC80.
void AppendMenuItem(uint32_t parent, uint32_t child) {
  if (!parent || !child || Read32(child + 32) != 0) {
    return;
  }
  uint32_t head = Read32(parent + 44);
  if (!head) {
    Write32(parent + 44, child);
    Write32(child + 40, 0);
  } else {
    uint32_t last = head;
    for (uint32_t next; (next = Read32(last + 36)) != 0; last = next) {
    }
    Write32(last + 36, child);
    Write32(child + 40, last);
  }
  Write32(child + 32, parent);
  Write32(child + 36, 0);
}

// The same, as the first child: VR heads the settings (Matt 2026-10-30).
void PrependMenuItem(uint32_t parent, uint32_t child) {
  if (!parent || !child || Read32(child + 32) != 0) {
    return;
  }
  const uint32_t head = Read32(parent + 44);
  Write32(child + 36, head);
  Write32(child + 40, 0);
  if (head) {
    Write32(head + 40, child);
  }
  Write32(parent + 44, child);
  Write32(child + 32, parent);
}

uint32_t MenuRootNode() {
  uint32_t root = Read32(kMenuSystemGlobal);
  return root ? Read32(root + 52) : 0;
}

uint32_t MenuEngine() {
  uint32_t node = MenuRootNode();
  return node ? node + 4 : 0;
}

void SetStateName(uint32_t state, const char* name) {
  if (uint32_t buffer = AllocGuestString(name)) {
    Write32(state + 20, buffer);
  }
}

// A plain menu state, as the engine's own factory makes them.
uint32_t CreateMenuState(const char* name) {
  uint32_t engine = MenuEngine();
  if (!engine) {
    return 0;
  }
  uint32_t state = CallGuest(kAllocStateFn, engine);
  if (!state) {
    return 0;
  }
  const uint32_t index = Read32(engine + 80) - 1;
  HashMapInsert(engine + 88, HashString(name), index);
  SetStateName(state, name);
  Write32(state + 16, 0x42);
  for (uint32_t offset = 24; offset <= 44; offset += 4) {
    Write32(state + offset, 0);
  }
  return state;
}

// A "UIMenu" state: what the class factory (sub_82222080) builds for one.
uint32_t CreateUIMenuState(const char* name) {
  uint32_t engine = MenuEngine();
  if (!engine) {
    return 0;
  }
  uint32_t state = CallGuest(kGuestMallocFn, 56u);
  if (!state) {
    return 0;
  }
  CallGuest(kUIObjectCtorFn, state);
  Write32(state, kUIMenuVtable);
  uint32_t array = Read32(engine + 76);
  uint32_t count = Read32(engine + 80);
  if (!array) {
    return 0;
  }
  Write32(array + 4 * count, state);
  Write32(engine + 80, count + 1);
  HashMapInsert(engine + 88, HashString(name), count);
  SetStateName(state, name);
  return state;
}

bool MenuStackPush(uint32_t state) {
  uint32_t node = MenuRootNode();
  if (!node) {
    return false;
  }
  CallGuest(kStackPushFn, node + 776, node + 4, state, 0u);
  return true;
}

void MenuStackPop() {
  if (uint32_t node = MenuRootNode()) {
    CallGuest(kStackPopFn, node + 776, node + 4, 1u);
  }
}

// --- Options -----------------------------------------------------------------

std::string OptionGet(const char* name) {
  return rex::cvar::GetFlagByName(name);
}

std::mutex g_changed_mutex;
std::vector<std::string> g_changed_options;  // set in the menu since the last save

void OptionSet(const char* name, const char* value) {
  REXLOG_INFO("settings menu: {} = {}", name, value);
  rex::cvar::SetFlagByName(name, value);
  if (std::strcmp(name, "vr_headset_size") == 0) {
    VrApplyHeadsetSize();  // live: DLSS's output follows
  }
  std::lock_guard lock(g_changed_mutex);
  if (std::find(g_changed_options.begin(), g_changed_options.end(), name) == g_changed_options.end()) {
    g_changed_options.emplace_back(name);
  }
}

// The menu's options into the settings file: each changed one replaces its line or is added; every other line stays.
void SaveChangedOptions(const std::filesystem::path& path) {
  std::vector<std::string> changed;
  {
    std::lock_guard lock(g_changed_mutex);
    changed.swap(g_changed_options);
  }
  if (changed.empty()) {
    return;
  }
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      lines.push_back(line);
    }
  }
  if (lines.empty()) {
    lines.emplace_back("# MCLA settings (the in-game settings menu writes what it changes)");
  }
  for (const std::string& name : changed) {
    const std::string value = rex::cvar::GetFlagByName(name);
    const bool bare = value == "true" || value == "false" ||
                      (!value.empty() && value.find_first_not_of("0123456789.-") == std::string::npos);
    const std::string line = name + " = " + (bare ? value : "\"" + value + "\"");
    bool replaced = false;
    for (std::string& existing : lines) {
      const size_t key_end = existing.find_first_of(" =");
      if (key_end != std::string::npos && existing.compare(0, key_end, name) == 0 &&
          existing.find('=') != std::string::npos) {
        existing = line;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      lines.push_back(line);
    }
  }
  std::ofstream out(path, std::ios::trunc);
  for (const std::string& line : lines) {
    out << line << "\n";
  }
}

enum class ItemKind { kToggle, kStringChoice, kNumberChoice, kAction };

struct ItemDef {
  const char* key;    // menu state name and string-table key; must be unique
  ItemKind kind;
  const char* option;
  const char* label;
  const char* const* values = nullptr;        // kStringChoice: option values
  const char* const* value_labels = nullptr;  // kStringChoice: display text
  const double* numbers = nullptr;            // kNumberChoice
  int value_count = 0;
  const char* number_format = nullptr;
  const char* zero_label = nullptr;  // kNumberChoice: shown instead of 0
};

// Display text must stay within A-Z, 0-9, space, '+', '.', and parentheses.
// A value's text is also the name of the Flash element created for it, and
// characters such as '/' are path separators there (LARecomp found that the
// hard way: it crashes UI setup).
#if REX_PLATFORM_PS5
// The console's display is 60 Hz.
constexpr double kFpsTargets[] = {30.0, 60.0};
#else
constexpr double kFpsTargets[] = {30.0, 60.0, 120.0, 0.0};
#endif
constexpr int kFpsTargetCount = int(sizeof(kFpsTargets) / sizeof(kFpsTargets[0]));
constexpr const char* kPromptValues[] = {"auto", "xbox", "playstation"};
constexpr const char* kPromptLabels[] = {"AUTO", "XBOX", "PLAYSTATION"};
constexpr const char* kCarWindowValues[] = {"game", "light", "clear"};
constexpr const char* kCarWindowLabels[] = {"GAME", "LIGHT", "CLEAR"};
constexpr const char* kIntroValues[] = {"normal", "fast", "skip"};
constexpr const char* kIntroLabels[] = {"NORMAL", "FAST", "SKIP (NO LOGO)"};

const ItemDef kDisplayItems[] = {
    {"PM_MxFps", ItemKind::kNumberChoice, "mcla_fps", "FPS TARGET", nullptr, nullptr, kFpsTargets,
     kFpsTargetCount, "%.0f", "UNCAPPED"},
#if !REX_PLATFORM_PS5
    // Neither means anything on the console: there is one display mode, and
    // the PS5 host turns the emulated vsync off at every start.
    {"PM_MxVsync", ItemKind::kToggle, "vsync", "VSYNC"},
    {"PM_MxFullscreen", ItemKind::kToggle, "fullscreen", "FULLSCREEN"},
#endif
    {"PM_MxMotionBlur", ItemKind::kToggle, "mcla_motion_blur", "MOTION BLUR"},
    {"PM_MxDof", ItemKind::kToggle, "mcla_depth_of_field", "DEPTH OF FIELD"},
    {"PM_MxCarWindows", ItemKind::kStringChoice, "mcla_car_windows", "CAR WINDOWS", kCarWindowValues,
     kCarWindowLabels, nullptr, 3},
    {"PM_MxSunGlow", ItemKind::kToggle, "mcla_sun_glow", "SUN GLOW"},
    {"PM_MxBloom", ItemKind::kToggle, "mcla_bloom", "BLOOM"},
    {"PM_MxColourGrade", ItemKind::kToggle, "mcla_colour_grade", "COLOUR GRADE"},
};

// Detail the game can do without when the frame rate matters more; all as
// shipped by default (src/perf_options.cpp). Rows marked NEXT START are read
// by the game once, when it starts.
constexpr double kDetailScales[] = {0.5, 0.75, 1.0};
constexpr double kDensityScales[] = {0.0, 0.5, 1.0};
constexpr double kTrafficDistances[] = {250.0, 325.0, 400.0};

const ItemDef kPerformanceItems[] = {
    {"PM_MxSingleTile", ItemKind::kToggle, "mcla_single_tile", "SINGLE TILE RENDERING"},
    {"PM_MxFenceYield", ItemKind::kToggle, "mcla_fence_yield", "GPU WAIT YIELD"},
    {"PM_MxShadows", ItemKind::kToggle, "mcla_shadows", "SHADOWS"},
    {"PM_MxFoliageShadows", ItemKind::kToggle, "mcla_foliage_shadows", "FOLIAGE SHADOWS"},
    {"PM_MxCityLod", ItemKind::kNumberChoice, "mcla_city_lod", "CITY DETAIL DISTANCE", nullptr,
     nullptr, kDetailScales, 3, "%.2f"},
    {"PM_MxTrafficLod", ItemKind::kNumberChoice, "mcla_traffic_lod", "TRAFFIC DETAIL DISTANCE",
     nullptr, nullptr, kDetailScales, 3, "%.2f"},
    {"PM_MxTrafficDist", ItemKind::kNumberChoice, "mcla_traffic_distance", "TRAFFIC DISTANCE",
     nullptr, nullptr, kTrafficDistances, 3, "%.0f"},
    {"PM_MxPeds", ItemKind::kNumberChoice, "mcla_pedestrians", "PEDESTRIANS", nullptr, nullptr,
     kDensityScales, 3, "%.1f", "NONE"},
    {"PM_MxParked", ItemKind::kNumberChoice, "mcla_parked_cars", "PARKED CARS", nullptr, nullptr,
     kDensityScales, 3, "%.1f", "NONE"},
    {"PM_MxRaceShadows", ItemKind::kToggle, "mcla_race_shadows", "RACE SHADOWS (NEXT START)"},
    {"PM_MxFastCarShadows", ItemKind::kToggle, "mcla_fast_car_shadows",
     "SIMPLE CAR SHADOWS (NEXT START)"},
    {"PM_MxImpostors", ItemKind::kToggle, "mcla_foliage_impostors", "DISTANT TREES (NEXT LOAD)"},
    {"PM_MxFsBlur", ItemKind::kToggle, "mcla_fullscreen_blur", "FULLSCREEN BLUR (NEXT START)"},
    {"PM_MxMsaa", ItemKind::kToggle, "mcla_msaa", "ANTI ALIASING (NEXT START)"},
};

// VR: the render size an eye (a draw resolution scale on 1280x720) and the DLSS mode (its upscale to the headset);
// both are read at start (perf_options.cpp). The headset gets the render size times the upscale.
// Render sizes: the draw scale XxY on 1280x720, in steps of about a quarter of 2x4's pixels (2x3 and 3x5 have a little
// more detail across than down). Values stay short (the value column clips past ~14 characters): the size and the
// video memory it needs (TOO BIG past the card less 2 GB; at start the game would lower it: vr_twins.cpp).
constexpr const char* kVrResolutionValues[] = {"1x2", "2x3", "2x4", "3x5", "3x6"};
constexpr int kVrResolutionCount = 5;
const char* kVrResolutionLabels[kVrResolutionCount] = {"1280X1440", "2560X2160", "2560X2880", "3840X3600",
                                                       "3840X4320"};

void VrResolutionLabels() {
  static std::string labels[kVrResolutionCount];
  for (int i = 0; i < kVrResolutionCount; ++i) {
    const int x = kVrResolutionValues[i][0] - '0', y = kVrResolutionValues[i][2] - '0';
    const double needed = rex::ui::vulkan::vr::VideoMemoryNeededGB(uint32_t(x * y));
    const double total = rex::ui::vulkan::vr::VideoMemoryTotalGB();
    char text[32];
    if (total > 0.0 && needed + 2.0 > total) {
      std::snprintf(text, sizeof text, "%dX%d TOO BIG", 1280 * x, 720 * y);
    } else {
      std::snprintf(text, sizeof text, "%dX%d %.0fGB", 1280 * x, 720 * y, needed);
    }
    labels[i] = text;
    kVrResolutionLabels[i] = labels[i].c_str();
  }
}

// The size an eye sent to the headset: the render size (no anti-aliasing, the game's MSAA at the next start, or DLSS's
// anti-aliasing) or an upscale of it by DLSS (up to 2x), each factor labelled with the size it makes from the render size the
// game started with; only sizes up to 7680 on a side are listed (VrHeadsetLabels sets the count). DLSS needs an NVIDIA
// RTX GPU: without it those say NO RTX.
constexpr int kVrHeadsetMax = 7;
const char* kVrHeadsetValues[kVrHeadsetMax] = {"noaa", "msaa", "dlaa", "1.25", "1.5", "1.75", "2"};
const char* kVrHeadsetLabels[kVrHeadsetMax] = {"NO AA", "MSAA (RESTART)", "DLAA"};

// DLSS's model (NGX's render preset), live: NGX's choice per mode, the transformers (K, J; L, M lighter) or the older
// convolutional ones (F, E).
constexpr const char* kVrDlssModelValues[] = {"default", "K", "J", "M", "L", "F", "E"};
const char* kVrDlssModelLabels[] = {"DEFAULT", "K TRANSFORMER", "J TRANSFORMER", "M TRANSFORMER 2",
                                    "L TRANSFORMER 2", "F CNN", "E CNN"};

// The frame cap: frames to OpenXR at 1/N of the headset's refresh (for an external frame generator), live; labels with
// the rate they make (VrFrameCapLabels, when the menu is made).
constexpr const char* kVrFrameCapValues[] = {"1", "2", "3"};
const char* kVrFrameCapLabels[] = {"OFF", "1/2", "1/3"};

void VrFrameCapLabels() {
  static std::string labels[3];
  const double hz = rex::ui::vulkan::vr::RefreshHz();
  labels[0] = "OFF";
  for (int n = 2; n <= 3; ++n) {
    char text[32];
    if (hz > 0.0) {
      std::snprintf(text, sizeof text, "1/%d (%.0f FPS)", n, hz / n);
    } else {
      std::snprintf(text, sizeof text, "1/%d", n);
    }
    labels[n - 1] = text;
  }
  for (int i = 0; i < 3; ++i) {
    kVrFrameCapLabels[i] = labels[i].c_str();
  }
}

ItemDef kVrItems[] = {
    {"PM_MxVrRecentre", ItemKind::kAction, nullptr, "RECENTRE"},
    {"PM_MxVrRes", ItemKind::kStringChoice, "vr_resolution", "RENDER SIZE (RESTART)", kVrResolutionValues,
     kVrResolutionLabels, nullptr, kVrResolutionCount},
    {"PM_MxVrHeadset", ItemKind::kStringChoice, "vr_headset_size", "HEADSET SIZE",
     kVrHeadsetValues, kVrHeadsetLabels, nullptr, kVrHeadsetMax},
    {"PM_MxVrDlssModel", ItemKind::kStringChoice, "vr_dlss_model", "DLSS MODEL", kVrDlssModelValues,
     kVrDlssModelLabels, nullptr, 7},
    {"PM_MxVrCockpitSway", ItemKind::kToggle, "vr_cockpit_sway", "COCKPIT SWAY"},
    {"PM_MxVrFrameCap", ItemKind::kStringChoice, "vr_frame_cap", "FRAME RATE CAP", kVrFrameCapValues,
     kVrFrameCapLabels, nullptr, 3},
    {"PM_MxVrRestart", ItemKind::kAction, nullptr, "RESTART GAME"},
    {"PM_MxVrReset", ItemKind::kAction, nullptr, "RESET TO DEFAULTS"},
};

// What a restart applies (RESTART GAME is enabled when either differs from the start): the render size and MSAA (the
// headset size's MSAA, or the anti-aliasing toggle outside VR). Noted when the menu is first made, before any change.
std::string g_start_resolution;
bool g_start_msaa_off = false;

bool RestartNeeded() {
  return OptionGet("vr_resolution") != g_start_resolution || MsaaOff() != g_start_msaa_off;
}

void SaveSettingsIfChanged();

// An action row's state: RESTART GAME once a restart would apply something, RECENTRE and RESET TO DEFAULTS always.
bool ActionEnabled(const ItemDef& item) {
  return std::strcmp(item.key, "PM_MxVrReset") == 0 || std::strcmp(item.key, "PM_MxVrRecentre") == 0 ||
         RestartNeeded();
}

// The settings saved, the same program started again with the same command line, and this one ended at once (the
// orderly quit hangs with the headset session open).
void RestartGame(bool reset = false) {
  if (!reset) {
    SaveSettingsIfChanged();
  }
#ifdef _WIN32
  wchar_t path[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
    REXLOG_ERROR("settings menu: restart: no program path");
    return;
  }
  std::wstring command = GetCommandLineW();
  if (reset) {  // RESET TO DEFAULTS: the new process puts mcla.toml aside before it reads it (MclaApp)
    command += L" --reset-settings";
  }
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
    REXLOG_ERROR("settings menu: restart: the new process did not start ({})", GetLastError());
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  REXLOG_INFO("settings menu: restarting");
  rex::FlushLogging();
  TerminateProcess(GetCurrentProcess(), 0);
#else
  REXLOG_INFO("settings menu: restart is not available here");
#endif
}

void VrHeadsetLabels() {
  static std::string labels[kVrHeadsetMax];
  static const double kFactors[] = {1.25, 1.5, 1.75, 2.0};  // up to 2x (Matt 2026-10-27)
  const bool ready = rex::ui::vulkan::vr::dlss::Ready();
  const int render_width = 1280 * std::max(1, std::atoi(rex::cvar::GetFlagByName("draw_resolution_scale_x").c_str()));
  const int render_height = 720 * std::max(1, std::atoi(rex::cvar::GetFlagByName("draw_resolution_scale_y").c_str()));
  labels[0] = "NO AA";
  labels[1] = "MSAA (RESTART)";
  labels[2] = ready ? "DLAA" : "DLAA NO RTX";
  int count = 3;
  for (double factor : kFactors) {
    const int width = int(render_width * factor + 0.5) & ~1, height = int(render_height * factor + 0.5) & ~1;
    if (width > 7680 || height > 7680) {
      break;
    }
    char text[32];
    if (ready) {
      std::snprintf(text, sizeof text, "%dX%d %gX", width, height, factor);
    } else {
      std::snprintf(text, sizeof text, "%dX%d NO RTX", width, height);
    }
    labels[count] = text;
    ++count;
  }
  for (int i = 0; i < count; ++i) {
    kVrHeadsetLabels[i] = labels[i].c_str();
  }
  for (ItemDef& item : kVrItems) {
    if (std::strcmp(item.key, "PM_MxVrHeadset") == 0) {
      item.value_count = count;
    }
  }
}

const ItemDef kControlItems[] = {
    {"PM_MxPrompts", ItemKind::kStringChoice, "mcla_button_prompts", "BUTTON PROMPTS",
     kPromptValues, kPromptLabels, nullptr, 3},
    {"PM_MxIntro", ItemKind::kStringChoice, "mcla_intro", "INTRO", kIntroValues, kIntroLabels,
     nullptr, 3},
};

struct MenuDef {
  const char* button_key;  // button state name and its string-table key
  const char* label;       // button label and submenu title
  const char* menu_key;    // submenu state name
  const ItemDef* items;
  int item_count;
};

const MenuDef kMenus[] = {
    {"PM_MxTabDisplay", "DISPLAY", "MxDisplayMenu", kDisplayItems,
     int(sizeof(kDisplayItems) / sizeof(kDisplayItems[0]))},
    {"PM_MxTabPerf", "PERFORMANCE", "MxPerfMenu", kPerformanceItems,
     int(sizeof(kPerformanceItems) / sizeof(kPerformanceItems[0]))},
    {"PM_MxTabControls", "CONTROLS", "MxControlsMenu", kControlItems,
     int(sizeof(kControlItems) / sizeof(kControlItems[0]))},
    {"PM_MxTabVr", "VR", "MxVrMenu", kVrItems, int(sizeof(kVrItems) / sizeof(kVrItems[0]))},
};
constexpr int kMenuCount = int(sizeof(kMenus) / sizeof(kMenus[0]));
constexpr int kMaxRows = 16;

int ItemValueCount(const ItemDef& item) {
  return item.kind == ItemKind::kToggle ? 2 : item.kind == ItemKind::kAction ? 1 : item.value_count;
}

std::string ItemValueLabel(const ItemDef& item, int index) {
  switch (item.kind) {
    case ItemKind::kToggle:
      return index ? "ON" : "OFF";
    case ItemKind::kAction:
      return "";
    case ItemKind::kStringChoice:
      return item.value_labels[index];
    case ItemKind::kNumberChoice: {
      if (item.zero_label && item.numbers[index] == 0.0) {
        return item.zero_label;
      }
      char buffer[48];
      std::snprintf(buffer, sizeof buffer, item.number_format, item.numbers[index]);
      return buffer;
    }
  }
  return "";
}

int ItemCurrentIndex(const ItemDef& item) {
  if (item.kind == ItemKind::kAction) {
    return 0;
  }
  const std::string value = OptionGet(item.option);
  switch (item.kind) {
    case ItemKind::kToggle:
      return value == "true" ? 1 : 0;
    case ItemKind::kAction:
      return 0;
    case ItemKind::kStringChoice:
      for (int i = 0; i < item.value_count; ++i) {
        if (value == item.values[i]) {
          return i;
        }
      }
      return 0;
    case ItemKind::kNumberChoice: {
      const double number = std::strtod(value.c_str(), nullptr);
      int best = 0;
      double best_distance = -1.0;
      for (int i = 0; i < item.value_count; ++i) {
        const double distance = number > item.numbers[i] ? number - item.numbers[i]
                                                         : item.numbers[i] - number;
        if (best_distance < 0.0 || distance < best_distance) {
          best_distance = distance;
          best = i;
        }
      }
      return best;
    }
  }
  return 0;
}

std::atomic<bool> g_settings_dirty{false};

void ItemSetIndex(const ItemDef& item, int index) {
  if (index < 0 || index >= ItemValueCount(item) || item.kind == ItemKind::kAction) {
    return;
  }
  switch (item.kind) {
    case ItemKind::kToggle:
      OptionSet(item.option, index ? "true" : "false");
      break;
    case ItemKind::kStringChoice:
      OptionSet(item.option, item.values[index]);
      break;
    case ItemKind::kAction:
      break;
    case ItemKind::kNumberChoice: {
      char buffer[32];
      std::snprintf(buffer, sizeof buffer, "%g", item.numbers[index]);
      OptionSet(item.option, buffer);
      break;
    }
  }
  g_settings_dirty.store(true, std::memory_order_relaxed);
}

// --- Rows --------------------------------------------------------------------

// A value row shows three slots (previous, current, next) with the selection
// on the middle one. The game's row class writes one Flash element per value
// without checking how many the row clip has, and three is what its own
// numeric rows use, so the window never exceeds three however long the real
// list is. After every step the window is re-centred, which also makes a step
// readable: a selection that is no longer on slot 1 is the direction pressed.
constexpr int kRowWindow = 3;

struct MenuRows {
  uint32_t array = 0;
  uint32_t row[kMaxRows] = {};
  int last_index[kMaxRows] = {};
  int count = 0;
};

MenuRows g_rows[kMenuCount];
std::atomic<uint32_t> g_menu_button_key[kMenuCount] = {};
std::atomic<uint32_t> g_menu_state[kMenuCount] = {};
std::atomic<uint32_t> g_menu_title[kMenuCount] = {};
std::atomic<int> g_active_menu{-1};
std::atomic<bool> g_menus_created{false};

std::atomic<uint32_t> g_pause_tab{0};
std::atomic<uint32_t> g_settings_state{0};
std::atomic<uint32_t> g_last_list{0};
std::atomic<uint32_t> g_pause_list{0};

// The pause list's own row sources, saved while one of our submenus borrows it.
struct SavedList {
  uint32_t rows, count_cap, table, state, select, scroll;
};
SavedList g_saved_list{};

void WindowKey(const ItemDef& item, int slot, char* out, size_t size) {
  std::snprintf(out, size, "%s_w%d", item.key, slot);
}

void CentreRowWindow(MenuRows& rows, int i, const ItemDef& item, int index) {
  const int count = ItemValueCount(item);
  for (int slot = 0; slot < kRowWindow; ++slot) {
    char key[96];
    WindowKey(item, slot, key, sizeof key);
    const int value = ((index + slot - 1) % count + count) % count;
    SetStringTableText(key, ItemValueLabel(item, value).c_str());
  }
  rows.last_index[i] = index;
  Write32(rows.row[i] + kRowSelect, 1);
}

uint32_t BuildValueSet(const ItemDef& item) {
  uint32_t keys = CallGuest(kGuestMallocFn, uint32_t(4 * kRowWindow));
  if (!keys) {
    return 0;
  }
  for (int slot = 0; slot < kRowWindow; ++slot) {
    char key[96];
    WindowKey(item, slot, key, sizeof key);
    SetStringTableText(key, "");
    uint32_t key_string = AllocGuestString(key);
    if (!key_string) {
      return 0;
    }
    Write32(keys + uint32_t(4 * slot), key_string);
  }
  uint32_t set = CallGuest(kGuestMallocFn, 16u);
  if (!set) {
    return 0;
  }
  Write32(set + 0, kStrListVtable);
  Write32(set + 4, keys);
  Write32(set + 8, uint32_t(kRowWindow));
  Write32(set + 12, 1);
  return set;
}

bool BuildRows(int menu) {
  MenuRows& rows = g_rows[menu];
  if (rows.array) {
    return true;
  }
  const MenuDef& def = kMenus[menu];
  if (def.item_count > kMaxRows) {
    return false;
  }
  uint32_t array = CallGuest(kGuestMallocFn, uint32_t(4 * def.item_count));
  if (!array) {
    return false;
  }
  for (int i = 0; i < def.item_count; ++i) {
    const ItemDef& item = def.items[i];
    // The row constructor resolves its label through the string table once,
    // so the text has to exist first.
    SetStringTableText(item.key, item.label);
    uint32_t name = AllocGuestString(item.key);
    uint32_t row = CallGuest(kGuestMallocFn, kRowObjectSize);
    if (!name || !row) {
      return false;
    }
    std::memset(Membase() + row, 0, kRowObjectSize);

    if (item.kind == ItemKind::kAction) {  // a label row, greyed out until it would do something
      CallGuest(kLabelRowCtorFn, row, name, 0u);
      Write32(row + kRowEnabled, ActionEnabled(item) ? 1 : 0);
      rows.row[i] = row;
      rows.last_index[i] = 0;
    } else if (item.kind == ItemKind::kToggle) {
      // The checkbox class has no constructor of its own: the label row's
      // constructor runs and the checkbox vtable is written over it.
      CallGuest(kLabelRowCtorFn, row, name, 0u);
      Write32(row, kToggleRowVtable);
      Write8(row + kRowChecked, uint8_t(ItemCurrentIndex(item)));
      Write8(row + kRowHasBox, 1);
      Write32(row + kRowEnabled, 1);
      Write32(row + kRowProperty1, 0xFFFFFFFFu);
      rows.row[i] = row;
      rows.last_index[i] = ItemCurrentIndex(item);
    } else {
      uint32_t set = BuildValueSet(item);
      if (!set) {
        return false;
      }
      CallGuest(kOptionRowCtorFn, row, name, set, 1u);
      rows.row[i] = row;
      CentreRowWindow(rows, i, item, ItemCurrentIndex(item));
    }
    Write32(array + uint32_t(4 * i), row);
  }
  rows.count = def.item_count;
  rows.array = array;
  REXLOG_INFO("settings menu: '{}' built with {} rows", def.label, rows.count);
  return true;
}

// Points the list at our rows. The renderer only reaches the row array when
// the two higher-priority sources are null, so those are cleared.
void InstallRows(int menu, uint32_t list) {
  MenuRows& rows = g_rows[menu];
  if (!list || !rows.array || rows.count <= 0) {
    return;
  }
  const uint32_t count_cap = (uint32_t(rows.count) << 16) | uint32_t(rows.count);
  Write32(list + kListStateSource, 0);
  Write32(list + kListTableSource, 0);
  Write32(list + kListRows, rows.array);
  Write32(list + kListCountCap, count_cap);
}

// Brings every row in line with the current option values.
void SyncRowsFromOptions(int menu) {
  MenuRows& rows = g_rows[menu];
  const MenuDef& def = kMenus[menu];
  for (int i = 0; i < rows.count; ++i) {
    const ItemDef& item = def.items[i];
    const int index = ItemCurrentIndex(item);
    if (item.kind == ItemKind::kAction) {
      Write32(rows.row[i] + kRowEnabled, ActionEnabled(item) ? 1 : 0);
    } else if (item.kind == ItemKind::kToggle) {
      rows.last_index[i] = index;
      Write8(rows.row[i] + kRowChecked, uint8_t(index));
    } else {
      CentreRowWindow(rows, i, item, index);
    }
  }
}

// Left and right are handled inside the row, which then re-renders the list.
// The render is where the move is noticed and written to the option.
void PollRows(int menu) {
  MenuRows& rows = g_rows[menu];
  const MenuDef& def = kMenus[menu];
  for (int i = 0; i < rows.count; ++i) {
    const ItemDef& item = def.items[i];
    const int count = ItemValueCount(item);
    if (item.kind == ItemKind::kAction) {
      Write32(rows.row[i] + kRowEnabled, ActionEnabled(item) ? 1 : 0);
      continue;
    }
    if (item.kind == ItemKind::kToggle) {
      const int checked = ItemCurrentIndex(item);
      if (checked != rows.last_index[i]) {
        rows.last_index[i] = checked;
        Write8(rows.row[i] + kRowChecked, uint8_t(checked));
      }
      continue;
    }
    const int slot = int(Read32(rows.row[i] + kRowSelect));
    if (slot != 1) {
      const int direction = slot < 1 ? -1 : +1;
      const int index = ((rows.last_index[i] + direction) % count + count) % count;
      ItemSetIndex(item, index);
      CentreRowWindow(rows, i, item, index);
    }
  }
}

// --- Pause list control ------------------------------------------------------

uint32_t PauseMovieContext() {
  uint32_t root = Read32(kMenuSystemGlobal);
  if (!root) {
    return 0;
  }
  uint32_t movie = CallGuest(kFindMovieFn, root, kStr_PAUSEMOVIE);
  return movie ? Read32(movie + 56) : 0;
}

void RefreshPauseList() {
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return;
  }
  uint32_t vtable = Read32(list);
  uint32_t function = vtable ? Read32(vtable + 176) : 0;
  if (function) {
    CallGuest(function, list);
  }
}

void ShowSubmenu() {
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (menu < 0 || !list) {
    return;
  }
  InstallRows(menu, list);
  RefreshPauseList();

  uint32_t context = PauseMovieContext();
  if (!context) {
    return;
  }
  // Hide the tab strip so the movie presents a submenu, as the game's own
  // time-of-day menu does.
  CallGuest(kSetFlashIntFn, context, kStr_tabs_count, 0u);

  uint32_t title = g_menu_title[menu].load(std::memory_order_relaxed);
  if (!title) {
    title = AllocGuestString(kMenus[menu].label);
    g_menu_title[menu].store(title, std::memory_order_relaxed);
  }
  uint32_t menu_object = CallGuest(kGetFlashObjFn, context, HashString("menu"), kStr_menu);
  if (menu_object && title) {
    CallGuest(kSetFlashStrFn, menu_object, kStr_name, title);
  }
}

void RestorePauseList() {
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return;
  }
  Write32(list + kListRows, g_saved_list.rows);
  Write32(list + kListCountCap, g_saved_list.count_cap);
  Write32(list + kListTableSource, g_saved_list.table);
  Write32(list + kListStateSource, g_saved_list.state);
  Write32(list + kListSelect, g_saved_list.select);
  Write32(list + kListScroll, g_saved_list.scroll);
  RefreshPauseList();
}

void SaveSettingsIfChanged() {
  if (!g_settings_dirty.exchange(false, std::memory_order_relaxed)) {
    return;
  }
  // The file the runtime loads at start: <executable folder>/mcla.toml, or
  // the file a host without a writable executable folder names.
  const auto path = g_mcla_settings_path
                        ? std::filesystem::path(g_mcla_settings_path)
                        : rex::filesystem::GetExecutableFolder() / "mcla.toml";
  SaveChangedOptions(path);
  REXLOG_INFO("settings menu: saved to {}", path.string());
}

void CreateSubmenus() {
  if (g_menus_created.load(std::memory_order_relaxed)) {
    return;
  }
  g_start_resolution = OptionGet("vr_resolution");  // before any change in the menu
  g_start_msaa_off = MsaaOff();
  VrResolutionLabels();  // the card's memory and DLSS are known once the GPU has started
  VrHeadsetLabels();
  VrFrameCapLabels();
  uint32_t settings = g_settings_state.load(std::memory_order_relaxed);
  if (!settings) {
    return;
  }
  for (int menu = 0; menu < kMenuCount; ++menu) {
    const MenuDef& def = kMenus[menu];

    uint32_t button = CreateMenuState(def.button_key);
    if (!button) {
      return;
    }
    if (std::strcmp(def.button_key, "PM_MxTabVr") == 0) {
      PrependMenuItem(settings, button);  // VR first (Matt 2026-10-30)
    } else {
      AppendMenuItem(settings, button);
    }
    SetStringTableText(def.button_key, def.label);
    g_menu_button_key[menu].store(AllocGuestString(def.button_key), std::memory_order_relaxed);

    uint32_t submenu = CreateUIMenuState(def.menu_key);
    if (!submenu) {
      return;
    }
    // Give the submenu a parent pointer without listing it as a child: the
    // engine's "find my root" walk dereferences a null result for a state
    // with no parent.
    Write32(submenu + 32, settings);
    g_menu_state[menu].store(submenu, std::memory_order_relaxed);
    SetStringTableText(def.menu_key, def.label);

    // One child state per item keeps the tree the shape the engine expects,
    // although the list draws the row objects, not these.
    for (int i = 0; i < def.item_count; ++i) {
      if (uint32_t state = CreateMenuState(def.items[i].key)) {
        AppendMenuItem(submenu, state);
      }
    }
    BuildRows(menu);
  }

  g_menus_created.store(true, std::memory_order_relaxed);
  REXLOG_INFO("settings menu: {} submenus added to the pause menu", kMenuCount);
}

bool ButtonClicked(uint32_t key) {
  return key && CallGuest(kIsButtonClickedFn, key, 1u) != 0;
}

// True when `list` is reachable from `node` through the widget child arrays.
// Most widgets keep children at +68; the tab strip keeps its tabs at +180.
bool NodeReachesList(uint32_t node, uint32_t list, int depth, int& budget) {
  if (!node || depth > 8 || --budget < 0) {
    return false;
  }
  for (uint32_t offset : {68u, 180u}) {
    uint32_t array = Read32(node + offset);
    uint16_t count = Read16(node + offset + 4);
    if (array < 0xB0000000 || !count || count > 64) {
      continue;
    }
    for (uint16_t i = 0; i < count; ++i) {
      uint32_t child = Read32(array + uint32_t(4 * i));
      if (child == list) {
        return true;
      }
      if (child >= 0xB0000000 && NodeReachesList(child, list, depth + 1, budget)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// --- Hooks -------------------------------------------------------------------

void VrPauseMenuBuilt();  // VR fork, below

void mcla_menu_capture_continue(PPCRegister& r3) {
  VrPauseMenuBuilt();
  if (r3.u32) {
    if (uint32_t pause_tab = Read32(r3.u32 + 32)) {
      g_pause_tab.store(pause_tab, std::memory_order_relaxed);
    }
  }
}

bool mcla_menu_enable_save(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(mcla_settings_menu)) {
    return false;
  }
  const uint32_t save_button = r3.u32;
  if (save_button && !g_settings_state.load(std::memory_order_relaxed)) {
    if (uint32_t parent = Read32(save_button + 32)) {
      g_settings_state.store(parent, std::memory_order_relaxed);
    }
  }
  uint32_t pause_tab = g_pause_tab.load(std::memory_order_relaxed);
  if (save_button && pause_tab) {
    DetachMenuItem(save_button);
    AppendMenuItem(pause_tab, save_button);
    SetStringTableText("Save (Dev Only)", "SAVE GAME");
  }
  CreateSubmenus();
  r4.u64 = 1;
  return true;
}

bool mcla_menu_click(PPCRegister& r3, PPCRegister& r31) {
  (void)r31;
  if (!g_menus_created.load(std::memory_order_relaxed)) {
    return false;
  }
  const int active = g_active_menu.load(std::memory_order_relaxed);
  if (active >= 0) {
    // Accept on a row steps its value forward. Every press inside a submenu is
    // consumed so the game never resolves it against the Settings tab's items.
    uint32_t list = g_pause_list.load(std::memory_order_relaxed);
    const uint32_t index = list ? Read32(list + kListSelect) : 0xFFFFFFFFu;
    const MenuDef& def = kMenus[active];
    if (index < uint32_t(def.item_count)) {
      const ItemDef& item = def.items[index];
      if (item.kind == ItemKind::kAction) {
        if (std::strcmp(item.key, "PM_MxVrRecentre") == 0) {
          rex::ui::vulkan::vr::Recenter();
          REXLOG_INFO("VR: recentred (the VR menu)");
        } else if (std::strcmp(item.key, "PM_MxVrReset") == 0) {
          RestartGame(true);
        } else if (RestartNeeded()) {
          RestartGame();
        }
        r3.u64 = 1;
        return true;
      }
      const int count = ItemValueCount(item);
      ItemSetIndex(item, (ItemCurrentIndex(item) + 1) % count);
      SyncRowsFromOptions(active);
      ShowSubmenu();
    }
    r3.u64 = 1;
    return true;
  }

  for (int menu = 0; menu < kMenuCount; ++menu) {
    if (!ButtonClicked(g_menu_button_key[menu].load(std::memory_order_relaxed))) {
      continue;
    }
    uint32_t submenu = g_menu_state[menu].load(std::memory_order_relaxed);
    uint32_t list = g_last_list.load(std::memory_order_relaxed);
    if (!submenu || !list || !BuildRows(menu)) {
      return false;
    }
    g_pause_list.store(list, std::memory_order_relaxed);
    g_saved_list = {Read32(list + kListRows),        Read32(list + kListCountCap),
                    Read32(list + kListTableSource), Read32(list + kListStateSource),
                    Read32(list + kListSelect),      Read32(list + kListScroll)};
    Write32(list + kListSelect, 0);
    Write32(list + kListScroll, 0);

    g_active_menu.store(menu, std::memory_order_relaxed);
    SyncRowsFromOptions(menu);
    MenuStackPush(submenu);
    ShowSubmenu();
    REXLOG_INFO("settings menu: opened '{}'", kMenus[menu].label);
    r3.u64 = 1;
    return true;
  }
  return false;
}

// VR fork: whether the pause menu is open (VrPauseMenuOpen): the game builds it when it opens (the capture hook) and
// cancels it when it closes (B, or Start again), building it again straight after; a build within 100 ms of a cancel
// is that one, not an open.
std::atomic<bool> g_vr_pause_open{false};
std::atomic<int64_t> g_vr_pause_cancel_ms{0};

int64_t VrNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The pause menu's builds and cancels so far (VrPauseMenuEvents): Start in the game opens or closes it.
std::atomic<uint32_t> g_vr_pause_events{0};
uint32_t VrPauseMenuEvents() { return g_vr_pause_events.load(); }

void VrPauseMenuBuilt() {
  ++g_vr_pause_events;
  if (VrNowMs() - g_vr_pause_cancel_ms.load() > 100 && !g_vr_pause_open.exchange(true)) {
    REXLOG_INFO("VR: pause menu open");
  }
}

bool mcla_menu_cancel(PPCRegister& r31) {
  if (g_active_menu.load(std::memory_order_relaxed) < 0) {  // outside our submenus: the pause menu closes
    g_vr_pause_cancel_ms = VrNowMs();
    ++g_vr_pause_events;
    if (g_vr_pause_open.exchange(false)) {
      REXLOG_INFO("VR: pause menu closed");
    }
  }
  if (g_active_menu.exchange(-1, std::memory_order_relaxed) < 0) {
    return false;
  }
  // Pop first, then hand the list back, in that order.
  MenuStackPop();
  RestorePauseList();
  SaveSettingsIfChanged();
  REXLOG_INFO("settings menu: closed");
  return true;
}

bool mcla_menu_populate_redirect(PPCRegister& r3) {
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  uint32_t settings = g_settings_state.load(std::memory_order_relaxed);
  if (menu >= 0 && settings && r3.u32 == settings) {
    if (uint32_t submenu = g_menu_state[menu].load(std::memory_order_relaxed)) {
      r3.u64 = submenu;
    }
  }
  return false;
}

bool mcla_menu_list_populate(PPCRegister& r3) {
  const uint32_t list = r3.u32;
  if (!list) {
    return false;
  }
  g_last_list.store(list, std::memory_order_relaxed);
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  if (menu >= 0 && list == g_pause_list.load(std::memory_order_relaxed)) {
    PollRows(menu);
    InstallRows(menu, list);
  }
  return false;
}

bool mcla_menu_key_dispatch(PPCRegister& r31, PPCRegister& r28, PPCRegister& r27) {
  if (g_active_menu.load(std::memory_order_relaxed) < 0) {
    return false;
  }
  const uint32_t key = r28.u32 & 0xFF;
  if (key < '3' || key > '6') {  // left is '3' or '5', right is '4' or '6'
    return false;
  }
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return false;
  }
  int budget = 512;
  if (!NodeReachesList(r31.u32, list, 0, budget)) {
    return false;
  }
  uint32_t vtable = Read32(list);
  uint32_t function = vtable ? Read32(vtable + 32) : 0;
  if (!function) {
    return false;
  }
  CallGuest(function, list, key, r27.u32);
  return true;
}

bool mcla_menu_null_table_guard(PPCRegister& r30) {
  return r30.u32 == 0;
}

bool mcla_menu_list_vtable_guard(PPCRegister& r31) {
  const uint32_t list = r31.u32;
  return list && Read32(list) == 0;
}

bool mcla_motion_blur_gate(PPCRegister& r3) {
  if (REXCVAR_GET(mcla_motion_blur)) {
    return false;
  }
  r3.u64 = 0;
  return true;
}

void mcla_dof_composite(PPCRegister& r3) {
  if (REXCVAR_GET(mcla_depth_of_field) || !r3.u32) {
    return;
  }
  std::memset(Membase() + r3.u32 + 0xF0, 0, 16);
}
