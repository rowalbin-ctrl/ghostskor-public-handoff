#pragma once
#include "Detour.h"
#include <algorithm>
#include <cstdio> // Required for FILE, fopen_s
#include <share.h>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <memory>
#include <mutex>
#include <psapi.h> // Required for MODULEINFO
#include <sstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Global logger state (single instance across translation units).
inline std::mutex g_LogMutex;
inline FILE *g_LogFile = nullptr;
inline DWORD g_LogRateWindowStartMs = 0;
inline int g_LogRateWindowCount = 0;
inline int g_LogFlushCounter = 0;
inline DWORD g_LogLastSizeCheckMs = 0;
inline bool g_LogInitialized = false;
// Lock-free pre-check atomics — shadow the rate limiter state so
// high-frequency callers (R_AddCmdDrawText path) can bail out before
// contending on g_LogMutex.
inline std::atomic<int> g_LogRatePreCount{0};
inline std::atomic<DWORD> g_LogRatePreWindowMs{0};

constexpr const char *kGhostsKorLogPath = "GhostsKor.log";
constexpr int kLogMaxLinesPerSec = 220;
constexpr int kLogMaxLinesPerSecPriority = 520;
constexpr long kLogMaxFileSizeBytes = 12L * 1024L * 1024L; // 12MB cap
constexpr bool kVerboseRuntimeLogs = false;

#ifndef GHOSTSKOR_RUNTIME_DIAG
#define GHOSTSKOR_RUNTIME_DIAG 0
#endif

#ifndef GHOSTSKOR_LOGGING
#define GHOSTSKOR_LOGGING 0
#endif

#ifndef GHOSTSKOR_OSREV_ONLY
#define GHOSTSKOR_OSREV_ONLY 1
#endif

// Forward declaration for runtime flag path probing.
inline std::string GetModuleDirPath(HMODULE hModule);

inline bool HasUtf8Bom(const std::string &text) {
  return text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
         (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF;
}

inline std::string StripUtf8Bom(const std::string &text) {
  if (HasUtf8Bom(text)) {
    return text.substr(3);
  }
  return text;
}

inline bool IsValidUtf8(const char *data, size_t len) {
  size_t i = 0;
  while (i < len) {
    const unsigned char c = (unsigned char)data[i];
    if (c <= 0x7F) {
      ++i;
      continue;
    }

    size_t remaining = 0;
    uint32_t minCodePoint = 0;
    uint32_t codePoint = 0;
    if ((c & 0xE0) == 0xC0) {
      remaining = 1;
      minCodePoint = 0x80;
      codePoint = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      remaining = 2;
      minCodePoint = 0x800;
      codePoint = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      remaining = 3;
      minCodePoint = 0x10000;
      codePoint = c & 0x07;
    } else {
      return false;
    }

    if ((i + remaining) >= len) {
      return false;
    }

    for (size_t j = 1; j <= remaining; ++j) {
      const unsigned char cc = (unsigned char)data[i + j];
      if ((cc & 0xC0) != 0x80) {
        return false;
      }
      codePoint = (codePoint << 6) | (cc & 0x3F);
    }

    if (codePoint < minCodePoint || codePoint > 0x10FFFF ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
      return false;
    }

    i += remaining + 1;
  }

  return true;
}

inline bool IsValidUtf8(const std::string &text) {
  return IsValidUtf8(text.data(), text.size());
}

inline std::wstring DecodeMultiByteToWide(const std::string &input,
                                          UINT codePage) {
  if (input.empty()) {
    return std::wstring();
  }

  int needed = MultiByteToWideChar(codePage, 0, input.data(),
                                   (int)input.size(), nullptr, 0);
  if (needed <= 0) {
    return std::wstring();
  }

  std::wstring wide((size_t)needed, L'\0');
  if (MultiByteToWideChar(codePage, 0, input.data(), (int)input.size(),
                          wide.data(), needed) <= 0) {
    return std::wstring();
  }

  return wide;
}

inline std::wstring Utf8ToWide(const std::string &utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }

  std::string normalized = StripUtf8Bom(utf8);
  if (IsValidUtf8(normalized)) {
    std::wstring wide = DecodeMultiByteToWide(normalized, CP_UTF8);
    if (!wide.empty()) {
      return wide;
    }
  }

  return DecodeMultiByteToWide(normalized, CP_ACP);
}

