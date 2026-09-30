#pragma once
#include <cstdint>

namespace NativeHudContext {
// Only visible inside the current thread's original CG_DrawHudElem call.
// Nesting restores the outer object, including during exception unwinding.
inline thread_local uintptr_t currentElement = 0;
class Scope {
  uintptr_t previous;
public:
  explicit Scope(uintptr_t element) : previous(currentElement) {
    currentElement = element;
  }
  ~Scope() { currentElement = previous; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};
inline uintptr_t Get() { return currentElement; }
}
