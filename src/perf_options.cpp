// Optional detail reductions and a few crash guards. See
// config/perf_options.toml for what each hook address is and where the
// analysis came from (LARecomp, with its author's permission).
//
// Every option defaults to the game as it shipped. They exist because the
// frame rate here is limited by how many draws the GPU emulation gets through
// in a frame, and these are the game's own ways of asking for fewer.

#include "generated/default/mcla_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(int32_t, mcla_fps);  // frame_timing.cpp

REXCVAR_DEFINE_BOOL(mcla_shadows, true, "MCLA", "Real-time shadows. Off is the largest saving");
REXCVAR_DEFINE_BOOL(mcla_race_shadows, true, "MCLA",
                    "Shadows during races (applies at the next start of the game)");
REXCVAR_DEFINE_BOOL(mcla_fast_car_shadows, false, "MCLA",
                    "A simple blob shadow under cars instead of the real-time one (next start)");
REXCVAR_DEFINE_BOOL(mcla_foliage_shadows, true, "MCLA", "Shadows cast by distant foliage");
REXCVAR_DEFINE_BOOL(mcla_foliage_impostors, true, "MCLA",
                    "Billboards for distant trees (applies at the next load of the city)");
REXCVAR_DEFINE_BOOL(mcla_fullscreen_blur, true, "MCLA", "The full-screen blur pass (next start)");
REXCVAR_DEFINE_BOOL(mcla_msaa, true, "MCLA", "Multisample anti-aliasing (next start)");
REXCVAR_DEFINE_DOUBLE(mcla_city_lod, 1.0, "MCLA",
                      "Scale of the distance at which the city switches to less detail")
    .range(0.1, 4.0);
REXCVAR_DEFINE_DOUBLE(mcla_traffic_lod, 1.0, "MCLA",
                      "Scale of the distance at which traffic switches to less detail")
    .range(0.1, 4.0);
REXCVAR_DEFINE_DOUBLE(mcla_traffic_distance, 400.0, "MCLA",
                      "Distance in metres beyond which ambient traffic is removed (400 as shipped)")
    .range(100.0, 600.0);
REXCVAR_DEFINE_DOUBLE(mcla_pedestrians, 1.0, "MCLA", "Pedestrian density scale").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(mcla_parked_cars, 1.0, "MCLA", "Parked car density scale").range(0.0, 1.0);

// For measuring what an option is worth. The title screen's camera wanders, so
// two runs never show the same scene; this switches the named option between
// as-shipped and reduced every four seconds within one run and logs frame time
// and draws for each state, which compares like with like.
REXCVAR_DEFINE_STRING(mcla_ab_test, "", "MCLA",
                      "Development: alternate one option every 4 s and log the frame cost of each "
                      "state. shadows, foliage, lod, density or all");

namespace {

uint8_t* Base() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->virtual_membase() : nullptr;
}

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

float ReadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Read32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

void WriteFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  Write32(base, address, bits);
}

// Anything below the first 64 KiB is not an object.
bool LooksLikePointer(uint32_t value) {
  return value >= 0x10000;
}

// --- Developer switches -------------------------------------------------------

struct DevSwitch {
  const char* name;  // as the game registers it
  bool (*wanted)();
};
const DevSwitch kDevSwitches[] = {
    {"noraceshadows", [] { return !REXCVAR_GET(mcla_race_shadows); }},
    {"fastVehShadows", [] { return bool(REXCVAR_GET(mcla_fast_car_shadows)); }},
    {"noimpostors", [] { return !REXCVAR_GET(mcla_foliage_impostors); }},
    {"nofsblur", [] { return !REXCVAR_GET(mcla_fullscreen_blur); }},
};