inline std::string WideToUtf8(const std::wstring &wide) {
  if (wide.empty()) {
    return std::string();
  }

  int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(),
                                   nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return std::string();
  }

  std::string utf8((size_t)needed, '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(),
                          utf8.data(), needed, nullptr, nullptr) <= 0) {
    return std::string();
  }

  return utf8;
}

inline std::string MultiByteToUtf8(const std::string &input, UINT codePage) {
  std::wstring wide = DecodeMultiByteToWide(input, codePage);
  if (wide.empty()) {
    return input;
  }
  return WideToUtf8(wide);
}

inline std::string NormalizeToUtf8(const std::string &text) {
  std::string normalized = StripUtf8Bom(text);
  if (normalized.empty()) {
    return normalized;
  }
  if (IsValidUtf8(normalized)) {
    return normalized;
  }
  return MultiByteToUtf8(normalized, CP_ACP);
}

inline std::string NormalizePossiblyEncodedCString(const char *rawText) {
  if (!rawText) {
    return std::string();
  }
  return NormalizeToUtf8(std::string(rawText));
}

inline std::string GetModulePathUtf8(HMODULE hModule) {
  std::vector<wchar_t> buffer((size_t)MAX_PATH, L'\0');
  DWORD copied = GetModuleFileNameW(hModule, buffer.data(), (DWORD)buffer.size());
  while (copied != 0 && copied >= (buffer.size() - 1)) {
    buffer.resize(buffer.size() * 2, L'\0');
    copied = GetModuleFileNameW(hModule, buffer.data(), (DWORD)buffer.size());
  }
  if (copied == 0) {
    return std::string();
  }
  return WideToUtf8(std::wstring(buffer.data(), buffer.data() + copied));
}

inline bool ReadBinaryFileUtf8Path(const std::string &path, std::string &out) {
  out.clear();

  std::wstring widePath = Utf8ToWide(path);
  if (widePath.empty()) {
    return false;
  }

  HANDLE hFile =
      CreateFileW(widePath.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0) {
    CloseHandle(hFile);
    return false;
  }

  if (size.QuadPart == 0) {
    CloseHandle(hFile);
    return true;
  }

  out.resize((size_t)size.QuadPart);
  DWORD readTotal = 0;
  while (readTotal < out.size()) {
    DWORD chunk = 0;
    DWORD toRead = (DWORD)std::min<size_t>(1u << 20, out.size() - readTotal);
    if (!ReadFile(hFile, &out[readTotal], toRead, &chunk, nullptr)) {
      CloseHandle(hFile);
      out.clear();
      return false;
    }
    if (chunk == 0) {
      break;
    }
    readTotal += chunk;
  }

  CloseHandle(hFile);
  out.resize(readTotal);
  return true;
}

inline bool ReadTextUtf8(const std::string &path, std::string &out) {
  std::string raw;
  if (!ReadBinaryFileUtf8Path(path, raw)) {
    return false;
  }
  out = NormalizeToUtf8(raw);
  return true;
}

inline bool WriteTextUtf8(const std::string &path, const std::string &text) {
  std::wstring widePath = Utf8ToWide(path);
  if (widePath.empty()) {
    return false;
  }

  HANDLE hFile =
      CreateFileW(widePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    return false;
  }

  std::string normalized = StripUtf8Bom(text);
  DWORD written = 0;
  bool ok = normalized.empty() ||
            WriteFile(hFile, normalized.data(), (DWORD)normalized.size(),
                      &written, nullptr);
  CloseHandle(hFile);
  return ok && written == normalized.size();
}

inline HMODULE LoadLibraryUtf8(const std::string &path) {
  std::wstring widePath = Utf8ToWide(path);
  if (widePath.empty()) {
    return nullptr;
  }
  return LoadLibraryW(widePath.c_str());
}

inline int MessageBoxUtf8(HWND hWnd, const std::string &text,
                          const std::string &caption, UINT type) {
  std::wstring wideText = Utf8ToWide(text);
  std::wstring wideCaption = Utf8ToWide(caption);
  return MessageBoxW(hWnd, wideText.c_str(), wideCaption.c_str(), type);
}

inline bool StartsWith(const std::string &s, const char *prefix) {
  size_t n = strlen(prefix);
  return s.size() >= n && memcmp(s.data(), prefix, n) == 0;
}

