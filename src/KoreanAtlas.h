#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

// =============================================================================
// Korean Glyph Atlas - Dynamic Metrics System (multi-atlas)
// =============================================================================

namespace KoreanAtlas {

// Glyph metrics matching our Python generator output
struct GlyphInfo {
  uint32_t codepoint;
  uint16_t u0, v0, u1, v1; // Normalized UV (0-65535)
  int16_t xOffset;         // Bearing X
  int16_t yOffset;         // Bearing Y (rendering offset from baseline)
  uint16_t advance;        // Horizontal Advance
};

constexpr uint8_t kAtlasSlotBase = 0;
constexpr uint8_t kAtlasSlotHeader = 1;
constexpr uint8_t kAtlasSlotCount = 2;

// Constants
constexpr int GLYPH_SIZE_BASE = 42; // Match Python script config

// Load metrics from binary file into a specific atlas slot
bool LoadMetrics(const std::string &filename, uint8_t slot);

// Load metrics from memory buffer into a specific atlas slot
bool LoadMetricsFromMemory(const void *data, size_t dataSize, uint8_t slot);

// Legacy wrapper: base atlas slot (slot 0)
inline bool LoadMetrics(const std::string &filename) {
  return LoadMetrics(filename, kAtlasSlotBase);
}

// Get glyph info (O(1) lookup)
const GlyphInfo *GetGlyph(uint32_t codepoint, uint8_t slot);

// Legacy wrapper: base atlas slot (slot 0)
inline const GlyphInfo *GetGlyph(uint32_t codepoint) {
  return GetGlyph(codepoint, kAtlasSlotBase);
}

int GetAtlasWidth(uint8_t slot);
int GetAtlasHeight(uint8_t slot);
size_t GetGlyphCount(uint8_t slot);

// UTF-8 Decoder
int DecodeUTF8(const char *str, uint32_t &outCodepoint);

} // namespace KoreanAtlas
