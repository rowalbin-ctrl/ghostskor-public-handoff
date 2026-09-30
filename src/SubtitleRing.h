#pragma once
#include <cstddef>
#include <cstring>

namespace SubtitleRing {
// The caller first verifies that the complete source buffer is readable.
// Validate each restored entry before subtraction/copy; checkpoint data may
// describe a stale offset even when the buffer pointer itself is readable.
inline bool CopyText(char *destination, size_t destinationSize,
                     const char *buffer, size_t bufferSize,
                     size_t offset, size_t length) {
  if (!destination || !buffer || length == 0 || length >= destinationSize ||
      offset >= bufferSize || length > bufferSize) return false;
  const size_t tail = bufferSize - offset;
  const size_t first = length < tail ? length : tail;
  std::memcpy(destination, buffer + offset, first);
  if (first < length) std::memcpy(destination + first, buffer, length - first);
  destination[length] = '\0';
  return true;
}
}