inline bool IsNoisyRuntimeLog(const std::string &message) {
  return StartsWith(message, "[IRFSub1]") ||
         StartsWith(message, "[IRFSub2]") ||
         StartsWith(message, "[OVERLAY MATCH]") ||
         StartsWith(message, "[OVERLAY MISS]") ||
         StartsWith(message, "[QUEUE]") ||
         StartsWith(message, "[INTROTEXT]") ||
         StartsWith(message, "[ORIG STYLE]") ||
         StartsWith(message, "[STYLE DEBUG]") ||
         StartsWith(message, "[SLC KEYNAME]") ||
         StartsWith(message, "[SLC RBPPARAMS]") ||
         StartsWith(message, "[MENU SCALE]") ||
         StartsWith(message, "[HOVER]") ||
         StartsWith(message, "[D3D11] ProcessSubtitles RUNNING...") ||
         StartsWith(message, "[DXGIWrapper] Present Hook Alive") ||
         StartsWith(message, "[TextHook] New Text:") ||
         StartsWith(message, "[TextHook] INTRO queued:") ||
         StartsWith(message, "[TextHook] INTRO multipass") ||
         StartsWith(message, "[TextHook] TimeScript HUD hook deferred") ||
         StartsWith(message, "[INTRO-TIMER-STACK") ||
         // Per-frame HUD/objective diagnostic tags (suppress when not verbose)
         StartsWith(message, "[NHUD-NATIVE-") ||
         StartsWith(message, "[NHUD-SNAP]") ||
         StartsWith(message, "[NHUD-DYN-CACHE]") ||
         StartsWith(message, "[NHUD-PERF]") ||
         StartsWith(message, "[NHUD-TRACK]") ||
         StartsWith(message, "[NHUD-EVT]") ||
         StartsWith(message, "[NHUD-RT-") ||
         StartsWith(message, "[NHUD-RENDER") ||
         StartsWith(message, "[CFG-REV-") ||
         StartsWith(message, "[OSAUTH-") ||
         StartsWith(message, "[OBJ-REVERSE-") ||
         StartsWith(message, "[OBJ-LATCH-") ||
         StartsWith(message, "[OBJ-GMW-") ||
         StartsWith(message, "[OBJ-CONS-") ||
         StartsWith(message, "[OBJ-CTX-") ||
         StartsWith(message, "[OBJ-RSTATE]") ||
         StartsWith(message, "[OBJ-SCAN]") ||
         StartsWith(message, "[OBJ-NATIVE-SKIP-") ||
         StartsWith(message, "[OBJ-UNIFIED-DIAG]") ||
         StartsWith(message, "[GMW-") ||
         StartsWith(message, "[HUD-MP-") ||
         StartsWith(message, "[HUD-IR-") ||
         StartsWith(message, "[HUD-TAIL-") ||
         StartsWith(message, "[HUD-KEY-AUTO]") ||
         StartsWith(message, "[HUD-SLC-PARAM-") ||
         StartsWith(message, "[HUD-NATIVE-BRIDGE]") ||
         StartsWith(message, "[STATUS-") ||
         StartsWith(message, "[NOHH-RENDER]") ||
         StartsWith(message, "[PAUSE-STRICT]") ||
         StartsWith(message, "[MENU-METRICS]") ||
         StartsWith(message, "[MENU-ALIGN]") ||
         StartsWith(message, "[MENU-MISSION]") ||
         StartsWith(message, "[MENU-FRONT-") ||
         StartsWith(message, "[KoreanRenderer] AtlasUsage") ||
         StartsWith(message, "[OVL-CLAIM]") ||
         StartsWith(message, "[OVL-CLAIM-") ||
         StartsWith(message, "[OSAUTH-DIAG-") ||
         StartsWith(message, "[D3D11] INTRO ") ||
         StartsWith(message, "[D3D11] SUB style") ||
         StartsWith(message, "[HUD-STATE-") ||
         StartsWith(message, "[HUD-OBJECTIVE-") ||
         StartsWith(message, "[NHUD-RT-PARAM-") ||
         StartsWith(message, "[SLC] caller=") ||
         StartsWith(message, "[SLC HUD-MATCH]") ||
         StartsWith(message, "[TextHook] Raw AddCmd:");
}

inline bool IsUrgentLog(const std::string &message) {
  return message.find("ERROR") != std::string::npos ||
         message.find("FAILED") != std::string::npos ||
         message.find("EXCEPTION") != std::string::npos;
}

