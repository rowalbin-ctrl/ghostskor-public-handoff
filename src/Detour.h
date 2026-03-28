#pragma once
#include <cstdint>
#include <windows.h>

enum class HookBackend {
  MinHook = 1,
};

// Minimalistic x64 Detour Class
// Uses 14-byte absolute JMP: FF 25 00 00 00 00 [Address]
class Detour {
public:
  // stolenBytes: Number of bytes to copy from target function prologue.
  static bool Install(void *target, void *detour, void **original,
                      int stolenBytes = 16);
  static bool Uninstall(void *target, int stolenBytes = 16);

  // Backward-compatible API surface.
  static bool Create(void *target, void *detour, void **original,
                     int stolenBytes = 16) {
    return Install(target, detour, original, stolenBytes);
  }
  static bool Remove(void *target, int stolenBytes = 16) {
    return Uninstall(target, stolenBytes);
  }

  static HookBackend ActiveBackend();
  static const char *ActiveBackendName();
};
