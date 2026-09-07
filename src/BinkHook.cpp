// =============================================================================
// BinkHook.cpp - Hooks bink2w64.dll to detect video playback and timing
// =============================================================================

#include "BinkHook.h"
#include "Utils.h"  // For IATHook
#include "IW6Offsets.h"
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <windows.h>

// External logging function
extern void LogToFile(const std::string& msg);

// Credits-guard timing globals (defined in texthook.cpp, global namespace)
extern std::atomic<DWORD>    g_lastBinkVideoCloseTick;
extern std::atomic<DWORD>    g_lastBinkVideoOpenTick;
extern std::atomic<uint64_t> g_presentFrameCount;
extern std::atomic<uint64_t> g_presentFrameAtLastVideoClose;

namespace BinkHook {

// =============================================================================
// Dvar System - For disabling English subtitles during video playback
// =============================================================================

// Forward declaration of Dvar structure (opaque)
struct dvar_t;

// Dvar function types
typedef dvar_t* (*Dvar_FindVar_t)(const char* name);
typedef void (*Dvar_SetBool_t)(const dvar_t* dvar, bool value);
typedef bool (*Dvar_GetBool_t)(const char* dvarName);
typedef void (*Dvar_SetFloat_t)(const dvar_t* dvar, float value);

// Dvar function pointers (resolved at runtime)
static Dvar_FindVar_t g_Dvar_FindVar = nullptr;
static Dvar_SetBool_t g_Dvar_SetBool = nullptr;
static Dvar_GetBool_t g_Dvar_GetBool = nullptr;

// Saved subtitle state (to restore after video)
static bool g_SubtitleWasEnabled = true;
static bool g_DvarResolved = false;
static float g_SavedSubtitleWidthStandard = 0.5f;
static float g_SavedSubtitleWidthWidescreen = 0.5f;

// Resolve Dvar functions from game
static void ResolveDvarFunctions() {
    if (g_DvarResolved) return;
    
    uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
    
    g_Dvar_FindVar = (Dvar_FindVar_t)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(moduleBase, IW6Offsets::Dvar_FindVar_SP)));
    
    // Dvar_SetBool offset from symbols.hpp: SP=0x42C370
    g_Dvar_SetBool = (Dvar_SetBool_t)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(moduleBase, IW6Offsets::Dvar_SetBool_SP)));
    
    // Dvar_GetBool offset from symbols.hpp: SP=0x429FC0
    g_Dvar_GetBool = (Dvar_GetBool_t)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(moduleBase, IW6Offsets::Dvar_GetBool_SP)));
    
    g_DvarResolved = g_Dvar_FindVar && g_Dvar_SetBool && g_Dvar_GetBool;
    LogToFile("[BinkHook] Dvar functions resolved");
}

// Disable game's native subtitle rendering (for video playback)
static void DisableNativeSubtitles() {
    if (!g_DvarResolved) ResolveDvarFunctions();
    
    if (!g_Dvar_FindVar || !g_Dvar_SetBool) {
        LogToFile("[BinkHook] WARNING: Dvar functions not available");
        return;
    }
    
    // Method 1: Try bool dvars
    const char* boolDvarNames[] = {"cl_subtitles", "cg_subtitles", "subtitles", "subtitle"};
    
    for (const char* name : boolDvarNames) {
        dvar_t* dvar = g_Dvar_FindVar(name);
        if (dvar) {
            if (g_Dvar_GetBool) {
                g_SubtitleWasEnabled = g_Dvar_GetBool(name);
            }
            g_Dvar_SetBool(dvar, false);
            LogToFile(std::string("[BinkHook] Disabled bool subtitle dvar: ") + name);
            return;
        }
    }
    
    // Method 2: Set subtitle width to 0 using Dvar_SetCommand
    // This will make subtitles invisible by setting width to minimum
    typedef void (*Cbuf_AddText_t)(int localClientNum, const char* text);
    static Cbuf_AddText_t Cbuf_AddText = nullptr;
    
    if (!Cbuf_AddText) {
        uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
        // Cbuf_AddText offset from symbols.hpp: SP=0x3B3050
        Cbuf_AddText = (Cbuf_AddText_t)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(moduleBase, IW6Offsets::Cbuf_AddText_SP)));
    }
    
    if (Cbuf_AddText) {
        // Save original values (we'll need to restore them)
        // Set subtitle width to very small value to hide them
        Cbuf_AddText(0, "cg_subtitleWidthStandard 0.001\n");
        Cbuf_AddText(0, "cg_subtitleWidthWidescreen 0.001\n");
        LogToFile("[BinkHook] Set subtitle width to 0.001 via Cbuf_AddText");
        return;
    }
    
    LogToFile("[BinkHook] No subtitle control method available");
}

// Re-enable subtitles after video playback
static void EnableNativeSubtitles() {
    if (!g_DvarResolved || !g_Dvar_FindVar || !g_Dvar_SetBool) return;
    
    // Method 1: Try bool dvars
    const char* boolDvarNames[] = {"cl_subtitles", "cg_subtitles", "subtitles", "subtitle"};
    
    for (const char* name : boolDvarNames) {
        dvar_t* dvar = g_Dvar_FindVar(name);
        if (dvar) {
            g_Dvar_SetBool(dvar, g_SubtitleWasEnabled);
            LogToFile(std::string("[BinkHook] Restored bool subtitle dvar: ") + name);
            return;
        }
    }
    
    // Method 2: Restore subtitle width
    typedef void (*Cbuf_AddText_t)(int localClientNum, const char* text);
    static Cbuf_AddText_t Cbuf_AddText = nullptr;
    
    if (!Cbuf_AddText) {
        uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
        Cbuf_AddText = (Cbuf_AddText_t)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(moduleBase, IW6Offsets::Cbuf_AddText_SP)));
    }
    
    if (Cbuf_AddText) {
        // Restore original subtitle width values
        Cbuf_AddText(0, "cg_subtitleWidthStandard 0.5\n");
        Cbuf_AddText(0, "cg_subtitleWidthWidescreen 0.5\n");
        LogToFile("[BinkHook] Restored subtitle width to 0.5 via Cbuf_AddText");
    }
}

