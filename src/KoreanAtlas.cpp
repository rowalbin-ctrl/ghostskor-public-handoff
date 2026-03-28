#include "KoreanAtlas.h"
#include <fstream>
#include <iostream>
#include <vector>

namespace KoreanAtlas {

namespace {
std::unordered_map<uint32_t, GlyphInfo> g_metricsSlots[kAtlasSlotCount];
int g_atlasWidthSlots[kAtlasSlotCount] = {4096, 4096};
int g_atlasHeightSlots[kAtlasSlotCount] = {4096, 4096};
} // namespace

bool LoadMetrics(const std::string &filename, uint8_t slot) {
  if (slot >= kAtlasSlotCount) {
    std::cerr << "[KoreanAtlas] Invalid slot: " << (int)slot << std::endl;
    return false;
  }

  std::ifstream file(filename, std::ios::binary);
  if (!file.is_open()) {
    std::cerr << "[KoreanAtlas] Failed to open metrics file (slot "
              << (int)slot << "): " << filename << std::endl;
    return false;
  }

  uint32_t count = 0;
  file.read(reinterpret_cast<char *>(&count), sizeof(count));

  if (count == 0) {
    std::cerr << "[KoreanAtlas] Metrics file is empty or invalid (slot "
              << (int)slot << ")." << std::endl;
    return false;
  }

  auto &metrics = g_metricsSlots[slot];
  metrics.clear();
  metrics.reserve(count);

  // File Format:
  // uint32_t codepoint
  // uint16_t u0, v0, u1, v1
  // int16_t xOffset, yOffset
  // uint16_t advance
  // Total 18 bytes per glyph (packed tightly in python)
#pragma pack(push, 1)
  struct PackedMetric {
    uint32_t codepoint;
    uint16_t u0, v0, u1, v1;
    int16_t xOffset, yOffset;
    uint16_t advance;
  };
#pragma pack(pop)

  std::vector<PackedMetric> buffer(count);
  file.read(reinterpret_cast<char *>(buffer.data()),
            count * sizeof(PackedMetric));

  if (!file) {
    std::cerr << "[KoreanAtlas] Error reading metrics data (slot "
              << (int)slot << ")." << std::endl;
    return false;
  }

  for (const auto &pm : buffer) {
    GlyphInfo info;
    info.codepoint = pm.codepoint;
    info.u0 = pm.u0;
    info.v0 = pm.v0;
    info.u1 = pm.u1;
    info.v1 = pm.v1;
    info.xOffset = pm.xOffset;
    info.yOffset = pm.yOffset;
    info.advance = pm.advance;
    metrics[pm.codepoint] = info;
  }

  std::cout << "[KoreanAtlas] Loaded " << count << " glyph metrics (slot "
            << (int)slot << ")." << std::endl;
  return true;
}

bool LoadMetricsFromMemory(const void *data, size_t dataSize, uint8_t slot) {
  if (slot >= kAtlasSlotCount) {
    std::cerr << "[KoreanAtlas] Invalid slot: " << (int)slot << std::endl;
    return false;
  }

  if (!data || dataSize < sizeof(uint32_t)) {
    std::cerr << "[KoreanAtlas] Embedded metrics data is null or too small (slot "
              << (int)slot << ")." << std::endl;
    return false;
  }

  const uint8_t *ptr = static_cast<const uint8_t *>(data);
  const uint8_t *end = ptr + dataSize;

  uint32_t count = 0;
  memcpy(&count, ptr, sizeof(count));
  ptr += sizeof(count);

  if (count == 0) {
    std::cerr << "[KoreanAtlas] Embedded metrics is empty or invalid (slot "
              << (int)slot << ")." << std::endl;
    return false;
  }

#pragma pack(push, 1)
  struct PackedMetric {
    uint32_t codepoint;
    uint16_t u0, v0, u1, v1;
    int16_t xOffset, yOffset;
    uint16_t advance;
  };
#pragma pack(pop)

  size_t needed = (size_t)count * sizeof(PackedMetric);
  if (ptr + needed > end) {
    std::cerr << "[KoreanAtlas] Embedded metrics data truncated (slot "
              << (int)slot << ")." << std::endl;
    return false;
  }

  auto &metrics = g_metricsSlots[slot];
  metrics.clear();
  metrics.reserve(count);

  const PackedMetric *buffer = reinterpret_cast<const PackedMetric *>(ptr);
  for (uint32_t i = 0; i < count; ++i) {
    const auto &pm = buffer[i];
    GlyphInfo info;
    info.codepoint = pm.codepoint;
    info.u0 = pm.u0;
    info.v0 = pm.v0;
    info.u1 = pm.u1;
    info.v1 = pm.v1;
    info.xOffset = pm.xOffset;
    info.yOffset = pm.yOffset;
    info.advance = pm.advance;
    metrics[pm.codepoint] = info;
  }

  std::cout << "[KoreanAtlas] Loaded " << count
            << " glyph metrics from embedded resource (slot " << (int)slot
            << ")." << std::endl;
  return true;
}

const GlyphInfo *GetGlyph(uint32_t codepoint, uint8_t slot) {
  if (slot >= kAtlasSlotCount) {
    return nullptr;
  }
  auto &metrics = g_metricsSlots[slot];
  auto it = metrics.find(codepoint);
  if (it != metrics.end()) {
    return &it->second;
  }
  return nullptr;
}

int GetAtlasWidth(uint8_t slot) {
  if (slot >= kAtlasSlotCount) {
    return g_atlasWidthSlots[kAtlasSlotBase];
  }
  return g_atlasWidthSlots[slot];
}

int GetAtlasHeight(uint8_t slot) {
  if (slot >= kAtlasSlotCount) {
    return g_atlasHeightSlots[kAtlasSlotBase];
  }
  return g_atlasHeightSlots[slot];
}

size_t GetGlyphCount(uint8_t slot) {
  if (slot >= kAtlasSlotCount) {
    return 0;
  }
  return g_metricsSlots[slot].size();
}

int DecodeUTF8(const char *str, uint32_t &outCodepoint) {
  if (!str || !*str) {
    outCodepoint = 0;
    return 0;
  }

  unsigned char c = static_cast<unsigned char>(*str);

  // ASCII (0xxxxxxx)
  if ((c & 0x80) == 0) {
    outCodepoint = c;
    return 1;
  }

  // 2-byte (110xxxxx 10xxxxxx)
  if ((c & 0xE0) == 0xC0) {
    outCodepoint = (c & 0x1F) << 6;
    outCodepoint |= (static_cast<unsigned char>(str[1]) & 0x3F);
    return 2;
  }

  // 3-byte (1110xxxx 10xxxxxx 10xxxxxx) - Korean is here!
  if ((c & 0xF0) == 0xE0) {
    outCodepoint = (c & 0x0F) << 12;
    outCodepoint |= (static_cast<unsigned char>(str[1]) & 0x3F) << 6;
    outCodepoint |= (static_cast<unsigned char>(str[2]) & 0x3F);
    return 3;
  }

  // 4-byte (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
  if ((c & 0xF8) == 0xF0) {
    outCodepoint = (c & 0x07) << 18;
    outCodepoint |= (static_cast<unsigned char>(str[1]) & 0x3F) << 12;
    outCodepoint |= (static_cast<unsigned char>(str[2]) & 0x3F) << 6;
    outCodepoint |= (static_cast<unsigned char>(str[3]) & 0x3F);
    return 4;
  }

  // Invalid
  outCodepoint = 0xFFFD; // Replacement character
  return 1;
}

} // namespace KoreanAtlas
