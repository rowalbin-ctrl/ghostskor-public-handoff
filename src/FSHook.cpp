// =============================================================================
// FSHook.cpp - Intercept DB_FindXAssetHeader for:
//   1. subtitles.csv (STRINGTABLE) - Extract video subtitles
//   2. LOCALIZE - Register original content without changing asset values
// =============================================================================

#include "FSHook.h"
#include "BinkHook.h"
#include "Detour.h"
#include "IW6Offsets.h"
#include "TextHook.h"
#include "Utils.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace FSHook {

// Asset Types (IW6)
static const int ASSET_TYPE_STRINGTABLE = 0x2E; // 46
static const int ASSET_TYPE_LOCALIZE = 0x21;    // 33 - Localization strings

// StringTable Structures (iw6x-client)
struct StringTableCell {
  const char *string;
  int hash;
};

struct StringTable {
  const char *name;
  int columnCount;
  int rowCount;
  StringTableCell *values;
};

// LocalizeEntry structure (iw6x-client)
struct LocalizeEntry {
  const char *value;
  const char *name;
};

// Function pointer
typedef void *(*DB_FindXAssetHeader_t)(int type, const char *name,
                                       int allowCreateDefault);
static DB_FindXAssetHeader_t g_Original_DB_FindXAssetHeader = nullptr;

// Empty table to return
static StringTable g_EmptyStringTable = {0};

// State guard
static bool g_SubtitlesLoaded = false;

// English -> Korean translation map
static std::unordered_map<std::string, std::string> g_EnglishToKorean;

// Case-insensitive substring
static const char *StrIStr(const char *haystack, const char *needle) {
  if (!haystack || !needle)
    return nullptr;
  size_t needleLen = strlen(needle);
  for (; *haystack; ++haystack) {
    if (_strnicmp(haystack, needle, needleLen) == 0) {
      return haystack;
    }
  }
  return nullptr;
}

// SEH-safe string read (SEH isolated to avoid C2712)
static int SEH_ReadString(const char *str, char *buffer, size_t bufSize) {
  if (!str || !buffer || bufSize == 0)
    return -1;

  __try {
    size_t i = 0;
    for (; i < bufSize - 1; ++i) {
      buffer[i] = str[i];
      if (str[i] == '\0')
        break;
    }
    buffer[i] = '\0';
    return (int)i;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    buffer[0] = '\0';
    return -1;
  }
}

static std::string SafeReadString(const char *str, size_t maxLen = 2048) {
  if (!str)
    return std::string();

  if (maxLen > 4095)
    maxLen = 4095;

  char buffer[4096] = {0};
  int len = SEH_ReadString(str, buffer, maxLen + 1);
  if (len < 0)
    return std::string();

  return std::string(buffer);
}

static std::string ToLowerCopy(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)tolower(c); });
  return out;
}

static bool ParseDouble(const std::string &text, double &outVal) {
  try {
    size_t idx = 0;
    outVal = std::stod(text, &idx);
    return idx > 0;
  } catch (...) {
    return false;
  }
}

static std::string ToUpperCopy(const std::string &s) {
  std::string out=s;
  std::transform(out.begin(),out.end(),out.begin(),[](unsigned char c){return (char)toupper(c);});
  return out;
}

static std::string ReadLocalizeValue(void *header, const char *assetName) {
  if (!header || !assetName) {
    return std::string();
  }

  LocalizeEntry *entry = (LocalizeEntry *)header;
  std::string a = SafeReadString(entry->value, 512);
  std::string b = SafeReadString(entry->name, 512);

  if (!a.empty() && _stricmp(a.c_str(), assetName) == 0) {
    return TrimAscii(b);
  }
  if (!b.empty() && _stricmp(b.c_str(), assetName) == 0) {
    return TrimAscii(a);
  }

  // Fallback: return the field that doesn't look like an uppercase key name.
  auto looksLikeKey = [](const std::string &v) -> bool {
    if (v.empty() || v.size() > 96) {
      return false;
    }
    bool hasUnderscore = false;
    for (char c : v) {
      if (c == '_') {
        hasUnderscore = true;
        continue;
      }
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        continue;
      }
      return false;
    }
    return hasUnderscore;
  };

  if (!a.empty() && !looksLikeKey(a)) {
    return TrimAscii(a);
  }
  if (!b.empty() && !looksLikeKey(b)) {
    return TrimAscii(b);
  }
  if (!a.empty()) {
    return TrimAscii(a);
  }
  return TrimAscii(b);
}