// =============================================================================
// Bink2 Types and Function Pointers
// =============================================================================

// Bink handle structure (opaque, we only need some fields)
struct BINK {
    uint32_t Width;
    uint32_t Height;
    uint32_t Frames;
    uint32_t FrameNum;          // Current frame (1-based)
    uint32_t LastFrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    // ... more fields we don't need
};

// Function signatures - x64 uses Microsoft x64 calling convention (no __stdcall)
typedef BINK* (*BinkOpen_t)(const char* filename, uint32_t flags);
typedef void  (*BinkClose_t)(BINK* bink);
typedef int   (*BinkDoFrame_t)(BINK* bink);
typedef void  (*BinkNextFrame_t)(BINK* bink);
typedef int   (*BinkWait_t)(BINK* bink);

// Original function pointers
static BinkOpen_t      Original_BinkOpen = nullptr;
static BinkClose_t     Original_BinkClose = nullptr;
static BinkDoFrame_t   Original_BinkDoFrame = nullptr;
static BinkNextFrame_t Original_BinkNextFrame = nullptr;
static BinkWait_t      Original_BinkWait = nullptr;

// Hook installation status (runtime verification)
static std::atomic<bool> g_HookOpenInstalled{false};
static std::atomic<bool> g_HookCloseInstalled{false};
static std::atomic<bool> g_HookDoFrameInstalled{false};
static std::atomic<bool> g_HookNextFrameInstalled{false};
static std::atomic<bool> g_HookWaitInstalled{false};

// =============================================================================
// State Tracking
// =============================================================================

static std::mutex g_StateMutex;
static bool g_VideoPlaying = false;
static bool g_CutsceneVideoPlaying = false;
static std::string g_CurrentVideoName;
static std::string g_CurrentVideoLookupName;
static BINK* g_CurrentBink = nullptr;
static std::atomic<BINK*> g_CurrentBinkAtomic{nullptr};
static std::chrono::steady_clock::time_point g_VideoStartTime;
static uint32_t g_StartFrame = 0;
static bool g_SubtitleRestorePending = false;
static DWORD g_SubtitleRestoreQueuedTick = 0;
static std::atomic<long long> g_QpcFrequency{0};
static std::atomic<long long> g_NativeAnchorQpc{0};
static std::atomic<long long> g_NativeLastQpc{0};
static std::atomic<unsigned long long> g_CurrentVideoNativeTickCalls{0};

// Native timeline probe counters (verification-only)
static std::atomic<unsigned long long> g_DoFrameCalls{0};
static std::atomic<unsigned long long> g_NextFrameCalls{0};
static std::atomic<unsigned long long> g_WaitCalls{0};
static std::atomic<unsigned long long> g_CurrentVideoDoFrameCalls{0};
static std::atomic<unsigned long long> g_CurrentVideoNextFrameCalls{0};
static std::atomic<unsigned long long> g_CurrentVideoWaitCalls{0};
static std::atomic<unsigned long long> g_VideoSessionCounter{0};
static std::atomic<unsigned long long> g_ActiveVideoSessionId{0};
static std::atomic<unsigned long long> g_VerifyWarnedSessionId{0};
static std::atomic<unsigned long long> g_VerifyReadySessionId{0};

// Subtitle data
static std::vector<VideoSubtitle> g_AllSubtitles;
static std::vector<VideoSubtitle> g_CurrentVideoSubtitles;
static std::unordered_set<std::string> g_KnownSubtitleVideoNames;
static bool g_NativeSubtitleSuppressed = false;
static size_t g_CurrentSubtitleIndexHint = 0;
static std::mutex g_SubtitleMutex;

// Korean translations (loaded from localize_kr.json)
static std::unordered_map<std::string, std::string> g_EnglishToKorean;

// Forward declaration
static std::string NormalizeForMatch(const std::string& text);
static bool TryGetNativePlaybackTimeSeconds_NoLock(double& outSeconds);
static double GetPlaybackTimeSeconds_NoLock(bool* outUsingNativeTimeline = nullptr);
static std::string CanonicalizeVideoName(const std::string& text);
static long long GetOrInitQpcFrequency() {
    long long freq = g_QpcFrequency.load(std::memory_order_relaxed);
    if (freq > 0) {
        return freq;
    }

    LARGE_INTEGER liFreq{};
    if (!QueryPerformanceFrequency(&liFreq) || liFreq.QuadPart <= 0) {
        return 0;
    }

    g_QpcFrequency.store(liFreq.QuadPart, std::memory_order_relaxed);
    return liFreq.QuadPart;
}

static long long QueryQpcNow() {
    LARGE_INTEGER liNow{};
    if (!QueryPerformanceCounter(&liNow)) {
        return 0;
    }
    return liNow.QuadPart;
}

static void RecordNativeTimelineTickForCurrentVideo() {
    const long long qpcNow = QueryQpcNow();
    if (qpcNow <= 0) {
        return;
    }

    if (g_NativeAnchorQpc.load(std::memory_order_relaxed) <= 0) {
        g_NativeAnchorQpc.store(qpcNow, std::memory_order_relaxed);
    }

    g_NativeLastQpc.store(qpcNow, std::memory_order_relaxed);
    g_CurrentVideoNativeTickCalls.fetch_add(1, std::memory_order_relaxed);
}