// The value every switch gets: a guest string "1", which satisfies both the
// consumers that test the pointer and the ones that parse a number from it.
uint32_t DevSwitchValue() {
  static uint32_t address = 0;
  if (!address) {
    auto* runtime = rex::Runtime::instance();
    if (!runtime || !runtime->memory()) {
      return 0;
    }
    address = runtime->memory()->SystemHeapAlloc(16);
    if (address) {
      uint8_t* text = runtime->virtual_membase() + address;
      std::memset(text, 0, 16);
      text[0] = '1';
    }
  }
  return address;
}

// --- Shadow render phases -------------------------------------------------------

// The renderer object and its mask of enabled render phases, which the frame's
// phase loop reads every frame. The sun-shadow cascades are 0x61E0 (what the
// game's own "noshadows" clears); the shadow drawn at night is phase 0x400.
constexpr uint32_t kRendererPointer = 0x8287E064;
constexpr uint32_t kPhaseEnableMask = 448;
constexpr uint32_t kShadowPhases = 0x65E0;

void ApplyShadowPhases(uint8_t* base) {
  static bool cleared = false;
  static uint32_t were_set = 0;
  const bool want_off = !REXCVAR_GET(mcla_shadows);
  if (!want_off && !cleared) {
    return;
  }
  const uint32_t renderer = Read32(base, kRendererPointer);
  if (!LooksLikePointer(renderer)) {
    return;
  }
  const uint32_t mask = Read32(base, renderer + kPhaseEnableMask);
  if (want_off) {
    if (!cleared) {
      were_set = mask & kShadowPhases;
      cleared = true;
      REXLOG_INFO("perf options: shadow phases off (mask {:#x})", mask);
    }
    // Every frame: the game may turn them back on.
    if (mask & kShadowPhases) {
      Write32(base, renderer + kPhaseEnableMask, mask & ~kShadowPhases);
    }
  } else {
    Write32(base, renderer + kPhaseEnableMask, mask | were_set);
    cleared = false;
    REXLOG_INFO("perf options: shadow phases back on");
  }
}

// --- Ambient density ------------------------------------------------------------

// The tuning fields at the start of an ambient zone.
constexpr uint32_t kZoneTrafficUnspawn = 0x10;  // 400.0 as shipped
constexpr uint32_t kZonePedestrians = 0x60;
constexpr uint32_t kZoneParkedCars = 0x98;

struct Zone {
  uint32_t address;
  float unspawn, pedestrians, parked;  // as the game set them
};
std::mutex g_zones_mutex;
std::vector<Zone> g_zones;
double g_applied_distance = 400.0, g_applied_pedestrians = 1.0, g_applied_parked = 1.0;

void WriteZone(uint8_t* base, const Zone& zone) {
  const double distance = REXCVAR_GET(mcla_traffic_distance);
  // The option is the shipped zone's 400 m scaled; zones keep their own ratio.
  WriteFloat(base, zone.address + kZoneTrafficUnspawn, float(zone.unspawn * (distance / 400.0)));
  WriteFloat(base, zone.address + kZonePedestrians,
             float(zone.pedestrians * REXCVAR_GET(mcla_pedestrians)));
  WriteFloat(base, zone.address + kZoneParkedCars,
             float(zone.parked * REXCVAR_GET(mcla_parked_cars)));
}

void ApplyAmbientIfChanged(uint8_t* base) {
  const double distance = REXCVAR_GET(mcla_traffic_distance);
  const double pedestrians = REXCVAR_GET(mcla_pedestrians);
  const double parked = REXCVAR_GET(mcla_parked_cars);
  if (distance == g_applied_distance && pedestrians == g_applied_pedestrians &&
      parked == g_applied_parked) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_zones_mutex);
  for (const Zone& zone : g_zones) {
    WriteZone(base, zone);
  }
  g_applied_distance = distance;
  g_applied_pedestrians = pedestrians;
  g_applied_parked = parked;
  REXLOG_INFO("perf options: ambient density applied to {} zones", g_zones.size());
}

}  // namespace