inline bool IsSubtitleReversePriorityLog(const std::string &message) {
  return StartsWith(message, "[SUBREV_BIND]") ||
         StartsWith(message, "[SUBREV_GUARD]") ||
         StartsWith(message, "[SUBREV_RIP]") ||
         StartsWith(message, "[SUBREV_SLOT]") ||
         // Misc reverse dumps (code/memory hexdumps etc.)
         StartsWith(message, "[SUBREV]") ||
         StartsWith(message, "[SUBREV_REMOVE]") ||
         StartsWith(message, "[SUBREV_SUMMARY]") ||
         // Dvar dumps are reverse instrumentation and should not be dropped by
         // the normal per-sec log limiter during early init.
         StartsWith(message, "[SUBDVAR]");
}

inline void EnsureLogFileOpenLocked() {
  if (g_LogFile)
    return;

  const std::wstring widePath = Utf8ToWide(kGhostsKorLogPath);
  g_LogFile = _wfsopen(widePath.c_str(), L"ab+", _SH_DENYNO);
  if (!g_LogFile)
    return;

  setvbuf(g_LogFile, nullptr, _IOFBF, 64 * 1024);
  fseek(g_LogFile, 0, SEEK_END);
  long size = ftell(g_LogFile);
  if (size > kLogMaxFileSizeBytes) {
    fclose(g_LogFile);
    g_LogFile = nullptr;
    g_LogFile = _wfsopen(widePath.c_str(), L"wb", _SH_DENYNO);
    if (g_LogFile) {
      const char *msg =
          "[GhostsKor] log rotated (startup size cap exceeded)\n";
      fwrite(msg, 1, strlen(msg), g_LogFile);
    }
  }
}

// Helper to create a console for debugging
inline void CreateConsole() {
  AllocConsole();
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  FILE *f;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONIN$", "r", stdin);
  std::cout << "[GhostsKor] Console Initialized (Analysis Mode)" << std::endl;
}

inline void LogToFile(const std::string &message) {
#if !GHOSTSKOR_LOGGING
  (void)message;
  return;
#endif
  if (message.empty())
    return;
  if (!kVerboseRuntimeLogs && IsNoisyRuntimeLog(message))
    return;

  // Lock-free pre-check: bail out early when clearly over rate limit.
  // Avoids mutex contention on the render thread for high-frequency callers.
  {
    const DWORD nowPre = GetTickCount();
    const DWORD winPre = g_LogRatePreWindowMs.load(std::memory_order_relaxed);
    if ((nowPre - winPre) < 1000 &&
        g_LogRatePreCount.load(std::memory_order_relaxed) > kLogMaxLinesPerSec) {
      // Likely over limit — only proceed for urgent/bypass messages.
      if (!IsUrgentLog(message) &&
          !StartsWith(message, "[SUBDVAR]") &&
          !StartsWith(message, "[SIGMON]")) {
        return;
      }
    }
  }

  std::lock_guard<std::mutex> lock(g_LogMutex);
  DWORD now = GetTickCount();
  if (!g_LogInitialized || (now - g_LogRateWindowStartMs) >= 1000) {
    g_LogRateWindowStartMs = now;
    g_LogRateWindowCount = 0;
    g_LogInitialized = true;
    g_LogRatePreWindowMs.store(now, std::memory_order_relaxed);
    g_LogRatePreCount.store(0, std::memory_order_relaxed);
  }

  const bool urgent = IsUrgentLog(message);
  const bool subrevPriority = IsSubtitleReversePriorityLog(message);
  // Dvar dumps are small but extremely useful for reversing; never drop them.
  const bool bypassLimiter =
      StartsWith(message, "[SUBDVAR]") || StartsWith(message, "[SIGMON]");
  int hardLimit = subrevPriority ? kLogMaxLinesPerSecPriority : kLogMaxLinesPerSec;
  if (!urgent && !bypassLimiter) {
    if (++g_LogRateWindowCount > hardLimit) {
      g_LogRatePreCount.store(g_LogRateWindowCount, std::memory_order_relaxed);
      return;
    }
  }
  g_LogRatePreCount.store(g_LogRateWindowCount, std::memory_order_relaxed);

  EnsureLogFileOpenLocked();
  if (!g_LogFile)
    return;

  fprintf(g_LogFile, "%s\n", message.c_str());
  g_LogFlushCounter++;

  bool shouldFlush = false;
  if (urgent || subrevPriority)
    shouldFlush = true;
  else if (g_LogFlushCounter >= 64)
    shouldFlush = true;
  else if ((now - g_LogRateWindowStartMs) > 1000)
    shouldFlush = true;

  if (shouldFlush) {
    fflush(g_LogFile);
    g_LogFlushCounter = 0;
  }

  if ((now - g_LogLastSizeCheckMs) > 3000) {
    g_LogLastSizeCheckMs = now;
    fseek(g_LogFile, 0, SEEK_END);
    long size = ftell(g_LogFile);
    if (size > kLogMaxFileSizeBytes) {
      fclose(g_LogFile);
      g_LogFile = nullptr;
      const std::wstring widePath = Utf8ToWide(kGhostsKorLogPath);
      g_LogFile = _wfsopen(widePath.c_str(), L"wb", _SH_DENYNO);
      if (g_LogFile) {
        const char *msg = "[GhostsKor] log rotated (size cap exceeded)\n";
        fwrite(msg, 1, strlen(msg), g_LogFile);
        fflush(g_LogFile);
      }
    }
  }
}