static void EmitNativeTimelineDiagnostics(bool videoPlaying,
                                          const std::string& videoName,
                                          double steadyElapsedSec,
                                          double playbackElapsedSec,
                                          bool usingNativeTimeline) {
    static DWORD s_lastDiagTick = 0;
    static unsigned long long s_lastDoFrameCalls = 0;
    static unsigned long long s_lastNextFrameCalls = 0;
    static unsigned long long s_lastWaitCalls = 0;
    static double s_lastElapsedSec = 0.0;

    const DWORD now = GetTickCount();
    if (now - s_lastDiagTick < 1000) {
        return;
    }

    const unsigned long long doFrameCalls = g_DoFrameCalls.load(std::memory_order_relaxed);
    const unsigned long long nextFrameCalls = g_NextFrameCalls.load(std::memory_order_relaxed);
    const unsigned long long waitCalls = g_WaitCalls.load(std::memory_order_relaxed);
    const unsigned long long curDoFrameCalls =
        g_CurrentVideoDoFrameCalls.load(std::memory_order_relaxed);
    const unsigned long long curNextFrameCalls =
        g_CurrentVideoNextFrameCalls.load(std::memory_order_relaxed);
    const unsigned long long curWaitCalls =
        g_CurrentVideoWaitCalls.load(std::memory_order_relaxed);
    const unsigned long long curNativeTicks =
        g_CurrentVideoNativeTickCalls.load(std::memory_order_relaxed);
    const unsigned long long sessionId =
        g_ActiveVideoSessionId.load(std::memory_order_relaxed);

    double deltaSec = steadyElapsedSec - s_lastElapsedSec;
    if (deltaSec <= 0.0 || deltaSec > 10.0) {
        deltaSec = 1.0;
    }
    const double doFrameFps =
        (double)(doFrameCalls - s_lastDoFrameCalls) / deltaSec;
    const double nextFrameFps =
        (double)(nextFrameCalls - s_lastNextFrameCalls) / deltaSec;
    const double waitFps =
        (double)(waitCalls - s_lastWaitCalls) / deltaSec;

    s_lastDiagTick = now;
    s_lastDoFrameCalls = doFrameCalls;
    s_lastNextFrameCalls = nextFrameCalls;
    s_lastWaitCalls = waitCalls;
    s_lastElapsedSec = steadyElapsedSec;

    if (videoPlaying) {
        const double driftFromSteadySec = playbackElapsedSec - steadyElapsedSec;
        char buf[768];
        sprintf_s(
            buf,
            "[BinkHook][VERIFY] video=%s sess=%llu steady=%.3fs play=%.3fs src=%s drift=%.3fs "
            "hook(df=%d nf=%d wt=%d) calls(df=%llu nf=%llu wt=%llu) "
            "curr(df=%llu nf=%llu wt=%llu nt=%llu) rate(df=%.1f nf=%.1f wt=%.1f)",
            videoName.c_str(), sessionId, steadyElapsedSec, playbackElapsedSec,
            usingNativeTimeline ? "native" : "steady", driftFromSteadySec,
            g_HookDoFrameInstalled.load(std::memory_order_relaxed) ? 1 : 0,
            g_HookNextFrameInstalled.load(std::memory_order_relaxed) ? 1 : 0,
            g_HookWaitInstalled.load(std::memory_order_relaxed) ? 1 : 0,
            doFrameCalls, nextFrameCalls, waitCalls, curDoFrameCalls,
            curNextFrameCalls, curWaitCalls, curNativeTicks,
            doFrameFps, nextFrameFps, waitFps);
        LogToFile(buf);

        // Session-level verdict: is the active session using native timeline ticks?
        if (curNativeTicks == 0 && steadyElapsedSec >= 1.5) {
            const unsigned long long warnedSession =
                g_VerifyWarnedSessionId.load(std::memory_order_relaxed);
            if (warnedSession != sessionId) {
                g_VerifyWarnedSessionId.store(sessionId, std::memory_order_relaxed);
                LogToFile(
                    "[BinkHook][VERIFY] WARNING: active video has no DoFrame/NextFrame timeline ticks yet (steady fallback)");
            }
        } else if (curNativeTicks > 0) {
            const unsigned long long readySession =
                g_VerifyReadySessionId.load(std::memory_order_relaxed);
            if (readySession != sessionId) {
                g_VerifyReadySessionId.store(sessionId, std::memory_order_relaxed);
                LogToFile(
                    "[BinkHook][VERIFY] OK: native callback timeline observed for active video");
            }
        }
    }
}

static void ApplyTranslationsToSubtitles() {
    if (g_EnglishToKorean.empty() || g_AllSubtitles.empty()) {
        return;
    }

    int matched = 0;
    for (auto& sub : g_AllSubtitles) {
        std::string normalized = NormalizeForMatch(sub.englishText);
        auto it = g_EnglishToKorean.find(normalized);
        if (it != g_EnglishToKorean.end()) {
            sub.koreanText = it->second;
            matched++;
        } else {
            sub.koreanText.clear();
        }
    }

    LogToFile("[BinkHook] Matched " + std::to_string(matched) + "/" +
              std::to_string(g_AllSubtitles.size()) +
              " subtitles with Korean translations");
}

// =============================================================================
// Helper Functions
// =============================================================================

// Extract video name from full path (e.g., "C:\...\black_ice_load.bik" -> "black_ice_load")
static std::string ExtractVideoName(const char* fullPath) {
    std::string path(fullPath);
    
    // Find last slash or backslash
    size_t lastSlash = path.find_last_of("\\/");
    std::string filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    
    // Remove .bik extension
    size_t dotPos = filename.find_last_of('.');
    if (dotPos != std::string::npos) {
        filename = filename.substr(0, dotPos);
    }
    
    // Convert to lowercase for matching
    std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
    
    return filename;
}

// Strip color codes from text
static std::string StripColorCodes(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '^' && i + 1 < text.size()) {
            char next = text[i + 1];
            if ((next >= '0' && next <= '9') || (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')) {
                ++i;  // Skip the color code
                continue;
            }
        }
        result += text[i];
    }
    
    return result;
}

// Normalize text for matching (uppercase, stripped)
static std::string NormalizeForMatch(const std::string& text) {
    std::string stripped = StripColorCodes(text);
    std::transform(stripped.begin(), stripped.end(), stripped.begin(), ::toupper);
    return stripped;
}

static std::string CanonicalizeVideoName(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    bool lastWasUnderscore = false;
    for (unsigned char ch : text) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9')) {
            out.push_back((char)std::tolower(ch));
            lastWasUnderscore = false;
        } else if (!out.empty() && !lastWasUnderscore &&
                   (ch == '_' || ch == '-' || ch == ' ' || ch == '.' ||
                    ch == '/' || ch == '\\')) {
            out.push_back('_');
            lastWasUnderscore = true;
        }
    }

    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }

    return out;
}

