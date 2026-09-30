#pragma once
#ifndef GHOSTSKOR_TEXT_PERF
#define GHOSTSKOR_TEXT_PERF 0
#endif
#if GHOSTSKOR_TEXT_PERF
#include <atomic>
#include <cstdint>
#include <windows.h>
namespace TextPerf {
enum Event { SlcCalls, SlcAnalyzed, CfgCalls, CfgAnalyzed, IntroCalls,
             IntroSnapshots, NativeHudCalls, NativeHudOwned, AccessViolations, EventCount };
struct Data {
  uint32_t magic = 0x46505447, version = 1;
  std::atomic<uint64_t> frequency{0}, frames{0}, lastQpc{0}, intervalSum{0};
  std::atomic<uint64_t> events[EventCount]{};
  std::atomic<uint64_t> histogram[101]{}; // 1 ms bins; last bin >=100 ms
};
extern "C" __declspec(dllexport) Data GhostsKorTextPerf;
inline void Hit(Event event) { GhostsKorTextPerf.events[event].fetch_add(1,std::memory_order_relaxed); }
inline LONG CALLBACK ObserveException(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) Hit(AccessViolations);
  return EXCEPTION_CONTINUE_SEARCH;
}
inline void Frame() {
  static bool initialized = false;
  if (!initialized) {
    LARGE_INTEGER hz; QueryPerformanceFrequency(&hz);
    GhostsKorTextPerf.frequency.store(hz.QuadPart);
    AddVectoredExceptionHandler(0,ObserveException);
    initialized=true;
  }
  LARGE_INTEGER now; QueryPerformanceCounter(&now);
  const auto old=GhostsKorTextPerf.lastQpc.exchange(now.QuadPart);
  if (old) {
    uint64_t delta=now.QuadPart-old;
    uint64_t bin=delta*1000/GhostsKorTextPerf.frequency.load();
    if(bin>100) bin=100;
    GhostsKorTextPerf.histogram[bin].fetch_add(1);
    GhostsKorTextPerf.intervalSum.fetch_add(delta);
  }
  GhostsKorTextPerf.frames.fetch_add(1);
}
}
#else
namespace TextPerf {
enum Event { SlcCalls, SlcAnalyzed, CfgCalls, CfgAnalyzed, IntroCalls,
             IntroSnapshots, NativeHudCalls, NativeHudOwned, AccessViolations };
inline void Hit(Event) {}
inline void Frame() {}
}
#endif