namespace {

uint64_t NowMicros() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void SetReduced(const std::string& what, bool reduced) {
  // Any cvar: "cvar:NAME:SHIPPED:REDUCED" (VR fork, development).
  if (what.rfind("cvar:", 0) == 0) {
    const size_t a = what.find(':', 5), b = a == std::string::npos ? a : what.find(':', a + 1);
    if (b != std::string::npos) {
      rex::cvar::SetFlagByName(what.substr(5, a - 5), reduced ? what.substr(b + 1) : what.substr(a + 1, b - a - 1));
    }
    return;
  }
  const bool all = what == "all";
  if (all || what == "shadows") {
    rex::cvar::SetFlagByName("mcla_shadows", reduced ? "false" : "true");
  }
  if (all || what == "foliage") {
    rex::cvar::SetFlagByName("mcla_foliage_shadows", reduced ? "false" : "true");
  }
  if (all || what == "lod") {
    rex::cvar::SetFlagByName("mcla_city_lod", reduced ? "0.5" : "1.0");
    rex::cvar::SetFlagByName("mcla_traffic_lod", reduced ? "0.5" : "1.0");
  }
  if (all || what == "density") {
    rex::cvar::SetFlagByName("mcla_traffic_distance", reduced ? "250" : "400");
    rex::cvar::SetFlagByName("mcla_pedestrians", reduced ? "0.5" : "1.0");
    rex::cvar::SetFlagByName("mcla_parked_cars", reduced ? "0.5" : "1.0");
  }
}

void TickAbTest() {
  static const std::string what = REXCVAR_GET(mcla_ab_test);
  if (what.empty()) {
    return;
  }
  struct State {
    uint64_t frames = 0, micros = 0, draws = 0;
  };
  static State states[2];
  static bool reduced = false;
  static uint64_t switched_at = 0, last_frame = 0, last_report = 0;
  const uint64_t now = NowMicros();
  if (!switched_at) {
    switched_at = last_frame = last_report = now;
    return;
  }
  // The first second after a switch is the change settling, not the state.
  if (now - switched_at > 1'000'000) {
    State& state = states[reduced ? 1 : 0];
    ++state.frames;
    state.micros += now - last_frame;
    state.draws += uint64_t(rex::perf::GetSnapshotCounter(rex::perf::CounterId::kDrawCalls));
  }
  last_frame = now;
  if (now - switched_at > 4'000'000) {
    reduced = !reduced;
    SetReduced(what, reduced);
    switched_at = now;
  }
  if (now - last_report > 30'000'000) {
    last_report = now;
    for (int i = 0; i < 2; ++i) {
      const State& state = states[i];
      if (state.frames) {
        REXLOG_INFO("ab test '{}' {}: {} frames, {:.2f} ms a frame, {:.0f} draws a frame", what,
                    i ? "reduced" : "as shipped", state.frames,
                    double(state.micros) / double(state.frames) / 1000.0,
                    double(state.draws) / double(state.frames));
      }
    }
  }
}

}  // namespace

// Before the GPU starts: the shared memory's hot pages. The game rewrites much of its geometry and constant memory
// every frame; watching those pages costs a write fault on the game thread and a protection change on the GPU thread
// for each, every frame. A page faulting twice within a second stays unwatched (shared_memory_hot_page_faults) and
// is uploaded whole when a draw needs it, only if it differs from its last upload (shared_memory_hot_page_compare).
// Free roam, stereo VR: 63 -> 78 FPS parked, frame 14.5 -> 13.0 ms driving. The page state is kept from frame to
// frame (clear_memory_page_state off, as the PS5 port does): otherwise every page in use is copied again each frame
// (1.6 GB/s -> 69 MB/s; stereo VR 80 -> 98 FPS parked). Values given on the command line win.
//
// In VR (MCLA_VR=1): twice the vertical resolution (1280x1440 an eye: about square pixels over the headset's square
// field of view), no motion blur or depth of field (both blur head turns; the eye focuses for itself), and DLSS's
// anti-aliasing in place of the game's MSAA when DLSS starts, its output upscaled 1.5x to the headset (1920x2160 an
// eye: sharper on a high-resolution headset, ~0.4 ms; vr_dlss_output_scale=1 keeps DLAA at the render size).
// VR: --vr=true, or MCLA_VR=1.
bool VrRequested() {
  const char* v = std::getenv("MCLA_VR");
  return (v && v[0] == '1') || rex::cvar::Query<bool>("vr");
}

