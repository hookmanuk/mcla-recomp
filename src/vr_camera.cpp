// VR fork: the head-tracked camera. The game's main viewport (the one grcViewport::Perspective is called on from
// sub_822E5E10, return address 0x822E5EB8) takes the headset's field of view, and its camera matrix
// (grcViewport::SetCameraMtx, sub_8217CE80) is turned and moved by the head's pose in the camera's own space. The
// frame then goes to the headset as a projection layer at that pose (vr::SetFramePose).

#include "generated/default/mcla_init.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/vr_openxr.h>

REX_EXTERN(__imp__sub_8217D460);
REX_EXTERN(__imp__sub_8217CE80);
REX_EXTERN(__imp__sub_8217CFD0);
REX_EXTERN(__imp__sub_822AA040);

REXCVAR_DEFINE_DOUBLE(mcla_vr_world_scale, 1.0, "MCLA", "VR: game units per metre of head movement");
REXCVAR_DEFINE_BOOL(mcla_vr_camera_log, false, "MCLA", "VR, development: log the main camera's matrices");
REXCVAR_DEFINE_BOOL(vr_cockpit_sway, false, "VR",
                    "The cockpit camera sliding in the car as it accelerates and brakes (off: held at the seat)");

namespace vr = rex::ui::vulkan::vr;

