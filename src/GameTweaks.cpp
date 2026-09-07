#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include "DynamicLightPolicy.h"
#include "GameBuild.h"
#include "GameTweaks.h"
#include "IW6Offsets.h"

namespace {
std::atomic_flag g_pollBusy = ATOMIC_FLAG_INIT;
DynamicLightPolicy g_policy;

bool ReadDynamicLightLimit(int& current) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  using FindVar = const unsigned char* (*)(const char*);
  const auto findVar = reinterpret_cast<FindVar>(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(base, IW6Offsets::Dvar_FindVar_SP)));
  __try {
    const unsigned char* var = findVar("r_dlightLimit");
    // Registration at 0x5587D6 uses Dvar_RegisterInt (type 5, domain 0..4).
    // GetInt at 0x4708B0 confirms the type/current offsets on this build.
    if (!var || *reinterpret_cast<const volatile unsigned char*>(var + 0x0C) != 5)
      return false;
    current = *reinterpret_cast<const volatile int*>(var + 0x10);
    return current >= 0 && current <= 4;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool QueueDynamicLightOverride() {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  using AddText = void (*)(int, const char*);
  const auto addText = reinterpret_cast<AddText>(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(base, IW6Offsets::Cbuf_AddText_SP)));
  __try {
    // Cbuf_AddText owns critical section 0x22; the main game command loop
    // performs the actual dvar update. "seta" lets normal game saving persist
    // this one archived setting, even when players2 did not exist at startup.
    addText(0, "seta r_dlightLimit 4\n");
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void WriteLightingStatus(const char* status, bool haveValue, int current) {
  wchar_t path[32768] = {};
  constexpr size_t capacity = sizeof(path) / sizeof(path[0]);
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(capacity));
  if (!length || length >= capacity) return;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash || wcscpy_s(slash + 1, capacity - (slash + 1 - path),
                         L"ghostskor_lighting.txt") != 0) return;
  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  char report[768] = {};
  const int size = sprintf_s(report,
      "GhostsKor R5 dynamic lighting override\r\n"
      "Required r_dlightLimit: 4\r\nStatus: %s\r\n"
      "Registered integer: %s\r\nObserved value: %d\r\n"
      "Attempts in unconfirmed episode: %u / %u (including first submission)\r\n"
      "Queue exception stop until game restart: %s\r\n"
      "Only this setting is queued through the game; no config file is replaced.\r\n",
      status, haveValue ? "yes" : "no", haveValue ? current : -1,
      g_policy.Attempts(), DynamicLightPolicy::MaxAttempts,
      g_policy.QueueFaulted() ? "yes" : "no");
  DWORD written = 0;
  if (size > 0) WriteFile(file, report, static_cast<DWORD>(size), &written, nullptr);
  CloseHandle(file);
}
}

void GameTweaks::PollDynamicLights() {
  if (!GameBuild::RuntimeReady() || g_pollBusy.test_and_set(std::memory_order_acquire))
    return;
  struct PollGuard {
    ~PollGuard() { g_pollBusy.clear(std::memory_order_release); }
  } guard;
  const uint64_t now = GetTickCount64();
  if (!g_policy.BeginPoll(now)) return;

  int current = 0;
  const bool haveValue = ReadDynamicLightLimit(current);
  const char* status = "Waiting for registered integer setting";
  if (haveValue && current == DynamicLightPolicy::RequiredValue) {
    g_policy.ShouldQueue(now, true, current);
    status = "Verified r_dlightLimit = 4";
  } else if (g_policy.ShouldQueue(now, haveValue, current)) {
    if (QueueDynamicLightOverride()) {
      status = "Queued r_dlightLimit = 4; awaiting game confirmation";
    } else {
      // SEH cannot prove the engine remained consistent after an exception.
      // Do not re-enter the queue on later frames of this game process.
      g_policy.StopAfterQueueFault();
      status = "Stopped after queue exception; no more submissions this run";
    }
  } else if (g_policy.QueueFaulted()) {
    status = "Stopped after queue exception; no more submissions this run";
  } else if (g_policy.RetryLimitReached(now)) {
    status = "Stopped after 3 unconfirmed submissions; still observing value";
  } else if (haveValue) {
    status = "Waiting for game to apply r_dlightLimit = 4";
  }

  static const char* lastStatus = nullptr;
  static int lastValue = -2;
  static unsigned lastAttempts = 0;
  const int observed = haveValue ? current : -1;
  if (status != lastStatus || observed != lastValue ||
      g_policy.Attempts() != lastAttempts) {
    WriteLightingStatus(status, haveValue, current);
    lastStatus = status;
    lastValue = observed;
    lastAttempts = g_policy.Attempts();
  }
}
