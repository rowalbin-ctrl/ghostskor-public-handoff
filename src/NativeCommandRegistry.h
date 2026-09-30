#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <utility>

// Caller synchronizes producer/backend access. IDs are never reused when the
// game's command buffers recycle. Keep recent batches for legitimate replay;
// discarded engine frames cannot accumulate translations indefinitely.
template<class Batch> class NativeCommandRegistry {
  struct Entry { uint64_t frame; std::shared_ptr<const Batch> batch; };
  std::unordered_map<uint64_t, Entry> entries;
  uint64_t next = 0, frame = 0;
public:
  void BeginFrame() {
    ++frame;
    for (auto it = entries.begin(); it != entries.end();)
      if (frame - it->second.frame > 8) it = entries.erase(it); else ++it;
  }
  uint64_t Add(Batch batch) {
    const auto id = ++next;
    entries.emplace(id, Entry{frame, std::make_shared<const Batch>(std::move(batch))});
    return id;
  }
  std::shared_ptr<const Batch> Find(uint64_t id) const {
    const auto it = entries.find(id);
    return it == entries.end() ? nullptr : it->second.batch;
  }
  static void WriteMarker(char *out, size_t capacity, uint64_t id) {
    snprintf(out, capacity, "\x1eGK%016llX", (unsigned long long)id);
  }
  static bool ReadMarker(const char *text, size_t capacity, uint64_t &id) {
    if (capacity < 20 || text[0] != '\x1e' || text[1] != 'G' || text[2] != 'K' || text[19] != 0) return false;
    uint64_t value = 0;
    for (size_t i = 3; i < 19; ++i) {
      const unsigned char c = text[i];
      const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
          c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
      if (digit >= 16) return false;
      value = (value << 4) | digit;
    }
    id = value;
    return true;
  }
};