static bool IsSafeCloseVideoNameMatch(const std::string& currentName,
                                      const std::string& knownName) {
    if (currentName.empty() || knownName.empty()) {
        return false;
    }
    if (currentName == knownName) {
        return true;
    }

    auto boundaryMatch = [](const std::string& longer,
                            const std::string& shorter) {
        if (longer.size() <= shorter.size() ||
            longer.compare(0, shorter.size(), shorter) != 0) {
            return false;
        }

        const unsigned char boundary = (unsigned char)longer[shorter.size()];
        return boundary == '_' || boundary == '-' ||
               (boundary >= '0' && boundary <= '9');
    };

    return boundaryMatch(currentName, knownName) ||
           boundaryMatch(knownName, currentName);
}

static bool FindMatchedSubtitleVideoName_NoLock(
    const std::string& currentLookupName, std::string& outMatchedName) {
    outMatchedName.clear();

    if (currentLookupName.empty() || g_KnownSubtitleVideoNames.empty()) {
        return false;
    }

    if (g_KnownSubtitleVideoNames.find(currentLookupName) !=
        g_KnownSubtitleVideoNames.end()) {
        outMatchedName = currentLookupName;
        return true;
    }

    std::string closeMatch;
    for (const auto& knownName : g_KnownSubtitleVideoNames) {
        if (!IsSafeCloseVideoNameMatch(currentLookupName, knownName)) {
            continue;
        }
        if (!closeMatch.empty() && closeMatch != knownName) {
            return false;
        }
        closeMatch = knownName;
    }

    if (closeMatch.empty()) {
        return false;
    }

    outMatchedName = closeMatch;
    return true;
}

static bool IsWhitelistedCutsceneVideo_NoLock() {
    return g_CurrentVideoLookupName == "satfarm_b_load";
}

static void RefreshCutsceneState_NoLock() {
    g_CutsceneVideoPlaying =
        g_VideoPlaying && (!g_CurrentVideoSubtitles.empty() || IsWhitelistedCutsceneVideo_NoLock());
}

static bool TryGetNativePlaybackTimeSeconds_NoLock(double& outSeconds) {
    outSeconds = 0.0;

    const long long qpcFreq = GetOrInitQpcFrequency();
    if (qpcFreq <= 0) {
        return false;
    }

    // Native timeline is considered valid only after we observe callback ticks
    // for the active video session.
    const unsigned long long nativeTicks =
        g_CurrentVideoNativeTickCalls.load(std::memory_order_relaxed);
    if (nativeTicks == 0) {
        return false;
    }

    const long long anchorQpc = g_NativeAnchorQpc.load(std::memory_order_relaxed);
    const long long lastQpc = g_NativeLastQpc.load(std::memory_order_relaxed);
    if (anchorQpc <= 0 || lastQpc < anchorQpc) {
        return false;
    }

    outSeconds = (double)(lastQpc - anchorQpc) / (double)qpcFreq;
    if (outSeconds < 0.0) {
        outSeconds = 0.0;
    }
    return true;
}

static double GetPlaybackTimeSeconds_NoLock(bool* outUsingNativeTimeline) {
    double nativeElapsedSec = 0.0;
    if (TryGetNativePlaybackTimeSeconds_NoLock(nativeElapsedSec)) {
        if (outUsingNativeTimeline) {
            *outUsingNativeTimeline = true;
        }
        return nativeElapsedSec;
    }

    if (outUsingNativeTimeline) {
        *outUsingNativeTimeline = false;
    }

    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - g_VideoStartTime).count();
}

static void RebuildCurrentVideoSubtitleCache_NoLock() {
    g_CurrentVideoSubtitles.clear();
    g_CurrentSubtitleIndexHint = 0;
    g_CutsceneVideoPlaying = false;

    if (g_CurrentVideoLookupName.empty()) {
        return;
    }

    std::string matchedVideoName;
    if (!FindMatchedSubtitleVideoName_NoLock(g_CurrentVideoLookupName,
                                             matchedVideoName)) {
        RefreshCutsceneState_NoLock();
        return;
    }

    for (const auto& sub : g_AllSubtitles) {
        if (sub.videoName == matchedVideoName) {
            g_CurrentVideoSubtitles.push_back(sub);
        }
    }

    std::sort(g_CurrentVideoSubtitles.begin(), g_CurrentVideoSubtitles.end(),
              [](const VideoSubtitle& lhs, const VideoSubtitle& rhs) {
                  if (lhs.startTime != rhs.startTime) {
                      return lhs.startTime < rhs.startTime;
                  }
                  return lhs.endTime < rhs.endTime;
              });

    RefreshCutsceneState_NoLock();
}

static ptrdiff_t FindActiveSubtitleIndex_NoLock(double currentTime) {
    if (g_CurrentVideoSubtitles.empty()) {
        return -1;
    }

    const size_t count = g_CurrentVideoSubtitles.size();
    size_t idx = g_CurrentSubtitleIndexHint;
    if (idx >= count) {
        idx = count - 1;
    }

    auto isActiveAt = [currentTime](const VideoSubtitle& sub) {
        return currentTime >= sub.startTime && currentTime <= sub.endTime;
    };

    if (isActiveAt(g_CurrentVideoSubtitles[idx])) {
        g_CurrentSubtitleIndexHint = idx;
        return (ptrdiff_t)idx;
    }

    if (idx + 1 < count && isActiveAt(g_CurrentVideoSubtitles[idx + 1])) {
        g_CurrentSubtitleIndexHint = idx + 1;
        return (ptrdiff_t)(idx + 1);
    }

    if (idx > 0 && isActiveAt(g_CurrentVideoSubtitles[idx - 1])) {
        g_CurrentSubtitleIndexHint = idx - 1;
        return (ptrdiff_t)(idx - 1);
    }

    auto it = std::upper_bound(
        g_CurrentVideoSubtitles.begin(), g_CurrentVideoSubtitles.end(),
        currentTime, [](double value, const VideoSubtitle& sub) {
            return value < sub.startTime;
        });
    if (it == g_CurrentVideoSubtitles.begin()) {
        return -1;
    }

    idx = (size_t)(it - g_CurrentVideoSubtitles.begin() - 1);
    if (isActiveAt(g_CurrentVideoSubtitles[idx])) {
        g_CurrentSubtitleIndexHint = idx;
        return (ptrdiff_t)idx;
    }

    return -1;
}

