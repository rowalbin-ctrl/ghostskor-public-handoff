// =============================================================================
// FSHook.cpp - Intercept DB_FindXAssetHeader for:
//   1. subtitles.csv (STRINGTABLE) - Extract video subtitles
//   2. INTROSCREEN (LOCALIZE) - Suppress English + queue Korean overlay
// =============================================================================

#include "FSHook.h"
#include "BinkHook.h"
#include "Detour.h"
#include "IW6Offsets.h"
#include "TextHook.h"
#include "HudClassify.h"
#include "ObjectiveRendererUnified.h"
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

// Blank string for suppressing localized entries
static const char *kIntroBlank = " ";

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
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return (char)toupper(c); });
  return out;
}

static bool ShouldTreatAsGameplayHudKey(const char *name) {
  if (!name) {
    return false;
  }

  const HudKeyType t = ClassifyHudKey(std::string(name));
  if (t == HUD_KEY_EXCLUDE || t == HUD_KEY_MENU || t == HUD_KEY_INTRO ||
      t == HUD_KEY_SUBTITLE || t == HUD_KEY_UNKNOWN) {
    return false;
  }
  return true;
}
// Hints/prompts should be driven by runtime draw/SLC lifecycle, not by
// DB_FindXAssetHeader lookups (which can fire outside active rendering).
static bool IsRuntimeManagedHintKey(const char *name) {
  if (!name)
    return false;

  if (strncmp(name, "PLATFORM_", 9) == 0)
    return true;
  if (strncmp(name, "SCRIPT_HINT_", 12) == 0)
    return true;
  if (strncmp(name, "SCRIPT_PLATFORM_HINT_", 21) == 0)
    return true;
  if (strncmp(name, "HINT_", 5) == 0)
    return true;
  if (strstr(name, "_HINT") != nullptr)
    return true;

  if (strncmp(name, "CORNERED_", 9) == 0) {
    // Objective list/header keys remain in DB path (non-hint top-left track).
    if (strncmp(name, "CORNERED_OBJ_", 13) == 0 ||
        strncmp(name, "CORNERED_OBJECTIVE_", 19) == 0) {
      return false;
    }
    return true;
  }

  return false;
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

static std::string ExtractObjectiveNamespace(const char *name) {
  if (!name || !name[0]) {
    return std::string();
  }
  if (strncmp(name, "GAME_", 5) == 0 ||
      strncmp(name, "CGAME_", 6) == 0 ||
      strncmp(name, "EXE_", 4) == 0 ||
      strncmp(name, "MENU_", 5) == 0 ||
      strncmp(name, "LUA_", 4) == 0 ||
      strncmp(name, "PLATFORM_", 9) == 0 ||
      strncmp(name, "HINT_", 5) == 0 ||
      strncmp(name, "SCRIPT_", 7) == 0) {
    return std::string();
  }

  const std::string key(name);
  const size_t first = key.find('_');
  if (first == std::string::npos) {
    return key;
  }
  const size_t second = key.find('_', first + 1);
  if (second != std::string::npos && first <= 5) {
    return key.substr(0, second);
  }
  return key.substr(0, first);
}

static bool ShouldAcceptForceGameplayByNamespace(const char *name, DWORD now) {
  const std::string candidateNs = ExtractObjectiveNamespace(name);
  if (candidateNs.empty()) {
    // Force-prewarm must stay mission-objective scoped.
    // Empty namespace corresponds to global/UI/LUA-style keys.
    return false;
  }

  std::vector<ObjectiveOverlayEntry> active = TextHook_GetObjectiveEntries();
  std::unordered_set<std::string> activeNs;
  activeNs.reserve(8);
  for (const auto &e : active) {
    if (e.key.empty()) {
      continue;
    }
    if (e.channel != OBJ_CHANNEL_GAMEPLAY_LIST &&
        e.channel != OBJ_CHANNEL_GAMEPLAY_UPDATE) {
      continue;
    }
    unsigned long age = (now >= e.lastSeen) ? (now - e.lastSeen) : 0;
    if (age > 12000) {
      continue;
    }
    const std::string ns = ExtractObjectiveNamespace(e.key.c_str());
    if (!ns.empty()) {
      activeNs.insert(ns);
    }
  }

  if (activeNs.empty()) {
    return true;
  }
  return activeNs.find(candidateNs) != activeNs.end();
}

static bool IsObjectiveLoadingPhaseLikely() {
  const DWORD now = GetTickCount();
  const unsigned long nativeTick = TextHook_GetNativeObjectiveItemsTick();
  if (nativeTick == 0) {
    return true;
  }
  if (now < nativeTick) {
    return true;
  }
  return (now - nativeTick) > 1600;
}

// =============================================================================
// SuppressIntroLocalize - Blank the English value in a LocalizeEntry
// so the game's typewriter renderer shows nothing (Korean overlay replaces it)
// =============================================================================
static void SuppressIntroLocalize(void *header, const char *assetName) {
  if (!header || !assetName)
    return;

  LocalizeEntry *entry = (LocalizeEntry *)header;

  // Detect field order by matching assetName against both fields
  std::string nameA = SafeReadString(entry->value, 256);
  std::string nameB = SafeReadString(entry->name, 256);

  const char **valuePtr = nullptr;

  if (!nameA.empty() && _stricmp(nameA.c_str(), assetName) == 0) {
    // entry->value contains the key name -> entry->name is the actual value
    valuePtr = &entry->name;
  } else if (!nameB.empty() && _stricmp(nameB.c_str(), assetName) == 0) {
    // entry->name contains the key name -> entry->value is the actual value
    valuePtr = &entry->value;
  } else {
    // Neither field matches - try the default (value is the text to blank)
    valuePtr = &entry->value;
  }

  if (!valuePtr)
    return;

  // Patch the value pointer to blank string
  DWORD oldProtect = 0;
  if (VirtualProtect((void *)valuePtr, sizeof(const char *),
                     PAGE_READWRITE, &oldProtect)) {
    *valuePtr = kIntroBlank;
    VirtualProtect((void *)valuePtr, sizeof(const char *), oldProtect,
                   &oldProtect);
  }
}

// =============================================================================
// Extract subtitles from StringTable (subtitles.csv)
// =============================================================================
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

  // =========================================================================
  // 2. Handle intro-overlay LOCALIZE keys - suppress English, queue Korean overlay
  // =========================================================================
  if (type == ASSET_TYPE_LOCALIZE && name &&
      ClassifyHudKey(std::string(name)) == HUD_KEY_INTRO) {
    void *originalResult =
        g_Original_DB_FindXAssetHeader(type, name, allowCreateDefault);

    if (originalResult) {
      // Repeated DB localize lookups for the same intro key can continue well
      // past the actual intro card, especially across SATFARM's section
      // transitions. Keep English suppression active on every lookup, but only
      // re-queue the same key for the Korean overlay a few times per second.
      bool shouldQueueIntroKey = true;
      {
        static std::mutex s_introQueueThrottleMutex;
        static std::unordered_map<std::string, DWORD> s_introQueueTick;
        const DWORD nowIntro = GetTickCount();
        const std::string introKey(name);
        std::lock_guard<std::mutex> lk(s_introQueueThrottleMutex);
        auto itTick = s_introQueueTick.find(introKey);
        if (itTick != s_introQueueTick.end() &&
            nowIntro >= itTick->second &&
            (nowIntro - itTick->second) < 250) {
          shouldQueueIntroKey = false;
        } else {
          s_introQueueTick[introKey] = nowIntro;
        }
        if (s_introQueueTick.size() > 128) {
          for (auto it = s_introQueueTick.begin();
               it != s_introQueueTick.end();) {
            if (nowIntro >= it->second && (nowIntro - it->second) > 30000) {
              it = s_introQueueTick.erase(it);
            } else {
              ++it;
            }
          }
        }
      }
      if (shouldQueueIntroKey) {
        TextHook_OnIntroKey(name);
      }

      // Suppress English text (blank the value pointer)
      SuppressIntroLocalize(originalResult, name);

      // Log (throttled)
      static int introLogCount = 0;
      if (introLogCount < 100) {
        introLogCount++;
        LogToFile("[FSHook] INTRO suppressed+queued: " + std::string(name));
      }
    }

    return originalResult;
  }

  // =========================================================================
  // 3. Handle gameplay HUD LOCALIZE keys (objectives, hints, game messages)
  //    Safe keys only: NOT MENU_, PLATFORM_, MP_ (those are used in menus)
  // =========================================================================
  if (type == ASSET_TYPE_LOCALIZE && name && ShouldTreatAsGameplayHudKey(name)) {
    void *originalResult =
        g_Original_DB_FindXAssetHeader(type, name, allowCreateDefault);

    if (originalResult) {
      std::string englishValue = ReadLocalizeValue(originalResult, name);
      TextHook_RegisterRuntimeEnglishKey(name, englishValue.c_str());
      const bool loadingPhaseLikely = IsObjectiveLoadingPhaseLikely();
      TextHook_OnObjectiveLocalizeLookup(
          name, englishValue.c_str(), loadingPhaseLikely, false);
      // Runtime-managed hint channels (PLATFORM_ / *_HINT / prompt-style keys)
      // must NOT be queued from DB localize lookups.
      //
      // DB_FindXAssetHeader lookups fire in menus/front-end and can inject
      // menu-only key labels (e.g. PLATFORM_KB_SECONDARY_BUTTON = ESC) into
      // gameplay HUD hint overlays. Real hint lifecycle must come from
      // runtime draw/SLC paths only (HUD560/AddCmd/SLC).
      if (IsRuntimeManagedHintKey(name)) {
        return originalResult;
      }

      // TextHook_OnHudKey returns true if Korean translation was found
      if (TextHook_OnHudKey(name)) {
        // DO NOT suppress English — kept visible for comparison/debugging.
        // Suppressing breaks: pause menu objectives (ADDCMD matches English),
        // native typewriter effect (engine has nothing to render).
        // SuppressIntroLocalize(originalResult, name);

        // Direct status event emission for save/status messages.
        // DB_FindXAssetHeader fires exactly once per real save event — the
        // most reliable signal.  This replaces the probe-based detection
        // that created ghost "게임 저장됨" messages from persistent HudElems.
        {
          const std::string nameStr(name);
          if (nameStr == "EXE_GAMESAVED" || nameStr == "CGAME_NOW_SAVING") {
            TextHook_RecordObjectiveStatusEvent(nameStr, 0, "fshook_direct", 0);
            // NEW: Also feed the unified renderer.
            ObjUnified_RecordStatusEvent(nameStr, 0, "fshook_direct");
          }
        }

        // Log (throttled)
        static int hudLogCount = 0;
        if (hudLogCount < 200) {
          hudLogCount++;
          LogToFile("[FSHook] HUD queued (English visible): " + std::string(name));
        }
      }
    }

    return originalResult;
  }

  // =========================================================================
  // 4. Handle all other LOCALIZE keys for load-phase objective prewarm
  // =========================================================================
  if (type == ASSET_TYPE_LOCALIZE && name) {
    void *originalResult =
        g_Original_DB_FindXAssetHeader(type, name, allowCreateDefault);

    if (originalResult) {
      std::string englishValue = ReadLocalizeValue(originalResult, name);
      TextHook_RegisterRuntimeEnglishKey(name, englishValue.c_str());
      const bool loadingPhaseLikely = IsObjectiveLoadingPhaseLikely();
      const HudKeyType t = ClassifyHudKey(std::string(name));
      bool forceGameplayList = (t == HUD_KEY_OBJECTIVE_LIST ||
                                t == HUD_KEY_OBJECTIVE_UPDATE ||
                                t == HUD_KEY_GAME_SAVED ||
                                t == HUD_KEY_NOW_SAVING);

      const DWORD now = GetTickCount();
      const unsigned long nativeObjTick = TextHook_GetNativeObjectiveItemsTick();
      const bool nativeObjectiveLive =
          (nativeObjTick != 0 &&
           now >= nativeObjTick &&
           (now - nativeObjTick) <= 1800);
      const bool namespaceAllowed =
          ShouldAcceptForceGameplayByNamespace(name, now);

      if (forceGameplayList && namespaceAllowed &&
          (loadingPhaseLikely || nativeObjectiveLive)) {
        TextHook_OnObjectiveLocalizeLookup(
            name, englishValue.c_str(), loadingPhaseLikely, true);

        static std::unordered_map<std::string, DWORD> s_loadCandLogTick;
        const DWORD now = GetTickCount();
        std::string sig = std::string(name) + "|" +
                          (loadingPhaseLikely ? "1" : "0");
        auto it = s_loadCandLogTick.find(sig);
        if (it == s_loadCandLogTick.end() || (now - it->second) > 2500) {
          s_loadCandLogTick[sig] = now;
          if (s_loadCandLogTick.size() > 600) {
            for (auto it2 = s_loadCandLogTick.begin();
                 it2 != s_loadCandLogTick.end();) {
              if ((now - it2->second) > 30000) {
                it2 = s_loadCandLogTick.erase(it2);
              } else {
                ++it2;
              }
            }
          }

          char buf[512];
          sprintf_s(buf,
                    "[FSHOOK-LOAD-CAND] key=%s load=%d eng=\"%.96s\"",
                    name, loadingPhaseLikely ? 1 : 0, englishValue.c_str());
          LogToFile(buf);
        }
      }
    }

    return originalResult;
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

  void *targetAddress = (void *)(moduleBase + offset);
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