// Runtime feature snapshot (built-in defaults; no external JSON file).
struct RuntimeFlagsSnapshot {
  bool hook_backend = true;
  std::string objective_status_source = "osrev_only";
  bool hud_seed_v2_enabled = true;
  bool classify_unified_enabled = true;
  bool shadow_compare_enabled = false;
};

inline RuntimeFlagsSnapshot g_RuntimeFlags;
inline std::mutex g_RuntimeFlagsMutex;
inline std::once_flag g_RuntimeFlagsOnce;
inline std::atomic<bool> g_RuntimeFlagsLoaded{false};

inline std::string RuntimeFlags_Upper(std::string s) {
  for (char &c : s) {
    c = (char)toupper((unsigned char)c);
  }
  return s;
}

inline std::string TrimAscii(const std::string &s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace((unsigned char)s[b]))
    ++b;
  while (e > b && std::isspace((unsigned char)s[e - 1]))
    --e;
  return s.substr(b, e - b);
}

inline std::string RuntimeFlags_Trim(const std::string &s) {
  return TrimAscii(s);
}

inline RuntimeFlagsSnapshot RuntimeFlags_GetSnapshot() {
  std::lock_guard<std::mutex> lk(g_RuntimeFlagsMutex);
  return g_RuntimeFlags;
}

inline const char *RuntimeFlags_ObjectiveStatusSource() {
  static thread_local std::string cached;
  {
    std::lock_guard<std::mutex> lk(g_RuntimeFlagsMutex);
    cached = g_RuntimeFlags.objective_status_source;
  }
  return cached.c_str();
}

inline bool RuntimeFlags_ObjectiveStatusEnableOsRevOnly() {
#if GHOSTSKOR_OSREV_ONLY
  return true;
#else
  RuntimeFlagsSnapshot f = RuntimeFlags_GetSnapshot();
  std::string src = RuntimeFlags_Upper(RuntimeFlags_Trim(f.objective_status_source));
  return (src == "OSREV_ONLY" || src == "OSREV" || src == "NATIVE_SNAPSHOT");
#endif
}

inline bool RuntimeFlags_ObjectiveStatusEnableConsumeCap() {
#if GHOSTSKOR_OSREV_ONLY
  return false;
#else
  RuntimeFlagsSnapshot f = RuntimeFlags_GetSnapshot();
  std::string src = RuntimeFlags_Upper(RuntimeFlags_Trim(f.objective_status_source));
  return (src == "CONSUME_CAP" || src == "CONSUMER_CAP" ||
          src == "OBJ_CONSUMER");
#endif
}

inline bool RuntimeFlags_ObjectiveStatusEnableSlcStatusProducer() {
#if GHOSTSKOR_OSREV_ONLY
  return false;
#else
  RuntimeFlagsSnapshot f = RuntimeFlags_GetSnapshot();
  std::string src = RuntimeFlags_Upper(RuntimeFlags_Trim(f.objective_status_source));
  return (src == "SLC_STATUS_PRODUCER" || src == "SLC" ||
          src == "SLC_PRODUCER");
#endif
}

