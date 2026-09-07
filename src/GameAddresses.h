#pragma once
#include <cstdint>

namespace GameAddresses {
// Called once under the GameBuild initialization lock, before installing hooks.
// The known build gate remains: caller/data layouts still require a profile.
bool Initialize(uintptr_t moduleBase);
// No unchecked base+RVA fallback. Null before publication or for unknown IDs.
void* Resolve(uintptr_t moduleBase, uintptr_t referenceRva);
const char* Failure();
}