void MclaConfigureGpuDefaults() {
  std::vector<std::pair<const char*, const char*>> defaults = {
      {"shared_memory_hot_page_faults", "2"},
      {"shared_memory_hot_page_exact", "0"},
      {"clear_memory_page_state", "false"},
  };
  if (VrRequested()) {
    defaults.insert(defaults.end(), {{"draw_resolution_scale_y", "2"},
                                     {"mcla_motion_blur", "false"},
                                     {"mcla_depth_of_field", "false"},
                                     {"vr_dlss", "true"},
                                     {"vr_dlss_output_scale", "1.5"}});
    REXLOG_INFO("VR: 2x vertical resolution, no motion blur or depth of field, DLSS to 1.5x (unless set)");
  }
  for (const auto& [name, value] : defaults) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName(name, value);
    }
  }
  if (VrRequested() && REXCVAR_GET(mcla_fps) != 0) {
    // VR paces the game at the headset's refresh rate itself; the game's limiter (30 by default) would hold it below.
    REXLOG_INFO("VR: the game's frame-rate target {} off (the headset paces it)", REXCVAR_GET(mcla_fps));
    rex::cvar::SetFlagByName("mcla_fps", "0");
  }
}

// Once a frame, from the frame timer hook.
void MclaTickPerfOptions() {
  uint8_t* base = Base();
  if (!base) {
    return;
  }
  TickAbTest();
  ApplyShadowPhases(base);
  ApplyAmbientIfChanged(base);
}

void mcla_dev_option_registered(PPCRegister& r3) {
  uint8_t* base = Base();
  const uint32_t node = r3.u32;
  if (!base || !LooksLikePointer(node)) {
    return;
  }
  const uint32_t name = Read32(base, node);
  if (!LooksLikePointer(name)) {
    return;
  }
  for (const DevSwitch& dev_switch : kDevSwitches) {
    if (std::strcmp(reinterpret_cast<const char*>(base + name), dev_switch.name) != 0) {
      continue;
    }
    if (dev_switch.wanted()) {
      if (const uint32_t value = DevSwitchValue()) {
        Write32(base, node + 4, value);
        REXLOG_INFO("perf options: developer switch '{}' set", dev_switch.name);
      }
    }
    return;
  }
}

bool mcla_foliage_shadows(PPCRegister& r11) {
  if (REXCVAR_GET(mcla_foliage_shadows)) {
    return false;
  }
  r11.u64 = 0;
  return true;
}

namespace rex::ui::vulkan::vr::dlss {
bool Ready();  // rexglue vr_dlss
}

// MSAA off: asked for, or in VR when DLSS anti-aliases instead (it runs and MSAA is not asked for).
bool MsaaOff() {
  static const bool vr_default = VrRequested() && rex::cvar::GetFlagSource("mcla_msaa") == rex::cvar::Source::kDefault;
  return !REXCVAR_GET(mcla_msaa) || (vr_default && rex::ui::vulkan::vr::dlss::Ready());
}

bool mcla_msaa(PPCRegister& r11) {
  if (!MsaaOff()) {
    return false;
  }
  r11.u64 = 1;
  return true;
}

void mcla_msaa_request(PPCRegister& r4) {
  if (r4.u32 && MsaaOff()) {
    r4.u64 = 0;
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("MSAA off: the game's multisample requests are held off");
    }
  }
}