// =============================================================================
// Hooked Functions
// =============================================================================

BINK* Hooked_BinkOpen(const char* filename, uint32_t flags) {
    // Call original FIRST, then do our work
    BINK* result = Original_BinkOpen(filename, flags);
    
    if (result && filename) {
        std::string openedVideoName;
        const long long openQpc = QueryQpcNow();
        // Minimal processing - avoid locks during critical path
        {
            std::lock_guard<std::mutex> stateLock(g_StateMutex);
            g_CurrentVideoName = ExtractVideoName(filename);
            g_CurrentVideoLookupName = CanonicalizeVideoName(g_CurrentVideoName);
            g_CurrentBink = result;
            g_CurrentBinkAtomic.store(result, std::memory_order_relaxed);
            g_VideoPlaying = true;
            g_CutsceneVideoPlaying = false;
            g_NativeSubtitleSuppressed = false;
            g_VideoStartTime = std::chrono::steady_clock::now();
            g_StartFrame = 1;
            g_SubtitleRestorePending = false;
            g_SubtitleRestoreQueuedTick = 0;
            const unsigned long long sessionId =
                g_VideoSessionCounter.fetch_add(1, std::memory_order_relaxed) + 1;
            g_ActiveVideoSessionId.store(sessionId, std::memory_order_relaxed);
            g_CurrentVideoDoFrameCalls.store(0, std::memory_order_relaxed);
            g_CurrentVideoNextFrameCalls.store(0, std::memory_order_relaxed);
            g_CurrentVideoWaitCalls.store(0, std::memory_order_relaxed);
            g_CurrentVideoNativeTickCalls.store(0, std::memory_order_relaxed);
            g_NativeAnchorQpc.store(openQpc, std::memory_order_relaxed);
            g_NativeLastQpc.store(openQpc, std::memory_order_relaxed);
            openedVideoName = g_CurrentVideoName;
        }
        
        // DON'T access BINK struct members - layout is unknown and causes crashes!
        // Just log the video name
        ::g_lastBinkVideoOpenTick.store(GetTickCount(), std::memory_order_relaxed);
        LogToFile("[BinkHook] Video opened: " + openedVideoName);

        bool cutsceneVideo = false;
        size_t cutsceneSubtitleCount = 0;
        {
            std::scoped_lock lock(g_StateMutex, g_SubtitleMutex);
            RebuildCurrentVideoSubtitleCache_NoLock();
            cutsceneVideo = g_CutsceneVideoPlaying;
            cutsceneSubtitleCount = g_CurrentVideoSubtitles.size();
            if (cutsceneVideo) {
                g_NativeSubtitleSuppressed = true;
            }
        }

        if (cutsceneVideo) {
            DisableNativeSubtitles();
        } else {
            LogToFile("[BinkHook] Raw Bink session ignored for cutscene gating: " +
                      openedVideoName);
        }

        LogToFile("[BinkHook] Loaded " + std::to_string(cutsceneSubtitleCount) + 
                  " subtitles for video: " + openedVideoName);
    }
    
    return result;
}