inline bool RuntimeFlags_ObjectiveStatusEnableGameMessage() {
#if GHOSTSKOR_OSREV_ONLY
  return false;
#else
  RuntimeFlagsSnapshot f = RuntimeFlags_GetSnapshot();
  std::string src = RuntimeFlags_Upper(RuntimeFlags_Trim(f.objective_status_source));
  return (src == "CG_GAME_MESSAGE" || src == "CG_GAMEMESSAGE" ||
          src == "GAME_MESSAGE");
#endif
}

inline void RuntimeFlags_EnsureLoaded(HMODULE moduleHint = NULL) {
  (void)moduleHint;
  std::call_once(g_RuntimeFlagsOnce, [&]() {
    RuntimeFlagsSnapshot builtins;
    {
      std::lock_guard<std::mutex> lk(g_RuntimeFlagsMutex);
      g_RuntimeFlags = builtins;
    }

    char buf[320];
    sprintf_s(
        buf,
        "[RuntimeFlags] built-in defaults hook_backend=%d objective_status_source=%s hud_seed_v2=%d classify_unified=%d shadow_compare=%d",
        builtins.hook_backend ? 1 : 0, builtins.objective_status_source.c_str(),
        builtins.hud_seed_v2_enabled ? 1 : 0,
        builtins.classify_unified_enabled ? 1 : 0,
        builtins.shadow_compare_enabled ? 1 : 0);
    LogToFile(buf);

    g_RuntimeFlagsLoaded.store(true, std::memory_order_release);
  });
}

inline bool RuntimeFlags_IsLoaded() {
  return g_RuntimeFlagsLoaded.load(std::memory_order_acquire);
}
// UTF-8 safe string truncation - avoids cutting in middle of multi-byte chars
// maxBytes: maximum byte length for the result (excluding null terminator)
// Returns a string truncated at a valid UTF-8 character boundary
inline std::string TruncateUTF8(const std::string &str, size_t maxBytes) {
  if (str.size() <= maxBytes) {
    return str;
  }
  
  size_t len = 0;
  size_t lastValidPos = 0;
  
  while (len < str.size() && lastValidPos < maxBytes) {
    unsigned char c = static_cast<unsigned char>(str[len]);
    size_t charBytes = 1;
    
    // Determine UTF-8 character byte length
    if ((c & 0x80) == 0) {
      charBytes = 1;       // ASCII: 0xxxxxxx
    } else if ((c & 0xE0) == 0xC0) {
      charBytes = 2;       // 2-byte: 110xxxxx
    } else if ((c & 0xF0) == 0xE0) {
      charBytes = 3;       // 3-byte: 1110xxxx (Korean, etc.)
    } else if ((c & 0xF8) == 0xF0) {
      charBytes = 4;       // 4-byte: 11110xxx (emoji, rare chars)
    }
    
    // Check if this character fits within maxBytes
    if (len + charBytes > maxBytes) {
      break;
    }
    
    len += charBytes;
    lastValidPos = len;
  }
  
  return str.substr(0, lastValidPos);
}

inline std::string Utf8Preview(const std::string &text, size_t maxBytes) {
  return TruncateUTF8(NormalizeToUtf8(text), maxBytes);
}

inline std::string GetModuleDirPath(HMODULE hModule) {
  std::string modulePath = GetModulePathUtf8(hModule);
  if (modulePath.empty())
    return std::string();
  size_t lastSlash = modulePath.find_last_of("\\/");
  if (lastSlash != std::string::npos) {
    modulePath.resize(lastSlash);
  }
  return modulePath;
}

// Log specific variant for text that might be in game encoding (ANSI/UTF8
// mixed)
inline void LogText(const std::string &prefix, const char *rawText) {
  if (!rawText)
    return;
  std::string msg = prefix;
  msg += NormalizePossiblyEncodedCString(rawText);
  LogToFile(msg);
}

// Helper: Get Module Info
inline MODULEINFO GetModuleInfo(char *szModule) {
  MODULEINFO modinfo = {0};
  HMODULE hModule = GetModuleHandle(szModule);
  if (hModule == 0)
    return modinfo;
  GetModuleInformation(GetCurrentProcess(), hModule, &modinfo,
                       sizeof(MODULEINFO));
  return modinfo;
}

// Helper: Find Pattern
inline void *FindPattern(char *base, size_t size, const char *pattern,
                         const char *mask) {
  size_t patternLength = strlen(mask);
  for (size_t i = 0; i < size - patternLength; i++) {
    bool found = true;
    for (size_t j = 0; j < patternLength; j++) {
      if (mask[j] != '?' && pattern[j] != *(base + i + j)) {
        found = false;
        break;
      }
    }
    if (found) {
      return (void *)(base + i);
    }
  }
  return nullptr;
}