void mcla_tiling_msaa(PPCRegister& r6) {
  if (r6.u32 > 1 && MsaaOff()) {
    r6.u64 = 1;
  }
}

bool mcla_impostor_search_guard(PPCRegister& r3) {
  const uint8_t* base = Base();
  if (!base) {
    return false;
  }
  const uint32_t manager = r3.u32;
  if (!manager) {
    return true;
  }
  const uint32_t array = Read32(base, manager + 16);
  if (!array) {
    return true;
  }
  const uint32_t count = (uint32_t(base[array + 12]) << 8) | base[array + 13];
  const uint32_t entries = Read32(base, array + 8);
  if (!count || !entries) {
    return true;
  }
  // The search wants an entry with both of these set; let it run only if
  // there is one.
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = entries + i * 224;
    if (Read32(base, entry + 168) && Read32(base, entry + 196)) {
      return false;
    }
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("perf options: no impostor to refresh in {} slots; search skipped", count);
  }
  return true;
}

void mcla_city_lod(PPCRegister& f13) {
  f13.f64 *= REXCVAR_GET(mcla_city_lod);
}

void mcla_traffic_lod(PPCRegister& f0) {
  f0.f64 *= REXCVAR_GET(mcla_traffic_lod);
}

void mcla_ambient_zone(PPCRegister& r3) {
  uint8_t* base = Base();
  const uint32_t address = r3.u32;
  if (!base || !LooksLikePointer(address)) {
    return;
  }
  Zone zone{address, ReadFloat(base, address + kZoneTrafficUnspawn),
            ReadFloat(base, address + kZonePedestrians), ReadFloat(base, address + kZoneParkedCars)};
  std::lock_guard<std::mutex> lock(g_zones_mutex);
  bool known = false;
  for (Zone& existing : g_zones) {
    if (existing.address == address) {
      existing = zone;
      known = true;
    }
  }
  if (!known) {
    g_zones.push_back(zone);
  }
  WriteZone(base, zone);
}

bool mcla_ui_lights_guard(PPCRegister& r3) {
  const uint8_t* base = Base();
  const uint32_t node = r3.u32;
  if (base && LooksLikePointer(node) && Read32(base, node + 8) == 5) {
    return false;
  }
  static std::atomic<int> reported{0};
  if (reported.fetch_add(1) < 4) {
    REXLOG_WARN("guard: UI movie without a lights node ({:#010x}); lighting pass skipped", node);
  }
  return true;
}

bool mcla_brain_reset_guard(PPCRegister& r3) {
  const uint8_t* base = Base();
  const uint32_t context = r3.u32;
  if (!base || !LooksLikePointer(context)) {
    return false;
  }
  const uint32_t arguments = Read32(base, context + 8);
  if (LooksLikePointer(arguments) && LooksLikePointer(Read32(base, arguments))) {
    return false;
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_WARN("guard: racing AI reset with no brain; skipped");
  }
  return true;
}

bool mcla_map_cursor_guard(PPCRegister& r31, PPCRegister& r30, PPCRegister& r26) {
  if (r31.u32 != 0) {
    return false;
  }
  uint8_t* base = Base();
  if (!base) {
    return false;
  }
  if (r26.u32 != r30.u32) {
    std::memmove(base + r26.u32, base + r30.u32, 16);
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_WARN("guard: map cursor step found no neighbouring cell; position taken unclamped");
  }
  return true;
}

// sub_821D5510 is the game's data cache flush: a loop of dcbf/dcbst over a
// range, one 128-byte line at a time, called about 5,000 times a second. The
// host needs none of it. A fence stands in for the one thing other threads
// (the audio decoder and mixer) could rely on it for: that what was written is
// visible to them afterwards. r3 is left as it came in, as the original does.
extern "C" REX_FUNC(sub_821D5510) {
  (void)ctx;
  (void)base;
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
