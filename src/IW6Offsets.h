#pragma once
#include "GameAddressProfile.h"
#include "GameAddresses.h"

namespace IW6Offsets {
inline void* GetAddress(uintptr_t moduleBase, uintptr_t referenceRva) {
  return GameAddresses::Resolve(moduleBase, referenceRva);
}
} // namespace IW6Offsets