namespace {

constexpr uint32_t kMainPerspectiveCaller = 0x822E5EB8;  // the render thread, the main viewport's projection
constexpr uint32_t kCameraOriginCaller = 0x82312D3C;      // the update thread, the frame's camera into its viewport
// The update thread's first setting of the frame's camera (from the camera object, sub_8233AF30). The one above is in
// the sun-shadow phase, which the SHADOWS option leaves out: without this one the camera then went unposed.
constexpr uint32_t kCameraObjectCaller = 0x8233B438;
// The render thread's setting of the frame's camera into its viewport, from the copy the update thread kept (unposed),
// before it culls and draws the frame: the city's culling saw the camera without the head's turn (looking 90 degrees
// left at the start, the building there had its walls missing; the right stick, turning the game's camera, did not).
// It gets the posed camera that frame was built with (that viewport's FramePose), so the culling matches the picture.
constexpr uint32_t kRenderCameraCaller = 0x822C08B4;

// The head pose each main viewport's frame was built with (the update thread fills one of the two viewports while
// the render thread draws the other): set at the camera's origin, used at the render thread's Perspective call.
struct FramePose {
  uint32_t viewport = 0;
  bool posed = false;
  float pose[16]{}, tangents[4]{};
  // The frame's camera as the game made it and as posed (the culling's camera: kRenderCameraCaller).
  bool has_camera = false;
  float game[16]{}, out[16]{};
};
std::mutex g_pose_mutex;
FramePose g_frame_poses[2];

// The cockpit camera's sway: the game slides the cockpit camera along the car as it accelerates (~15 cm back) and
// brakes (forward): against the dashboard ~3.5 cm and a degree of pitch. Without vr_cockpit_sway the camera is held
// at its place in the car's body when the car last stood still (the seat). The body is the active camera's target's
// matrix at target + 272 (rows scaled; the camera moves in it as against the dashboard drawn: 0.476 -> 0.441 m while
// the GPU saw 0.994 -> 0.960); the target is at +48 of the camera object whose update (sub_822AA040) rewrites the
// director's camera (director + 0x1C0, the matrix the camera object caller sets). The chassis (*(target + 28) + 16)
// moves under the body: holding the camera to it moved the interior the other way (6% in scale).
std::atomic<uint32_t> g_director{0};   // the camera director (r30 at the camera object caller)
std::atomic<uint32_t> g_car_matrix{0};  // the active camera's car's body matrix

float LoadFloat(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void StoreFloat(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

// Development (mcla_vr_camera_log): the distinct (function, viewport, caller) camera calls every 2 s, each with its
// rate and last camera position and forward row.
std::atomic<int> g_sequence{-400};  // development: one frame's camera calls in order (from the 400th main Perspective)

void ProbeCamera(const char* function, uint32_t viewport, uint32_t caller, uint8_t* base, uint32_t address) {
  const uint8_t* matrix = base + address;
  if (const int n = g_sequence.load(); n >= 0 && n < 80) {
    g_sequence.fetch_add(1);
    REXLOG_INFO("camera seq {} {} viewport {:08X} from {:08X} matrix {:08X} z {:.3f} {:.3f} {:.3f} stored {:.3f} {:.3f}",
                n, function, viewport, caller, address, LoadFloat(matrix + 32), LoadFloat(matrix + 36),
                LoadFloat(matrix + 40), LoadFloat(base + viewport + 64 + 32), LoadFloat(base + viewport + 64 + 40));
  }
  struct Seen {
    uint64_t calls = 0;
    uint32_t matrix = 0;
    float row[8]{};
  };
  static std::mutex mutex;
  static std::map<std::tuple<std::string, uint32_t, uint32_t>, Seen> seen;
  static auto window = std::chrono::steady_clock::now();
  std::scoped_lock lock{mutex};
  auto& entry = seen[{function, viewport, caller}];
  ++entry.calls;
  entry.matrix = address;
  for (int i = 0; i < 4; ++i) {
    entry.row[i] = LoadFloat(matrix + 32 + 4 * i);
    entry.row[4 + i] = LoadFloat(matrix + 48 + 4 * i);
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - window).count();
  if (seconds >= 2.0) {
    for (const auto& [key, e] : seen) {
      REXLOG_INFO("camera {} viewport {:08X} from {:08X} matrix {:08X}: {:.1f}/s z {:.3f} {:.3f} {:.3f} pos {:.1f} {:.1f} {:.1f}",
                  std::get<0>(key), std::get<1>(key), std::get<2>(key), e.matrix, e.calls / seconds, e.row[0], e.row[1],
                  e.row[2], e.row[4], e.row[5], e.row[6]);
    }
    seen.clear();
    window = std::chrono::steady_clock::now();
  }
}

uint32_t LoadU32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

// Holds the cockpit camera (m: rows, the game's camera) at its seat in the car: its place, and its pitch with no roll
// (the game's heading is kept: looking left or right). The seat is where the camera was in the body's frame after the
// car stood still for half a second (learn: the update thread's first setting of a frame); only a camera inside the
// car looking along it (cockpit view, not the bumper, chase or look-back views) is held.
void HoldCockpitCamera(uint8_t* base, float (&m)[16], bool learn) {
  const uint32_t car_address = g_car_matrix.load(std::memory_order_relaxed);
  if (REXCVAR_GET(vr_cockpit_sway) || !car_address) {
    return;
  }
  const uint8_t* car = base + car_address;
  float rows[3][3], position[3];
  for (int r = 0; r < 3; ++r) {
    float length = 0.f;
    for (int c = 0; c < 3; ++c) {
      rows[r][c] = LoadFloat(car + 16 * r + 4 * c);
      length += rows[r][c] * rows[r][c];
    }
    length = std::sqrt(length);
    if (!(length > 1e-3f)) {
      return;
    }
    for (float& v : rows[r]) {
      v /= length;
    }
    position[r] = LoadFloat(car + 48 + 4 * r);
  }
  float local[3];
  for (int r = 0; r < 3; ++r) {
    local[r] = (m[12] - position[0]) * rows[r][0] + (m[13] - position[1]) * rows[r][1] +
               (m[14] - position[2]) * rows[r][2];
  }
  const float along = m[8] * rows[2][0] + m[9] * rows[2][1] + m[10] * rows[2][2];  // the camera's back on the car's
  const bool cockpit = std::abs(local[0]) < 1.2f && local[1] > 0.3f && local[1] < 2.f && std::abs(local[2]) < 1.5f &&
                       along > 0.9f;
  float back[3];  // the camera's back in the body's frame
  for (int r = 0; r < 3; ++r) {
    back[r] = m[8] * rows[r][0] + m[9] * rows[r][1] + m[10] * rows[r][2];
  }
  static uint32_t seat_car = 0;
  static bool have_seat = false;
  static float seat[3], seat_pitch = 0.f, last_position[3];
  static int still = 0;
  if (learn) {
    if (car_address != seat_car) {
      seat_car = car_address;
      have_seat = false;
      still = 0;
    }
    const float dx = position[0] - last_position[0], dy = position[1] - last_position[1],
                dz = position[2] - last_position[2];
    std::memcpy(last_position, position, sizeof(position));
    still = dx * dx + dy * dy + dz * dz < 1e-6f ? still + 1 : 0;  // under 1 mm a frame
    if (cockpit && still >= 30) {
      std::memcpy(seat, local, sizeof(seat));
      seat_pitch = back[1];
      have_seat = true;
    }
  }
  if (!cockpit || !have_seat || seat_car != car_address) {
    return;
  }
  for (int c = 0; c < 3; ++c) {
    m[12 + c] = position[c] + seat[0] * rows[0][c] + seat[1] * rows[1][c] + seat[2] * rows[2][c];
  }
  // In the body's frame: back keeps its heading at the seat's pitch, up is the body's up made square to it.
  const float level = std::sqrt(back[0] * back[0] + back[2] * back[2]);
  if (!(level > 1e-3f)) {
    return;
  }
  const float flat = std::sqrt(std::max(0.f, 1.f - seat_pitch * seat_pitch)) / level;
  const float b[3] = {back[0] * flat, seat_pitch, back[2] * flat};
  float u[3] = {-b[1] * b[0], 1.f - b[1] * b[1], -b[1] * b[2]};  // (0, 1, 0) less its part along b
  const float u_length = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
  for (float& v : u) {
    v /= u_length;
  }
  const float x[3] = {u[1] * b[2] - u[2] * b[1], u[2] * b[0] - u[0] * b[2], u[0] * b[1] - u[1] * b[0]};  // up x back
  const float* axes[3] = {x, u, b};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      m[r * 4 + c] = axes[r][0] * rows[0][c] + axes[r][1] * rows[1][c] + axes[r][2] * rows[2][c];
    }
  }
}

}  // namespace

