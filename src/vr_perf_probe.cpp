// VR fork, development: where the game thread waits for the GPU thread (D3DDevice_BlockOnFence, sub_82411E98).
// --mcla_fence_probe=true logs every 2 s: the waits, their total time, the time by caller (the return address) and
// how many waited for the newest fence (a kick and wait).

#include "generated/default/mcla_init.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mcla_fence_probe, false, "MCLA", "Development: log the game thread's waits for the GPU thread");
REXCVAR_DEFINE_INT32(mcla_swap_depth, 1, "MCLA",
                     "Swaps the game may run ahead of the GPU thread (its Swap waits for the fence of the swap that "
                     "many before; the game's own is 1)")
    .range(1, 4);

REX_EXTERN(__imp__sub_82411E98);

namespace {

uint32_t Load32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

struct Stats {
  uint64_t calls = 0, newest = 0;
  double total_ms = 0;
  std::map<uint32_t, std::pair<uint64_t, double>> by_caller;  // caller -> (calls, ms)
};

}  // namespace

extern "C" REX_FUNC(sub_82411E98) {
  // The swap throttle (D3D Swap, sub_82419E98, return address 0x8241A3FC): with a depth above 1 it waits for an
  // older swap's fence, so the game builds the next frame while the GPU thread still works on the last ones.
  if (uint32_t(ctx.lr) == 0x8241A3FC) {
    static uint32_t history[4] = {};
    const int32_t depth = REXCVAR_GET(mcla_swap_depth);
    const uint32_t fence = ctx.r4.u32;
    if (depth > 1) {
      ctx.r4.u32 = history[depth - 2];
      for (int i = 3; i > 0; --i) {
        history[i] = history[i - 1];
      }
      history[0] = fence;
      if (!ctx.r4.u32) {
        return;
      }
    }
  }
  if (!REXCVAR_GET(mcla_fence_probe)) {
    __imp__sub_82411E98(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32, fence = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  const bool newest = fence && fence == Load32(base, device + 10908);
  const auto start = std::chrono::steady_clock::now();
  __imp__sub_82411E98(ctx, base);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  static std::mutex mutex;
  static Stats stats;
  static auto window = std::chrono::steady_clock::now();
  std::scoped_lock lock{mutex};
  ++stats.calls;
  stats.newest += newest;
  stats.total_ms += ms;
  auto& c = stats.by_caller[caller];
  ++c.first;
  c.second += ms;
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - window).count();
  if (seconds >= 2.0) {
    std::vector<std::pair<uint32_t, std::pair<uint64_t, double>>> callers(stats.by_caller.begin(),
                                                                          stats.by_caller.end());
    std::sort(callers.begin(), callers.end(),
              [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    std::string top;
    for (size_t i = 0; i < std::min<size_t>(callers.size(), 5); ++i) {
      top += fmt::format(" {:08X}: {} x {:.1f} ms;", callers[i].first, callers[i].second.first,
                         callers[i].second.second / seconds);
    }
    REXLOG_INFO("fence waits per second: {:.0f} calls ({:.0f} for the newest fence), {:.1f} ms waiting;{}",
                stats.calls / seconds, stats.newest / seconds, stats.total_ms / seconds, top);
    stats = {};
    window = std::chrono::steady_clock::now();
  }
}
