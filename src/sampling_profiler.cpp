// Built-in sampling profiler (development aid, Windows only for now).
//
// --mcla_profile=<file> starts a thread that waits --mcla_profile_delay
// seconds, then for --mcla_profile_seconds samples the instruction pointer of
// every other thread about once a millisecond. Samples inside recompiled game
// code are attributed to their guest function through the dispatch table;
// everything else is written as module + offset for scripts/profile_report.py
// to symbolise.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

REXCVAR_DEFINE_STRING(mcla_profile, "", "MCLA", "Write a sampling profile to this file");
REXCVAR_DEFINE_INT32(mcla_profile_delay, 40, "MCLA", "Seconds to wait before sampling")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(mcla_profile_seconds, 15, "MCLA", "Seconds to sample for").range(1, 600);

// Defined in crash_trace.cpp.
uint32_t MclaGuestFunctionForHostPc(uintptr_t host_pc);

#ifdef _WIN32
namespace {

struct Sample {
  uint32_t thread_id;
  uint64_t pc;
  uint64_t caller;  // MCLA_PROFILE_CALLERS: outside our modules, the nearest return address into them on the stack
};

// Our modules (mcla.exe, rexruntime.dll, rexgpu-xenos.dll): [start, end).
std::vector<std::pair<uint64_t, uint64_t>> OurModules() {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  for (const char* name : {static_cast<const char*>(nullptr), "rexruntime.dll", "rexgpu-xenos.dll"}) {
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(name));
    if (!base) {
      continue;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    ranges.emplace_back(uint64_t(base), uint64_t(base) + nt->OptionalHeader.SizeOfImage);
  }
  return ranges;
}

std::vector<uint32_t> OtherThreadIds() {
  std::vector<uint32_t> ids;
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return ids;
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof entry;
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self) {
        ids.push_back(entry.th32ThreadID);
      }
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return ids;
}