void Hooked_BinkClose(BINK* bink) {
    // Call original FIRST
    Original_BinkClose(bink);

    // Then do cleanup
    bool matchedCurrent = false;
    bool restoreSuppressedSubtitles = false;
    std::string closedVideoName;
    {
        std::scoped_lock lock(g_StateMutex, g_SubtitleMutex);
        if (bink == g_CurrentBink) {
            matchedCurrent = true;
            closedVideoName = g_CurrentVideoName;
            restoreSuppressedSubtitles = g_NativeSubtitleSuppressed;
            g_VideoPlaying = false;
            g_CutsceneVideoPlaying = false;
            g_CurrentVideoName.clear();
            g_CurrentVideoLookupName.clear();
            g_CurrentBink = nullptr;
            g_CurrentBinkAtomic.store(nullptr, std::memory_order_relaxed);
            g_CurrentVideoSubtitles.clear();
            g_CurrentSubtitleIndexHint = 0;
            g_NativeSubtitleSuppressed = false;
            g_SubtitleRestorePending = restoreSuppressedSubtitles;
            g_SubtitleRestoreQueuedTick =
                restoreSuppressedSubtitles ? GetTickCount() : 0;
            g_ActiveVideoSessionId.store(0, std::memory_order_relaxed);
            g_CurrentVideoNativeTickCalls.store(0, std::memory_order_relaxed);
            g_NativeAnchorQpc.store(0, std::memory_order_relaxed);
            g_NativeLastQpc.store(0, std::memory_order_relaxed);
        }
    }

    if (matchedCurrent) {
        ::g_lastBinkVideoCloseTick.store(GetTickCount(), std::memory_order_relaxed);
        ::g_presentFrameAtLastVideoClose.store(
            ::g_presentFrameCount.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        LogToFile("[BinkHook] Video closed: " + closedVideoName);
    }
}

int Hooked_BinkDoFrame(BINK* bink) {
    g_DoFrameCalls.fetch_add(1, std::memory_order_relaxed);
    if (bink && bink == g_CurrentBinkAtomic.load(std::memory_order_relaxed)) {
        g_CurrentVideoDoFrameCalls.fetch_add(1, std::memory_order_relaxed);
        RecordNativeTimelineTickForCurrentVideo();
    }
    if (Original_BinkDoFrame) {
        return Original_BinkDoFrame(bink);
    }
    return 0;
}

void Hooked_BinkNextFrame(BINK* bink) {
    g_NextFrameCalls.fetch_add(1, std::memory_order_relaxed);
    if (bink && bink == g_CurrentBinkAtomic.load(std::memory_order_relaxed)) {
        g_CurrentVideoNextFrameCalls.fetch_add(1, std::memory_order_relaxed);
        RecordNativeTimelineTickForCurrentVideo();
    }
    if (Original_BinkNextFrame) {
        Original_BinkNextFrame(bink);
    }
}

int Hooked_BinkWait(BINK* bink) {
    g_WaitCalls.fetch_add(1, std::memory_order_relaxed);
    if (bink && bink == g_CurrentBinkAtomic.load(std::memory_order_relaxed)) {
        g_CurrentVideoWaitCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (Original_BinkWait) {
        return Original_BinkWait(bink);
    }
    return 0;
}

// =============================================================================
// Public API
// =============================================================================

bool Init() {
    // Use IAT Hook instead of Detour - more stable for Bink DLL
    HMODULE gameExe = GetModuleHandleA(NULL);  // Main exe
    
    if (!gameExe) {
        LogToFile("[BinkHook] ERROR: Could not get game exe handle!");
        return false;
    }
    
    LogToFile("[BinkHook] Attempting IAT hook on game exe for bink2w64.dll imports...");
    
    // First, get original function pointers from the DLL
    HMODULE binkDll = GetModuleHandleA("bink2w64.dll");
    if (!binkDll) {
        binkDll = LoadLibraryW(L"bink2w64.dll");
    }
    
    if (binkDll) {
        LogToFile("[BinkHook] Found bink2w64.dll at: " + std::to_string((uintptr_t)binkDll));
    }

    const long long qpcFreq = GetOrInitQpcFrequency();
    if (qpcFreq > 0) {
        LogToFile("[BinkHook] QPC frequency initialized: " + std::to_string(qpcFreq));
    } else {
        LogToFile("[BinkHook] WARNING: QueryPerformanceFrequency failed (native timeline disabled)");
    }
    
    // Try IAT hook on the game exe
    void* origOpen = nullptr;
    void* origClose = nullptr;
    void* origDoFrame = nullptr;
    void* origNextFrame = nullptr;
    void* origWait = nullptr;
    
    bool hooked = false;
    
    // IAT Hook BinkOpen
    if (IATHook(gameExe, "bink2w64.dll", "BinkOpen", (void*)Hooked_BinkOpen, &origOpen)) {
        Original_BinkOpen = (BinkOpen_t)origOpen;
        g_HookOpenInstalled.store(true, std::memory_order_relaxed);
        LogToFile("[BinkHook] IAT Hook BinkOpen SUCCESS!");
        hooked = true;
    } else {
        LogToFile("[BinkHook] IAT Hook BinkOpen failed (might use delay load or ordinal)");
    }
    
    // IAT Hook BinkClose
    if (IATHook(gameExe, "bink2w64.dll", "BinkClose", (void*)Hooked_BinkClose, &origClose)) {
        Original_BinkClose = (BinkClose_t)origClose;
        g_HookCloseInstalled.store(true, std::memory_order_relaxed);
        LogToFile("[BinkHook] IAT Hook BinkClose SUCCESS!");
        hooked = true;
    } else {
        LogToFile("[BinkHook] IAT Hook BinkClose failed");
    }

    // Native timeline callbacks + verification probes
    if (IATHook(gameExe, "bink2w64.dll", "BinkDoFrame", (void*)Hooked_BinkDoFrame, &origDoFrame)) {
        Original_BinkDoFrame = (BinkDoFrame_t)origDoFrame;
        g_HookDoFrameInstalled.store(true, std::memory_order_relaxed);
        LogToFile("[BinkHook] IAT Hook BinkDoFrame SUCCESS!");
    } else {
        LogToFile("[BinkHook] IAT Hook BinkDoFrame failed");
    }

    if (IATHook(gameExe, "bink2w64.dll", "BinkNextFrame", (void*)Hooked_BinkNextFrame, &origNextFrame)) {
        Original_BinkNextFrame = (BinkNextFrame_t)origNextFrame;
        g_HookNextFrameInstalled.store(true, std::memory_order_relaxed);
        LogToFile("[BinkHook] IAT Hook BinkNextFrame SUCCESS!");
    } else {
        LogToFile("[BinkHook] IAT Hook BinkNextFrame failed");
    }

    if (IATHook(gameExe, "bink2w64.dll", "BinkWait", (void*)Hooked_BinkWait, &origWait)) {
        Original_BinkWait = (BinkWait_t)origWait;
        g_HookWaitInstalled.store(true, std::memory_order_relaxed);
        LogToFile("[BinkHook] IAT Hook BinkWait SUCCESS!");
    } else {
        LogToFile("[BinkHook] IAT Hook BinkWait failed");
    }
    
    if (!hooked) {
        LogToFile("[BinkHook] WARNING: No IAT hooks applied. Game may use dynamic loading.");
        // Don't return false - we'll try alternative detection later
    }
    
    return true;
}

void Shutdown() {
    // Hooks will be cleaned up when DLL unloads
    LogToFile("[BinkHook] Shutdown");
}

bool IsVideoPlaying() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    return g_VideoPlaying;
}

bool IsCutsceneVideoPlaying() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    return g_CutsceneVideoPlaying;
}

std::string GetCurrentVideoName() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    return g_CurrentVideoName;
}

double GetCurrentPlaybackTime() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    
    if (!g_VideoPlaying) {
        return 0.0;
    }

    return GetPlaybackTimeSeconds_NoLock();
}

unsigned int GetCurrentFrame() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    if (!g_VideoPlaying) {
        return 0;
    }

    unsigned long long frameApprox =
        g_CurrentVideoNextFrameCalls.load(std::memory_order_relaxed);
    if (frameApprox == 0) {
        frameApprox = g_CurrentVideoDoFrameCalls.load(std::memory_order_relaxed);
    }
    if (frameApprox > 0xFFFFFFFFull) {
        frameApprox = 0xFFFFFFFFull;
    }
    return (unsigned int)frameApprox;
}

