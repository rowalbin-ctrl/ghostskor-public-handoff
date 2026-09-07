#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace AddressPatternMatcher {
struct Pattern {
  const char* name;
  uintptr_t referenceRva;
  size_t size;
  size_t anchorOffset;
  size_t anchorSize;
  ptrdiff_t relativeDispOffset; // -1: match start; otherwise decode a rel32 target
  const char* bytes;
  const char* mask;
};
struct Match {
  uintptr_t rva = 0;
  unsigned count = 0; // Saturates at two: ambiguity is a failure.
};
inline bool Valid(const Pattern& p) {
  return p.name && p.bytes && p.mask && p.size > 0 && p.anchorSize > 0 &&
         p.anchorOffset < p.size && p.anchorSize <= p.size - p.anchorOffset &&
         (p.relativeDispOffset == -1 || (p.relativeDispOffset >= 0 && p.size >= 4 &&
          static_cast<size_t>(p.relativeDispOffset) <= p.size - 4));
}
inline void Search(const unsigned char* data, size_t size, uintptr_t sectionRva,
                   const Pattern& p, Match& result) {
  if (!data || !Valid(p) || size < p.size || result.count > 1) return;
  size_t position = 0;
  const size_t last = size - p.size;
  while (position <= last) {
    const auto* hit = static_cast<const unsigned char*>(std::memchr(
        data + position + p.anchorOffset,
        static_cast<unsigned char>(p.bytes[p.anchorOffset]), last - position + 1));
    if (!hit) return;
    position = static_cast<size_t>(hit - data) - p.anchorOffset;
    const auto* candidate = data + position;
    if (std::memcmp(candidate + p.anchorOffset, p.bytes + p.anchorOffset,
                    p.anchorSize) == 0) {
      bool matches = true;
      for (size_t i = 0; i < p.size; ++i) {
        if (p.mask[i] != '?' && candidate[i] != static_cast<unsigned char>(p.bytes[i])) {
          matches = false;
          break;
        }
      }
      if (matches) {
        result.rva = sectionRva + position;
        if (p.relativeDispOffset >= 0) {
          int32_t displacement = 0;
          std::memcpy(&displacement, candidate + p.relativeDispOffset, sizeof(displacement));
          const int64_t target = static_cast<int64_t>(result.rva) +
                                p.relativeDispOffset + 4 + displacement;
          if (target < 0) return;
          result.rva = static_cast<uintptr_t>(target);
        }
        if (++result.count > 1) return;
      }
    }
    ++position;
  }
}
}