// The game camera's update (r3 the camera): when it rewrites the director's camera it is the active camera, and its
// target is the car the cockpit camera rides in (HoldCockpitCamera).
extern "C" REX_FUNC(sub_822AA040) {
  const uint32_t camera = ctx.r3.u32, director = g_director.load(std::memory_order_relaxed);
  uint8_t before[64];
  if (director) {
    std::memcpy(before, base + director + 0x1C0, sizeof(before));
  }
  __imp__sub_822AA040(ctx, base);
  if (director && std::memcmp(before, base + director + 0x1C0, sizeof(before)) != 0) {
    const uint32_t target = LoadU32(base + camera + 48);
    g_car_matrix.store(target ? target + 272 : 0, std::memory_order_relaxed);
  }
}

// grcViewport::SetCameraMtx with an explicit view (development probe only).
extern "C" REX_FUNC(sub_8217CFD0) {
  if (REXCVAR_GET(mcla_vr_camera_log)) {
    ProbeCamera("CFD0", ctx.r3.u32, uint32_t(ctx.lr), base, ctx.r4.u32);
  }
  __imp__sub_8217CFD0(ctx, base);
}

// grcViewport::Perspective(this, fov (vertical, degrees), aspect, near, far).
extern "C" REX_FUNC(sub_8217D460) {
  if (uint32_t(ctx.lr) == kMainPerspectiveCaller && REXCVAR_GET(mcla_vr_camera_log)) {
    if (const int n = g_sequence.load(); n < 0) {
      g_sequence.fetch_add(1);
    } else if (n < 80) {
      REXLOG_INFO("camera seq {} Perspective viewport {:08X}", n, ctx.r3.u32);
    }
  }
  if (uint32_t(ctx.lr) == kMainPerspectiveCaller && vr::Active()) {
    FramePose frame;
    {
      std::scoped_lock lock{g_pose_mutex};
      for (const FramePose& f : g_frame_poses) {
        if (f.viewport == ctx.r3.u32) {
          frame = f;
        }
      }
    }
    if (frame.posed) {
      static const bool game_fov = [] {  // development: MCLA_VR_GAME_FOV=1 keeps the game's field of view
        const char* v = std::getenv("MCLA_VR_GAME_FOV");
        return v && v[0] == '1';
      }();
      if (game_fov) {
        const float v = float(std::tan(ctx.f1.f64 * 0.5 / 57.29577951308232)), h = v * float(ctx.f2.f64);
        frame.tangents[0] = -h;
        frame.tangents[1] = h;
        frame.tangents[2] = v;
        frame.tangents[3] = -v;
      } else {
        ctx.f1.f64 = 2.0 * std::atan(double(frame.tangents[2])) * 57.29577951308232;
        ctx.f2.f64 = double(frame.tangents[1]) / double(frame.tangents[2]);
      }
      vr::SetFramePose(frame.pose, frame.tangents);
    }
  }
  __imp__sub_8217D460(ctx, base);
}

