#include "NativeGamepad.h"
#include "GameBuild.h"
#include "IW6Offsets.h"
#include <cstring>
#include <windows.h>

namespace {
bool CopyState(uintptr_t base, bool* enabled, NativeGamepad::BindingSnapshot* snapshot) {
  __try {
    const auto controller = *reinterpret_cast<volatile int*>(base + IW6Offsets::PadControllerIndex_SP);
    if (controller < 0 || controller >= 8) return false;
    const auto profile = base + IW6Offsets::PadProfiles_SP + controller * 0x1130;
    const auto mode = *reinterpret_cast<volatile unsigned char*>(profile + 0x2bc);
    if (mode > 1) return false;
    *enabled = mode != 0;
    if (!snapshot) return true;
    // Native SP takes localClientNum 0. Controller index selects the profile,
    // not the client key array. Entries are three 32-bit fields, binding at +8
    // relative to the key's down/repeats fields (this RVA points to binding).
    const auto keys = base + IW6Offsets::PadKeyBindings_SP;
    const auto names = reinterpret_cast<const char* const*>(base + IW6Offsets::PadCommandNames_SP);
    for (size_t i = 0; i < snapshot->names.size(); ++i) {
      size_t n = 0;
      const char* source = names[i];
      if (!source) return false;
      for (; n < snapshot->names[i].size() - 1 && source[n]; ++n)
        snapshot->names[i][n] = source[n];
      if (source[n]) return false;
      snapshot->names[i][n] = 0;
    }
    for (size_t i = 0; i < snapshot->commands.size(); ++i)
      snapshot->commands[i] = *reinterpret_cast<volatile int32_t*>(keys + i * 12);
    // Layout changes occur on the game thread. Reject a torn read and retry
    // on the next request rather than caching an intermediate layout.
    for (size_t i = 0; i < snapshot->commands.size(); ++i)
      if (snapshot->commands[i] != *reinterpret_cast<volatile int32_t*>(keys + i * 12)) return false;
    return controller == *reinterpret_cast<volatile int*>(base + IW6Offsets::PadControllerIndex_SP) &&
           mode == *reinterpret_cast<volatile unsigned char*>(profile + 0x2bc);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
bool NativeGamepad::ReadEnabled(bool& enabled) {
  if (!GameBuild::RuntimeReady()) return false;
  return CopyState(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), &enabled, nullptr);
}
bool NativeGamepad::ReadBindings(BindingSnapshot& snapshot) {
  if (!GameBuild::RuntimeReady()) return false;
  bool enabled = false;
  return CopyState(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), &enabled, &snapshot) && enabled;
}
