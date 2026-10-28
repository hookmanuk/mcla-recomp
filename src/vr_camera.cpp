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

REXCVAR_DEFINE_DOUBLE(mcla_vr_world_scale, 1.0, "MCLA", "VR: game units per metre of head movement");
REXCVAR_DEFINE_BOOL(mcla_vr_camera_log, false, "MCLA", "VR, development: log the main camera's matrices");

namespace vr = rex::ui::vulkan::vr;

namespace {

constexpr uint32_t kMainPerspectiveCaller = 0x822E5EB8;  // the render thread, the main viewport's projection
constexpr uint32_t kCameraOriginCaller = 0x82312D3C;      // the update thread, the frame's camera into its viewport
// The update thread's first setting of the frame's camera (from the camera object, sub_8233AF30). The one above is in
// the sun-shadow phase, which the SHADOWS option leaves out: without this one the camera then went unposed.
constexpr uint32_t kCameraObjectCaller = 0x8233B438;

// The head pose each main viewport's frame was built with (the update thread fills one of the two viewports while
// the render thread draws the other): set at the camera's origin, used at the render thread's Perspective call.
struct FramePose {
  uint32_t viewport = 0;
  bool posed = false;
  float pose[16]{}, tangents[4]{};
};
std::mutex g_pose_mutex;
FramePose g_frame_poses[2];

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

}  // namespace

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
  if ((uint32_t(ctx.lr) != kCameraOriginCaller && uint32_t(ctx.lr) != kCameraObjectCaller) || !vr::Active()) {
    __imp__sub_8217CE80(ctx, base);
    return;
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
  // A camera the game read back from the posed viewport (the camera object's second setting in a frame) is turned
  // from the camera it came from, not turned again.
  struct Posed {
    uint32_t source;
    float in[16], out[16];
  };
  static Posed posed[4]{};
  Posed* entry = nullptr;
  for (Posed& p : posed) {
    if (p.source == ctx.r4.u32 || (!entry && !p.source)) {
      entry = &p;
    }
  }
  if (!entry) {
    entry = &posed[ctx.r4.u32 >> 4 & 3];
  }
  if (entry->source == ctx.r4.u32) {
    float difference = 0.f;
    for (int i = 0; i < 16; ++i) {
      difference = std::max(difference, std::abs(entry->out[i] - m[i]));
    }
    static uint32_t calls = 0, read_back = 0;
    read_back += difference < 1e-3f;
    if (++calls % 1000 == 0 && REXCVAR_GET(mcla_vr_camera_log)) {
      REXLOG_INFO("camera: {} of 1000 posed cameras read back from a posed viewport", read_back);
      read_back = 0;
    }
    if (difference < 1e-3f) {
      std::memcpy(m, entry->in, sizeof(m));
    }
  }
  entry->source = ctx.r4.u32;
  std::memcpy(entry->in, m, sizeof(m));
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
  std::memcpy(entry->out, out, sizeof(out));
  uint8_t* target = base + scratch;
  for (int i = 0; i < 16; ++i) {
    StoreFloat(target + 4 * i, out[i]);
  }
  ctx.r4.u32 = scratch;
  __imp__sub_8217CE80(ctx, base);
}