// Helper: Check if a pointer is safe to read/write
inline bool IsSafeRead(void *ptr, size_t size) {
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  if (mbi.Protect & PAGE_GUARD)
    return false;
  if (mbi.Protect & PAGE_NOACCESS)
    return false;
  return true;
}

// Helper: Resolve RIP-relative
inline void *ResolveRipRelative(void *addr, int instructionLength,
                                int offsetFromInstructionStart) {
  int32_t offset = *(int32_t *)((char *)addr + offsetFromInstructionStart);
  return (void *)((char *)addr + instructionLength + offset);
}

// Helper to easily hook functions
// stolenBytes: Number of bytes to copy from target function prologue
// - Use 14 for SEH_StringEd_GetString (avoids relative jump at byte 14-16)
// - Use 16 for R_AddCmdDrawText (default, safe for most functions)
inline bool CreateHook(void *target, void *detour, void **original,
                       int stolenBytes = 16) {
  if (Detour::Install(target, detour, original, stolenBytes)) {
#if GHOSTSKOR_LOGGING
    std::string msg = "[GhostsKor] Hook enabled for address: " +
                      std::to_string((uintptr_t)target) +
                      " (stolenBytes=" + std::to_string(stolenBytes) +
                      ", backend=" + Detour::ActiveBackendName() + ")";
    std::cout << msg << std::endl;
    LogToFile(msg);
#endif
    return true;
  }

#if GHOSTSKOR_LOGGING
  std::string msg = "[GhostsKor] Failed to create hook for address: " +
                    std::to_string((uintptr_t)target) +
                    " (backend=" + Detour::ActiveBackendName() + ")";
  std::cout << msg << std::endl;
  LogToFile(msg);
#endif
  return false;
}

// IAT Hook Helper
inline bool IATHook(void *moduleBase, const char *importDllName,
                    const char *importFuncName, void *newFunc,
                    void **originalFunc) {
  PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)moduleBase;
  if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    return false;

  PIMAGE_NT_HEADERS pNtHeaders =
      (PIMAGE_NT_HEADERS)((BYTE *)moduleBase + pDosHeader->e_lfanew);
  PIMAGE_IMPORT_DESCRIPTOR pImportDesc =
      (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)moduleBase +
                                 pNtHeaders->OptionalHeader
                                     .DataDirectory
                                         [IMAGE_DIRECTORY_ENTRY_IMPORT]
                                     .VirtualAddress);

  if (pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
          .Size == 0)
    return false;

  while (pImportDesc->Name) {
    char *dllName = (char *)((BYTE *)moduleBase + pImportDesc->Name);
    if (_stricmp(dllName, importDllName) == 0) {
      PIMAGE_THUNK_DATA pThunk =
          (PIMAGE_THUNK_DATA)((BYTE *)moduleBase + pImportDesc->FirstThunk);
      PIMAGE_THUNK_DATA pOrigThunk =
          (PIMAGE_THUNK_DATA)((BYTE *)moduleBase +
                              pImportDesc->OriginalFirstThunk);

      if (!pOrigThunk)
        pOrigThunk = pThunk; // Fallback

      while (pThunk->u1.Function) {
        if (pOrigThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
          // Ordinal import - skip or handle if needed
        } else {
          PIMAGE_IMPORT_BY_NAME pImport =
              (PIMAGE_IMPORT_BY_NAME)((BYTE *)moduleBase +
                                      pOrigThunk->u1.AddressOfData);
          if (strcmp(pImport->Name, importFuncName) == 0) {
            DWORD oldProtect;
            if (VirtualProtect(&pThunk->u1.Function, sizeof(void *),
                               PAGE_READWRITE, &oldProtect)) {
              if (originalFunc)
                *originalFunc = (void *)pThunk->u1.Function;
              pThunk->u1.Function = (ULONGLONG)newFunc;
              VirtualProtect(&pThunk->u1.Function, sizeof(void *), oldProtect,
                             &oldProtect);
              LogToFile(std::string("[Utils] IAT Hooked: ") + importFuncName);
              return true;
            }
          }
        }
        pThunk++;
        pOrigThunk++;
      }
    }
    pImportDesc++;
  }
  return false;
}