bool LoadSubtitlesCSV(const std::string& csvPath) {
    std::string csvText;
    if (!ReadTextUtf8(csvPath, csvText)) {
        LogToFile("[BinkHook] ERROR: Could not open subtitles CSV: " + csvPath);
        return false;
    }

    std::istringstream file(csvText);
    
    std::lock_guard<std::mutex> lock(g_SubtitleMutex);
    g_AllSubtitles.clear();
    g_KnownSubtitleVideoNames.clear();
    
    std::string line;
    int lineNum = 0;
    
    while (std::getline(file, line)) {
        lineNum++;
        
        // Skip empty lines
        if (line.empty()) continue;
        
        // Skip header row if present
        if (lineNum == 1) {
            std::string header = line;
            std::transform(header.begin(), header.end(), header.begin(), ::tolower);
            if (header.find("videoname") == 0) {
                continue;
            }
        }

        // Parse CSV: videoName,startTime,endTime,text
        std::stringstream ss(line);
        std::string videoName, startStr, endStr, text;
        
        if (!std::getline(ss, videoName, ',')) continue;
        if (!std::getline(ss, startStr, ',')) continue;
        if (!std::getline(ss, endStr, ',')) continue;
        if (!std::getline(ss, text)) continue;
        
        // Remove any remaining text after the text field (shouldn't happen)
        // The text field contains the rest of the line
        
        VideoSubtitle sub;
        // Normalize video name to lowercase for matching
        std::transform(videoName.begin(), videoName.end(), videoName.begin(), ::tolower);
        sub.videoName = CanonicalizeVideoName(videoName);
        
        try {
            sub.startTime = std::stod(startStr);
            sub.endTime = std::stod(endStr);
        } catch (...) {
            LogToFile("[BinkHook] WARNING: Invalid time values at line " + std::to_string(lineNum));
            continue;
        }
        
        sub.englishText = text;
        sub.koreanText = "";  // Will be filled from translation map
        
        g_AllSubtitles.push_back(sub);
        if (!sub.videoName.empty()) {
            g_KnownSubtitleVideoNames.insert(sub.videoName);
        }
    }
    
    ApplyTranslationsToSubtitles();
    LogToFile("[BinkHook] Loaded " + std::to_string(g_AllSubtitles.size()) + " subtitle entries from CSV");

    bool rawVideoPlaying = false;
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        rawVideoPlaying = g_VideoPlaying;
    }
    if (rawVideoPlaying) {
        bool shouldDisableNativeSubtitles = false;
        {
            std::scoped_lock lock(g_StateMutex, g_SubtitleMutex);
            RebuildCurrentVideoSubtitleCache_NoLock();
            if (g_CutsceneVideoPlaying && !g_NativeSubtitleSuppressed) {
                g_NativeSubtitleSuppressed = true;
                shouldDisableNativeSubtitles = true;
            }
        }
        if (shouldDisableNativeSubtitles) {
            DisableNativeSubtitles();
        }
    }
    
    return true;
}

std::string GetCurrentSubtitle() {
    std::lock_guard<std::mutex> lock(g_StateMutex);

    if (!g_VideoPlaying || !g_CutsceneVideoPlaying) {
        return "";
    }

    double currentTime = GetPlaybackTimeSeconds_NoLock();

    std::lock_guard<std::mutex> subLock(g_SubtitleMutex);

    const ptrdiff_t subIdx = FindActiveSubtitleIndex_NoLock(currentTime);
    if (subIdx >= 0) {
        const auto& sub = g_CurrentVideoSubtitles[(size_t)subIdx];
        // Return Korean if available, otherwise English
        if (!sub.koreanText.empty()) {
            return sub.koreanText;
        }
        return sub.englishText;
    }

    return "";
}

std::string GetCurrentEnglishSubtitle() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    
    if (!g_VideoPlaying || !g_CutsceneVideoPlaying) {
        return "";
    }

    double currentTime = GetPlaybackTimeSeconds_NoLock();
    
    std::lock_guard<std::mutex> subLock(g_SubtitleMutex);

    const ptrdiff_t subIdx = FindActiveSubtitleIndex_NoLock(currentTime);
    if (subIdx >= 0) {
        return g_CurrentVideoSubtitles[(size_t)subIdx].englishText;
    }
    
    return "";
}

bool ShouldBridgeCurrentSubtitleGap(unsigned int holdMs) {
    std::scoped_lock lock(g_StateMutex, g_SubtitleMutex);

    if (!g_VideoPlaying || !g_CutsceneVideoPlaying ||
        g_CurrentVideoSubtitles.empty() || holdMs == 0) {
        return false;
    }

    double currentTime = GetPlaybackTimeSeconds_NoLock();

    // If a subtitle is currently active, there is no gap to bridge.
    if (FindActiveSubtitleIndex_NoLock(currentTime) >= 0) {
        return false;
    }

    const auto nextIt = std::lower_bound(
        g_CurrentVideoSubtitles.begin(), g_CurrentVideoSubtitles.end(),
        currentTime, [](const VideoSubtitle& sub, double value) {
            return sub.startTime < value;
        });
    if (nextIt == g_CurrentVideoSubtitles.begin() ||
        nextIt == g_CurrentVideoSubtitles.end()) {
        return false;
    }

    const auto& prev = *(nextIt - 1);
    const auto& next = *nextIt;

    const double holdSec = (double)holdMs / 1000.0;
    const double sincePrevEnd = currentTime - prev.endTime;
    const double untilNextStart = next.startTime - currentTime;
    if (sincePrevEnd < 0.0 || sincePrevEnd > holdSec) {
        return false;
    }
    if (untilNextStart < 0.0 || untilNextStart > holdSec) {
        return false;
    }

    const std::string prevText = !prev.koreanText.empty()
                                     ? prev.koreanText
                                     : prev.englishText;
    const std::string nextText = !next.koreanText.empty()
                                     ? next.koreanText
                                     : next.englishText;
    if (prevText.empty() || nextText.empty()) {
        return false;
    }

    return NormalizeForMatch(prevText) == NormalizeForMatch(nextText);
}