void RunProfile(std::string path, int delay_s, int seconds) {
  Sleep(static_cast<DWORD>(delay_s) * 1000);
  // MCLA_PROFILE_TRIGGER=<file>: after the delay, sampling waits for the file to appear (a watcher's cue).
  if (const char* trigger = std::getenv("MCLA_PROFILE_TRIGGER"); trigger && *trigger) {
    while (GetFileAttributesA(trigger) == INVALID_FILE_ATTRIBUTES) {
      Sleep(100);
    }
  }

  std::vector<std::pair<uint32_t, HANDLE>> threads;
  for (uint32_t id : OtherThreadIds()) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, id);
    if (h) {
      threads.emplace_back(id, h);
    }
  }

  // Nothing may allocate or log between SuspendThread and ResumeThread: the
  // suspended thread could be holding the heap or logger lock.
  std::vector<Sample> samples;
  samples.resize(static_cast<size_t>(seconds) * 1100 * threads.size());
  static const bool callers = std::getenv("MCLA_PROFILE_CALLERS") != nullptr;
  const auto ours = OurModules();
  const auto in_ours = [&ours](uint64_t address) {
    for (const auto& [start, end] : ours) {
      if (address >= start && address < end) {
        return true;
      }
    }
    return false;
  };
  size_t count = 0;
  const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
  uint64_t rounds = 0;
  while (GetTickCount64() < end && count + threads.size() <= samples.size()) {
    for (auto& [id, handle] : threads) {
      if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
        continue;
      }
      CONTEXT context;
      context.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(handle, &context)) {
        uint64_t caller = 0;
        if (callers && !in_ours(context.Rip)) {
          // Plain reads of the suspended thread's stack (no locks, no allocation).
          const auto* stack = reinterpret_cast<const uint64_t*>(context.Rsp);
          for (int i = 0; i < 64 && !caller; ++i) {
            caller = in_ours(stack[i]) ? stack[i] : 0;
          }
        }
        samples[count++] = {id, context.Rip, caller};
      }
      ResumeThread(handle);
    }
    ++rounds;
    Sleep(1);
  }
  // MCLA_PROFILE_STACKS=1: each thread's whole stack once at the end (x64 unwind), for hangs.
  static const bool stacks_wanted = std::getenv("MCLA_PROFILE_STACKS") != nullptr;
  constexpr int kStackFrames = 48;
  std::vector<std::pair<uint32_t, std::array<uint64_t, kStackFrames>>> stacks;
  if (stacks_wanted) {
    stacks.resize(threads.size());
    for (size_t t = 0; t < threads.size(); ++t) {
      auto& [id, handle] = threads[t];
      stacks[t].first = id;
      stacks[t].second.fill(0);
      if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
        continue;
      }
      CONTEXT context;
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(handle, &context)) {
        for (int frame = 0; frame < kStackFrames && context.Rip; ++frame) {
          stacks[t].second[frame] = context.Rip;
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
          if (!function) {  // a leaf: the return address is at the stack pointer
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
          } else {
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                             &establisher, nullptr);
          }
        }
      }
      ResumeThread(handle);
    }
  }
  for (auto& [id, handle] : threads) {
    CloseHandle(handle);
  }

  // Aggregate: (thread, location) -> hits. A location is a guest function,
  // or module+offset.
  std::map<std::pair<uint32_t, std::string>, uint32_t> hits;
  char buffer[MAX_PATH + 32];
  for (size_t i = 0; i < count; ++i) {
    const uint64_t pc = samples[i].pc;
    std::string where;
    static const bool raw = std::getenv("MCLA_PROFILE_RAW") != nullptr;  // VR fork: module offsets for a PDB
    if (uint32_t guest = raw ? 0 : MclaGuestFunctionForHostPc(static_cast<uintptr_t>(pc))) {
      std::snprintf(buffer, sizeof buffer, "guest sub_%08X", guest);
      where = buffer;
    } else {
      HMODULE module = nullptr;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(pc), &module) &&
          module) {
        char name[MAX_PATH];
        GetModuleFileNameA(module, name, sizeof name);
        const char* base = std::strrchr(name, '\\');
        std::snprintf(buffer, sizeof buffer, "module %s 0x%llX", base ? base + 1 : name,
                      static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(module)));
      } else {
        std::snprintf(buffer, sizeof buffer, "unknown 0x%llX", static_cast<unsigned long long>(pc));
      }
      where = buffer;
    }
    if (samples[i].caller) {
      HMODULE module = nullptr;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(samples[i].caller), &module) &&
          module) {
        char name[MAX_PATH];
        GetModuleFileNameA(module, name, sizeof name);
        const char* base = std::strrchr(name, '\\');
        std::snprintf(buffer, sizeof buffer, " <- module %s 0x%llX", base ? base + 1 : name,
                      static_cast<unsigned long long>(samples[i].caller - reinterpret_cast<uint64_t>(module)));
        where += buffer;
      }
    }
    ++hits[{samples[i].thread_id, where}];
  }

  if (FILE* f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "# rounds=%llu seconds=%d threads=%zu samples=%zu\n",
                 static_cast<unsigned long long>(rounds), seconds, threads.size(), count);
    for (const auto& [key, n] : hits) {
      std::fprintf(f, "%u\t%u\t%s\n", key.first, n, key.second.c_str());
    }
    for (const auto& [id, frames] : stacks) {
      std::fprintf(f, "# stack %u:", id);
      for (uint64_t pc : frames) {
        if (!pc) {
          break;
        }
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        uint64_t base = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(pc), &module) &&
            module) {
          GetModuleFileNameA(module, name, sizeof name);
          base = reinterpret_cast<uint64_t>(module);
        }
        const char* short_name = std::strrchr(name, '\\');
        std::fprintf(f, " %s+0x%llX", short_name ? short_name + 1 : name,
                     static_cast<unsigned long long>(pc - base));
      }
      std::fprintf(f, "\n");
    }
    std::fclose(f);
  }
  REXLOG_INFO("profile: {} samples over {} rounds written to {}", count, rounds, path);
}

}  // namespace
#endif

void MclaApp::StartProfilerIfRequested() {
#ifdef _WIN32
  std::string path = REXCVAR_GET(mcla_profile);
  if (path.empty()) {
    return;
  }
  std::thread(RunProfile, path, static_cast<int>(REXCVAR_GET(mcla_profile_delay)),
              static_cast<int>(REXCVAR_GET(mcla_profile_seconds)))
      .detach();
#endif
}