// grcViewport::SetCameraMtx(this, const Matrix44* camera): rows right, up, back (the camera looks down -z) and
// position.
extern "C" REX_FUNC(sub_8217CE80) {
  if (REXCVAR_GET(mcla_vr_camera_log)) {
    ProbeCamera("CE80", ctx.r3.u32, uint32_t(ctx.lr), base, ctx.r4.u32);
  }
  if (uint32_t(ctx.lr) == kRenderCameraCaller && vr::Active()) {
    float out[16];
    bool found = false;
    {
      std::scoped_lock lock{g_pose_mutex};
      for (const FramePose& f : g_frame_poses) {
        if (f.viewport != ctx.r3.u32 || !f.posed || !f.has_camera) {
          continue;
        }
        found = true;
        for (int i = 0; i < 16 && found; ++i) {  // the axes and position (the w column can be NaN)
          found = (i & 3) == 3 || std::abs(LoadFloat(base + ctx.r4.u32 + 4 * i) - f.game[i]) <= 1e-3f;
        }
        if (found) {
          std::memcpy(out, f.out, sizeof(out));
          break;
        }
      }
    }
    if (found) {
      static uint32_t scratch = 0;
      if (!scratch) {
        scratch = rex::Runtime::instance()->memory()->SystemHeapAlloc(64, 16);
      }
      for (int i = 0; i < 16; ++i) {
        StoreFloat(base + scratch + 4 * i, out[i]);
      }
      ctx.r4.u32 = scratch;
    }
    __imp__sub_8217CE80(ctx, base);
    return;
  }
  if ((uint32_t(ctx.lr) != kCameraOriginCaller && uint32_t(ctx.lr) != kCameraObjectCaller) || !vr::Active()) {
    __imp__sub_8217CE80(ctx, base);
    return;
  }
  if (uint32_t(ctx.lr) == kCameraObjectCaller) {
    g_director.store(ctx.r30.u32, std::memory_order_relaxed);
  }
  FramePose frame{ctx.r3.u32};
  frame.posed = vr::HeadPose(frame.pose, frame.tangents);
  if (frame.posed) {
    // A symmetric frustum covering both eyes' (Perspective has no off-centre form).
    const float h = std::max(-frame.tangents[0], frame.tangents[1]),
                v = std::max(frame.tangents[2], -frame.tangents[3]);
    frame.tangents[0] = -h;
    frame.tangents[1] = h;
    frame.tangents[2] = v;
    frame.tangents[3] = -v;
  }
  {
    std::scoped_lock lock{g_pose_mutex};
    FramePose& slot = g_frame_poses[g_frame_poses[0].viewport == ctx.r3.u32 || !g_frame_poses[0].viewport ? 0 : 1];
    slot = frame;
  }
  if (!frame.posed) {
    __imp__sub_8217CE80(ctx, base);
    return;
  }
  vr::NoteCameraPose(frame.pose, frame.tangents);  // the pose in the camera now (the GPU draws with it next)
  const float* pose = frame.pose;
  static uint32_t scratch = 0;
  if (!scratch) {
    scratch = rex::Runtime::instance()->memory()->SystemHeapAlloc(64, 16);
  }
  const uint8_t* source = base + ctx.r4.u32;
  float m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = LoadFloat(source + 4 * i);
  }
  // A camera the game read back from a posed viewport (it does every frame: the director's camera, the camera object's
  // second setting) is turned from the camera it came from, not turned again. Any of the last 16 posed cameras counts:
  // the render thread's viewport holds an older frame's (kRenderCameraCaller), and one not recognised was turned twice
  // (looking up and down then moved the camera up, down and along, settling somewhere else).
  struct Posed {
    float in[16], out[16];
  };
  static Posed history[16]{};
  static uint32_t history_next = 0;
  {
    std::scoped_lock lock{g_pose_mutex};
    for (const Posed& p : history) {
      bool same = true;
      for (int i = 0; i < 16 && same; ++i) {  // the axes and position (the w column can be NaN)
        same = (i & 3) == 3 || std::abs(p.out[i] - m[i]) < 1e-3f;
      }
      if (same) {
        std::memcpy(m, p.in, sizeof(m));
        break;
      }
    }
  }
  float game[16];
  std::memcpy(game, m, sizeof(game));
  HoldCockpitCamera(base, m, uint32_t(ctx.lr) == kCameraObjectCaller);
  // The head's axes and position (rows, in the seat's space: x right, y up, z back) taken into the camera's space.
  float out[16];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      out[r * 4 + c] = pose[r * 4 + 0] * m[0 * 4 + c] + pose[r * 4 + 1] * m[1 * 4 + c] +
                       pose[r * 4 + 2] * m[2 * 4 + c];
    }
    out[r * 4 + 3] = m[r * 4 + 3];
  }
  const float scale = float(REXCVAR_GET(mcla_vr_world_scale));
  for (int c = 0; c < 3; ++c) {
    out[12 + c] = m[12 + c] + scale * (pose[12] * m[0 * 4 + c] + pose[13] * m[1 * 4 + c] +
                                       pose[14] * m[2 * 4 + c]);
  }
  out[15] = m[15];
  {
    std::scoped_lock lock{g_pose_mutex};
    Posed& p = history[history_next++ % 16];
    std::memcpy(p.in, game, sizeof(p.in));
    std::memcpy(p.out, out, sizeof(p.out));
  }
  {
    std::scoped_lock lock{g_pose_mutex};
    for (FramePose& f : g_frame_poses) {
      if (f.viewport == ctx.r3.u32) {
        std::memcpy(f.game, game, sizeof(f.game));
        std::memcpy(f.out, out, sizeof(f.out));
        f.has_camera = true;
      }
    }
  }
  uint8_t* target = base + scratch;
  for (int i = 0; i < 16; ++i) {
    StoreFloat(target + 4 * i, out[i]);
  }
  ctx.r4.u32 = scratch;
  __imp__sub_8217CE80(ctx, base);
}