void PumpDeferredWork() {
    bool shouldRestoreSubtitles = false;
    bool videoPlaying = false;
    std::string videoName;
    double steadyElapsedSec = 0.0;
    double playbackElapsedSec = 0.0;
    bool usingNativeTimeline = false;
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        if (!g_VideoPlaying && g_SubtitleRestorePending) {
            const DWORD now = GetTickCount();
            constexpr DWORD kDeferredRestoreDelayMs = 120;
            if (g_SubtitleRestoreQueuedTick == 0 ||
                (now - g_SubtitleRestoreQueuedTick) >= kDeferredRestoreDelayMs) {
                g_SubtitleRestorePending = false;
                g_SubtitleRestoreQueuedTick = 0;
                shouldRestoreSubtitles = true;
            }
        }
        videoPlaying = g_VideoPlaying;
        if (videoPlaying) {
            videoName = g_CurrentVideoName;
            auto nowSteady = std::chrono::steady_clock::now();
            steadyElapsedSec =
                std::chrono::duration<double>(nowSteady - g_VideoStartTime).count();
            playbackElapsedSec = GetPlaybackTimeSeconds_NoLock(&usingNativeTimeline);
        }
    }

    if (shouldRestoreSubtitles) {
        EnableNativeSubtitles();
        LogToFile("[BinkHook] Deferred subtitle restore applied");
    }

    EmitNativeTimelineDiagnostics(videoPlaying, videoName, steadyElapsedSec,
                                  playbackElapsedSec, usingNativeTimeline);
}

// =============================================================================
// Korean Translation Loading
// =============================================================================

// This should be called after loading localize_kr.json
void LoadKoreanTranslations(const std::unordered_map<std::string, std::string>& translations) {
    std::lock_guard<std::mutex> lock(g_SubtitleMutex);
    
    g_EnglishToKorean = translations;
    ApplyTranslationsToSubtitles();
}

// Load Korean translations by key (VIDSUBTITLES_{VIDEO_NAME}_VIDEO{N})
// This matches subtitles by video name and index order from subtitles.csv
void LoadKoreanByKey(const std::unordered_map<std::string, std::string>& keyToKorean) {
    std::lock_guard<std::mutex> lock(g_SubtitleMutex);
    
    if (g_AllSubtitles.empty()) {
        LogToFile("[BinkHook] WARNING: No subtitles loaded, cannot apply Korean translations");
        return;
    }
    
    // Group subtitles by video name to determine index
    std::unordered_map<std::string, int> videoSubIndex;
    int matched = 0;
    
    for (auto& sub : g_AllSubtitles) {
        // Get current index for this video (1-based)
        int& idx = videoSubIndex[sub.videoName];
        idx++;
        
        // Build key: VIDSUBTITLES_{VIDEO_NAME_UPPER}_VIDEO{N}
        std::string videoUpper = sub.videoName;
        std::transform(videoUpper.begin(), videoUpper.end(), videoUpper.begin(), ::toupper);
        
        std::string key = "VIDSUBTITLES_" + videoUpper + "_VIDEO" + std::to_string(idx);
        
        auto it = keyToKorean.find(key);
        if (it != keyToKorean.end()) {
            sub.koreanText = it->second;
            matched++;
        } else {
            // Try alternate key formats (some might have different numbering)
            // e.g., VIDEO1, VIDEO01, VIDEO_1
            bool found = false;
            
            // Try VIDEO01 format
            char altKey[256];
            sprintf_s(altKey, "VIDSUBTITLES_%s_VIDEO%02d", videoUpper.c_str(), idx);
            auto altIt = keyToKorean.find(altKey);
            if (altIt != keyToKorean.end()) {
                sub.koreanText = altIt->second;
                matched++;
                found = true;
            }
            
            if (!found) {
                sub.koreanText.clear();
            }
        }
    }
    
    LogToFile("[BinkHook] Matched " + std::to_string(matched) + "/" +
              std::to_string(g_AllSubtitles.size()) +
              " subtitles with Korean by key");
    
    // Log first few matches for debugging
    int logCount = 0;
    for (const auto& sub : g_AllSubtitles) {
        if (!sub.koreanText.empty() && logCount < 5) {
            logCount++;
            size_t maxLen = sub.koreanText.size() < 50 ? sub.koreanText.size() : 50;
            LogToFile("[BinkHook] Sample: " + sub.videoName + " -> " + 
                      sub.koreanText.substr(0, maxLen));
        }
    }
}

void LoadSubtitlesFromEntries(const std::vector<VideoSubtitle>& entries) {
    std::vector<VideoSubtitle> normalizedEntries = entries;
    for (auto& entry : normalizedEntries) {
        entry.videoName = CanonicalizeVideoName(entry.videoName);
    }

    {
        std::lock_guard<std::mutex> lock(g_SubtitleMutex);
        g_AllSubtitles = std::move(normalizedEntries);
        g_KnownSubtitleVideoNames.clear();
        for (const auto& entry : g_AllSubtitles) {
            if (!entry.videoName.empty()) {
                g_KnownSubtitleVideoNames.insert(entry.videoName);
            }
        }
        ApplyTranslationsToSubtitles();
    }

    // Refresh current video cache if a video is already playing
    bool rawVideoPlaying = false;
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        rawVideoPlaying = g_VideoPlaying;
    }
    if (rawVideoPlaying) {
        bool shouldDisableNativeSubtitles = false;
        {
            std::scoped_lock lock(g_StateMutex, g_SubtitleMutex);
            RebuildCurrentVideoSubtitleCache_NoLock();
            if (g_CutsceneVideoPlaying && !g_NativeSubtitleSuppressed) {
                g_NativeSubtitleSuppressed = true;
                shouldDisableNativeSubtitles = true;
            }
        }
        if (shouldDisableNativeSubtitles) {
            DisableNativeSubtitles();
        }
    }
}

} // namespace BinkHook
