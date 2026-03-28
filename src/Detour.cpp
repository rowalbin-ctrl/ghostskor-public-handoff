#include "Detour.h"
#include "Utils.h"
#include "vendor/minhook/include/MinHook.h"

#include <mutex>

namespace {
std::once_flag g_minHookInitOnce;
bool g_minHookReady = false;

const char *MinHookStatusName(MH_STATUS status) {
  const char *name = MH_StatusToString(status);
  return (name != nullptr && name[0] != '\0') ? name : "MH_UNKNOWN";
}

bool EnsureMinHookInitialized() {
  std::call_once(g_minHookInitOnce, []() {
    const MH_STATUS init = MH_Initialize();
    if (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) {
      g_minHookReady = true;
      char ibuf[128];
      sprintf_s(ibuf, "[Detour] backend=minhook init=%s", MinHookStatusName(init));
      LogToFile(ibuf);
    } else {
      g_minHookReady = false;
      char ibuf[160];
      sprintf_s(ibuf, "[Detour] backend=minhook init failed=%s", MinHookStatusName(init));
      LogToFile(ibuf);
    }
  });

  return g_minHookReady;
}

void LogMinHookFailure(const char *stage, MH_STATUS status, void *target) {
  char hbuf[256];
  sprintf_s(hbuf, "[Detour] MinHook %s failed: %s target=%p", stage,
            MinHookStatusName(status), target);
  LogToFile(hbuf);
}
} // namespace

HookBackend Detour::ActiveBackend() {
  return HookBackend::MinHook;
}

const char *Detour::ActiveBackendName() {
  return "minhook";
}

bool Detour::Install(void *target, void *detour, void **original,
                     int stolenBytes) {
  (void)stolenBytes;

  if (!EnsureMinHookInitialized()) {
    return false;
  }

  MH_STATUS stCreate = MH_CreateHook(target, detour, original);
  if (stCreate == MH_ERROR_ALREADY_CREATED) {
    MH_RemoveHook(target);
    stCreate = MH_CreateHook(target, detour, original);
  }

  if (stCreate != MH_OK) {
    LogMinHookFailure("create", stCreate, target);
    return false;
  }

  const MH_STATUS stEnable = MH_EnableHook(target);
  if (stEnable == MH_OK || stEnable == MH_ERROR_ENABLED) {
    return true;
  }

  LogMinHookFailure("enable", stEnable, target);
  MH_RemoveHook(target);
  return false;
}

bool Detour::Uninstall(void *target, int stolenBytes) {
  (void)stolenBytes;

  if (!EnsureMinHookInitialized()) {
    return false;
  }

  const MH_STATUS stDisable = MH_DisableHook(target);
  if (stDisable != MH_OK && stDisable != MH_ERROR_DISABLED &&
      stDisable != MH_ERROR_NOT_CREATED) {
    LogMinHookFailure("disable", stDisable, target);
    return false;
  }

  const MH_STATUS stRemove = MH_RemoveHook(target);
  if (stRemove == MH_OK || stRemove == MH_ERROR_NOT_CREATED) {
    return true;
  }

  LogMinHookFailure("remove", stRemove, target);
  return false;
}