// Extract video subtitles from the original table.
static void ExtractSubtitlesFromTable(StringTable *table) {
  if (!table || table->rowCount <= 0 || table->columnCount <= 0 ||
      !table->values) {
    return;
  }

  const int cols = table->columnCount;
  const int rows = table->rowCount;

  std::vector<BinkHook::VideoSubtitle> entries;
  entries.reserve(rows);

  int koreanMatched = 0;

  for (int r = 0; r < rows; ++r) {
    const StringTableCell *row = table->values + (r * cols);

    std::string videoName = SafeReadString(row[0].string);
    if (videoName.empty())
      continue;

    // Skip header row
    if (r == 0) {
      std::string headerLower = ToLowerCopy(videoName);
      if (headerLower == "videoname") {
        continue;
      }
    }

    std::string startStr = (cols > 1) ? SafeReadString(row[1].string) : "";
    std::string endStr = (cols > 2) ? SafeReadString(row[2].string) : "";
    std::string textStr = (cols > 3) ? SafeReadString(row[3].string) : "";

    if (startStr.empty() || endStr.empty() || textStr.empty())
      continue;

    double startTime = 0.0;
    double endTime = 0.0;
    if (!ParseDouble(startStr, startTime) || !ParseDouble(endStr, endTime))
      continue;

    std::string videoLower = ToLowerCopy(videoName);

    BinkHook::VideoSubtitle sub;
    sub.videoName = videoLower;
    sub.startTime = startTime;
    sub.endTime = endTime;
    sub.englishText = textStr;
    sub.koreanText = "";

    // ENGLISH TEXT MATCHING: Normalize English and lookup Korean directly
    std::string normalized = ToUpperCopy(textStr);
    // Strip color codes for matching (^0-^9, ^a-^z, ^A-^Z)
    std::string stripped;
    for (size_t i = 0; i < normalized.size(); ++i) {
      if (normalized[i] == '^' && i + 1 < normalized.size()) {
        char next = normalized[i + 1];
        if ((next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') ||
            (next >= 'A' && next <= 'Z')) {
          ++i;
          continue;
        }
      }
      stripped += normalized[i];
    }

    auto it = g_EnglishToKorean.find(stripped);
    if (it != g_EnglishToKorean.end()) {
      sub.koreanText = it->second;
      koreanMatched++;
    } else {
      // Log unmatched for debugging (first 80 chars)
      std::string preview = Utf8Preview(stripped, 80);
      LogToFile("[FSHook] UNMATCHED: " + preview);
    }

    entries.push_back(sub);
  }

  if (!entries.empty()) {
    BinkHook::LoadSubtitlesFromEntries(entries);
    g_SubtitlesLoaded = true;
    LogToFile("[FSHook] Extracted " + std::to_string(entries.size()) +
              " subtitles, " + std::to_string(koreanMatched) +
              " Korean matched");
  }
}

// =============================================================================
// Hooked DB_FindXAssetHeader
// =============================================================================
static void *Hooked_DB_FindXAssetHeader(int type, const char *name,
                                        int allowCreateDefault) {
  // =========================================================================
  // 1. Handle STRINGTABLE (subtitles.csv) - extract and suppress native subs
  // =========================================================================
  if (type == ASSET_TYPE_STRINGTABLE && name &&
      StrIStr(name, "subtitles.csv")) {

    void *originalResult =
        g_Original_DB_FindXAssetHeader(type, name, allowCreateDefault);

    if (originalResult) {
      StringTable *origTable = (StringTable *)originalResult;

      if (!g_SubtitlesLoaded) {
        ExtractSubtitlesFromTable(origTable);
      }

      g_EmptyStringTable.name = origTable->name;
      g_EmptyStringTable.columnCount = origTable->columnCount;
      g_EmptyStringTable.rowCount = 0; // Suppress native subtitles
      g_EmptyStringTable.values = nullptr;

      return &g_EmptyStringTable;
    }

    return originalResult;
  }

  // A localization lookup is content access, not a display event. In particular,
  // do not blank intro assets, prewarm objectives, enqueue status messages or
  // infer visibility from mission namespaces here. The final renderer owns it.
  if (type == ASSET_TYPE_LOCALIZE && name) {
    void *result=g_Original_DB_FindXAssetHeader(type,name,allowCreateDefault);
    if (result) {
      const auto english=ReadLocalizeValue(result,name);
      TextHook_RegisterRuntimeEnglishKey(name,english.c_str());
    }
    return result;
  }

  return g_Original_DB_FindXAssetHeader(type, name, allowCreateDefault);
}

// =============================================================================
// Initialize - Install DB_FindXAssetHeader hook
// =============================================================================
bool Initialize() {
  LogToFile("[FSHook] Initializing DB_FindXAssetHeader hook...");

  uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  if (!moduleBase) {
    LogToFile("[FSHook] ERROR: module base not found");
    return false;
  }

  // Detect SP/MP by exe name
  std::string exeName = GetModulePathUtf8(NULL);
  bool isMP = (StrIStr(exeName.c_str(), "iw6mp") != nullptr);

  uintptr_t offset = isMP ? IW6Offsets::DB_FindXAssetHeader_MP
                          : IW6Offsets::DB_FindXAssetHeader_SP;

  void *targetAddress = IW6Offsets::GetAddress(moduleBase, offset);
  if (!targetAddress) {
    LogToFile("[FSHook] ERROR: DB_FindXAssetHeader target not found");
    return false;
  }

  void *originalFunc = nullptr;
  if (CreateHook(targetAddress, (void *)Hooked_DB_FindXAssetHeader,
                     &originalFunc, 15)) {
    g_Original_DB_FindXAssetHeader = (DB_FindXAssetHeader_t)originalFunc;
    LogToFile("[FSHook] Hook installed successfully!");
    return true;
  }

  LogToFile("[FSHook] ERROR: Detour failed");
  return false;
}

void SetEnglishToKoreanMap(
    const std::unordered_map<std::string, std::string> &map) {
  g_EnglishToKorean = map;
  LogToFile("[FSHook] English->Korean map set with " +
            std::to_string(map.size()) + " entries");
}

} // namespace FSHook





