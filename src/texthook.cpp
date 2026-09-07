#include "TextHook.h"
#include "HudClassify.h"

#include "D3D11Hook.h"
#include "GameViewport.h"
#include "IW6Font.h"

#include "BinkHook.h"
#include "IW6Offsets.h"
#include "GameBuild.h"

#include "KoreanRenderer.h"
#include "BindingResolver.h"
#include "ObjectiveRuntime.h"
#include "ObjectiveRendererUnified.h"
#include "TranslationStore.h"

#include "NativeSubtitleTracker.h"
#include "GamepadGlyphAtlas.h"

#include "Utils.h"
#include "vendor/minhook/include/MinHook.h"

#include <atomic>

#include <cstring>

#include <iomanip>

#include <mutex>

#include <shared_mutex>

#include <sstream>

#include <string>

#include <map>
#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <deque>
#include <intrin.h>
#include <limits>

static std::string GetModuleDirectory() {
  std::string path = GetModuleDirPath(nullptr);
  if (path.empty()) {
    return ".";
  }
  return path;
}

// Forward declaration (defined later in this file)
static std::string StripColorCodes(const std::string &s);
static std::string NormalizeEnglishKey(const std::string &s);
static void LoadTranslations();
static void EnsureTranslationsLoaded();
static bool ResolveKeyFromEnglishInternal(const std::string &english,
                                          std::string &outKey);
static bool IsGameplayHudKeyName(const std::string &name,
                                 std::string *outAutoReason = nullptr);
static bool HasObjectiveSuffixOnlyKey(const std::string &key);
static bool IsObjectiveUpdateKeyName(const std::string &key);
static bool IsObjectiveOverlayKeyName(const std::string &key);
static bool IsObjectiveStatusMessageKeyName(const std::string &key);
static bool IsObjectiveListBootstrapKeyName(const std::string &key);
static const char *GetObjectiveStatusKoreanFallback(const std::string &key);
static void TextHook_RecordObjectiveStatusCfgTrust(const std::string &key,
                                                   unsigned int sourceToken,
                                                   unsigned long now,
                                                   const char *sourceTag = nullptr);
static bool IsDirectAddCmdObjHudHintKey(const std::string &key);
static bool IsHudRuntimeTailJoinPromptKey(const std::string &key);
static bool IsScannerHintAuthorityKey(const std::string &key);
static bool IsMovementTutorialHintKey(const std::string &key);
static bool HasRecentObjectiveObservationForKey(const std::string &key,
                                                DWORD maxAgeMs);
static void CaptureHudRuntimeTailFragment(const std::string &key,
                                          const std::string &rawText, float x,
                                          float y, unsigned int callerOffset,
                                          bool trustedCaller,
                                          bool gameplayContext);
static ObjectiveChannel DetermineObjectiveChannelForKey(const std::string &key,
                                                        bool pauseMenuLikely);
static ObjectiveChannel
DetermineObjectiveChannelForOwnerdraw(unsigned short ownerdrawId,
                                      ObjectiveChannel fallback);
static void RefreshHintEntryNative(const std::string &key,
                                   const HudNativeEntry &native,
                                   const std::string *englishRuntime = nullptr);
static void RefreshObjHudHintEntryNative(const std::string &key,
                                         const HudNativeEntry &native,
                                         const std::string *englishRuntime = nullptr);
static void RefreshObjectiveOverlayEntry(
    const std::string &key, ObjectiveChannel channel = OBJ_CHANNEL_NONE,
    const std::string *englishRuntime = nullptr);
static uint64_t NextObjectiveRuntimeInstanceId();
static uint32_t NextObjectiveGeneration();
static void LogCodeBytes(const char *tag, uintptr_t addr, int len);
static void LogRelativeCalls(const char *tag, uintptr_t addr, int len);
static void SubRev_LogRipDetailsOnce(uintptr_t rip, uintptr_t ripOff);
static void SubRev_LogFloat4Candidates(const char *tag, uintptr_t addr,
                                       int scanBytes, int limit);
struct SubtitleRuntimeTiming;
static void UpdateSubtitleRuntimeTiming(const std::string &key, int windowId,
                                        int msgTimeMs, int fadeOutMs,
                                        int arg6);
static bool TryGetSubtitleRuntimeTiming(const std::string &key,
                                        SubtitleRuntimeTiming &out,
                                        DWORD maxAgeMs = 2000);
static void UpdateSubtitleRuntimeTimingByText(const char *localizedText,
                                              int windowId, int msgTimeMs,
                                              int fadeOutMs, int arg6);
static bool TryGetSubtitleRuntimeTimingByText(const std::string &normText,
                                              SubtitleRuntimeTiming &out,
                                              DWORD maxAgeMs = 2000);
static bool ResolveSubtitleKeyFromLocalizedText(const char *localizedText,
                                                std::string &outKey);
static void ApplyRuntimeTimingToSubtitleQueue(const std::string &key,
                                              int msgTimeMs, int fadeOutMs,
                                              int arg6, DWORD tick);
static void ResetSubtitleStateForMapRestart();
static void SubtitleReverse_OnEnqueueInstance(const std::string &key,
                                              const char *localizedText,
                                              int msgTimeMs, int fadeOutMs,
                                              DWORD enqueueTick);
static void SubtitleReverse_PollLocked(DWORD now);
static void SubtitleReverse_Reset();
static void SubtitleReverse_Shutdown();
static std::string ToUpper(const std::string &s);
static bool TryReadCStringPreview(const char *s, size_t maxLen,
                                  std::string &out);
static bool LooksLikePressHoldPrompt(const std::string &text);
static bool HasBindingPlaceholderToken(const std::string &text);
static bool IsExcludedHudAutoPrefix(const std::string &name);
static void MaybeLogHudKeyAuto(const std::string &key, uintptr_t callerOffset,
                               const std::string &reason);
static bool LooksLikeHudLocalizationKey(const std::string &text);
static void EnsureHudConsumerSeedsLoaded();
static bool TryGetHudSeedChannelForKey(const std::string &key,
                                       HudRouteChannel &outChannel);
static bool TryGetHudSeedChannelForConsumer(OverlaySource source,
                                            unsigned int callerOffset,
                                            int slotBucket,
                                            HudRouteChannel &outChannel);
static bool HasMixedHudHintPromptCallers(const std::string &key,
                                         unsigned int currentCallerOffset,
                                         DWORD maxAgeMs);
static bool IsHudStrictConsumerAuthorityEnabled();
static void SaveMissingSubtitles();
void TextHook_ClearHudNativeEntries();
void TextHook_ClearSubtitleQueue();
void TextHook_ResetObjectiveRuntimeForMapRestart();
static bool ObjRev_DecodeCfgBaseFromCaller(uint32_t queryIdx, uintptr_t retAddr,
                                           uint32_t &outCfgIdx,
                                           uint32_t &outBias);
static void ObjRev_RecordDirectCfgSlc(uint32_t cfgIdx, uint32_t bias,
                                      uint32_t slcIdx, uintptr_t callerOffset);
static bool ObjRev_GetDirectCfgSlc(uint32_t cfgIdx, uint32_t &outSlcIdx,
                                   uint32_t &outBias,
                                   bool preferObjective = false);
static bool ObjRev_ResolveObjectiveBias(uint32_t &outBias);
static std::string ObjRev_ReadSlcStringRaw(uint32_t slcIdx);
static std::string ObjRev_ResolveConfigKey(uint32_t cfgIdx,
                                           bool preferObjective,
                                           bool directOnly);
static bool ObjRev_IsPlausibleConfigIndex(uint32_t cfgIdx);
typedef unsigned int(__fastcall *ObjRev_ConfigIndexToSlcFn)(unsigned int cfgIndex);
static ObjRev_ConfigIndexToSlcFn ObjRev_GetConfigIndexToSlcFn();
static void ObjRev_ScanIndexLookupTablesOnce();
static void ObjRev_UpdateActiveObjectiveCfg(
    const std::unordered_set<uint32_t> &cfgSet);
static void ObjRev_MarkObjectiveCfgActive(uint32_t cfgIdx, unsigned long now);
static bool ObjRev_IsObjectiveCfgActive(uint32_t cfgIdx, unsigned long now);
static int ObjRev_ResolveHudElemArrayIndex(uintptr_t elemPtr);
static bool ObjRev_LooksLikeLocalizationKey(const char *s);
static uint64_t ObjRev_MakeAuthoritativeInstanceKey(uint32_t cfgIndex,
                                                    int fxBirth,
                                                    int elemIndex);
static void ObjRev_RecordAuthoritativeObjectiveCapture(
    const std::string &key, uint32_t cfgText, uint32_t cfgLabel,
    uint32_t slcIndex, int fxBirth, uintptr_t elemPtr, uintptr_t callerOffset,
    bool publishOverlayEvent);
static void ObjRev_RecordAuthoritativeObjectiveCapture(
    const std::string &key, uint32_t cfgText, uint32_t cfgLabel, int fxBirth,
    uintptr_t elemPtr, uintptr_t callerOffset, bool publishOverlayEvent);
static bool ObjRev_GetAuthoritativeObjectiveKey(uint32_t cfgIndex, int fxBirth,
                                                int elemIndex,
                                                std::string &outKey,
                                                const char **outSource = nullptr);
static void ObjRev_EnqueuePendingObjectiveAuthorityEvent(
    const std::string &key, uint32_t slcIndex, uintptr_t callerOffset,
    uint32_t ordinal);
static bool ObjRev_ConsumePendingObjectiveAuthorityForLane(
    uint32_t cfgIndex, int fxBirth, int elemIndex, uintptr_t elemPtr,
    float laneY, uint32_t *outSlcIndex = nullptr);
static void ObjRev_ClearPendingObjectiveAuthorityEvents();
static void ObjRev_InvalidateAuthoritativeInstance(uint32_t cfgIndex,
                                                   int fxBirth, int elemIndex,
                                                   const char *reason);
static void ObjRev_InvalidateAuthoritativeCfg(uint32_t cfgIndex,
                                              const char *reason);
struct ObjConsumerResolveEvent;
static void ObjRev_RecordObjectiveConsumerResolve(uint32_t cfgIdx,
                                                  uint32_t bias,
                                                  uint32_t queryIdx,
                                                  uint32_t slcIdx,
                                                  uintptr_t cfgCallerOffset,
                                                  const char *sourceTag);
static bool ObjRev_GetRecentObjectiveConsumerResolve(
    uint32_t cfgIdx, DWORD maxAgeMs, ObjConsumerResolveEvent &outEvent);
static uintptr_t ObjRev_GetObjectiveAuthoritySlcCallerOffset();
static uintptr_t ObjRev_GetObjectiveAuthorityTypeCheckCallerOffset();
static bool ObjRev_IsObjectiveAuthoritySlcCaller(uintptr_t callerOffset);
static bool ObjRev_IsObjectiveAuthorityTypeCheckCaller(uintptr_t callerOffset);
static void ObjRev_UpdateObjectiveAuthorityCallers(uintptr_t typeCheckCaller,
                                                   uintptr_t slcCaller,
                                                   const char *reason);
static void ObjRev_UpdateObjectiveAuthoritySlcCaller(uintptr_t slcCaller,
                                                     const char *reason);
static void TextHook_OSAuth_RecordObjectiveProducerCapture(
    const std::string &key, uint32_t cfgIndex, uint32_t slcIndex, int fxBirth,
    uint64_t elemPtrOrIndex, unsigned int callerOffset,
    unsigned int calleeOffset);
static void TextHook_OSAuth_RecordStatusProducerCapture(
    const std::string &key, uint32_t cfgIndex, uint32_t slcIndex, int laneSlot,
    unsigned int callerOffset, uint64_t producerInstanceSig, int fxBirth,
    uint64_t elemPtrOrIndex, unsigned int calleeOffset);
static bool
TextHook_OSAuth_IsObjectiveCaptureCandidateKey(const std::string &key);
static void TextHook_OSAuth_RecordIntroCapture(
    const std::string &key, const std::string &rawText, uint32_t cfgIndex,
    uint32_t slcIndex, int fxBirth, int fxLetter, uint64_t elemPtrOrIndex,
    float x, float y, float scale, float fontHeight, uint32_t colorPacked,
    unsigned short virtualW, unsigned short virtualH, unsigned int callerOffset,
    unsigned int calleeOffset);
static bool TextHook_OSRev_LooksLikePromptFragment(const std::string &text);
static bool TextHook_OSRev_IsPromptLikeLocalizedKey(
    const std::string &key);
static bool TextHook_OSRev_IsObjectiveStatusKey(const std::string &key);
static bool TextHook_OSRev_IsNativeObjectiveCandidateKey(
    const std::string &key);
static bool TextHook_OSRev_ResolveKeyFromText(const std::string &text,
                                              std::string &outKey);
// OSREV data function forward declarations removed — OSAUTH is the sole authority.

// =============================================================================
// Objective SLC Observations (from SLC hook caller +0x34E25E)
// Used by ReadLiveHudElems to discover the HudElem-text configstring→SLC base.
// =============================================================================
struct ObjSlcObservation {
  uint32_t slcIdx; // stringValue from SLC hook (the SLC index)
  std::string key; // resolved key name (e.g., "CORNERED_OBJ_CONFIRM_ID")
  uint32_t ordinal; // rdi at +0x34E25E (1-based objective slot: 1→y=10, 2→y=30)
  DWORD tick; // capture time (GetTickCount) for recency-aware joins
};
static std::mutex g_objSlcObsMutex;
static std::vector<ObjSlcObservation> g_objSlcObservations;
static std::atomic<uint32_t> g_objSlcDirectBase{0};
// Build-stable objective producer caller pair for this binary.
static std::atomic<uintptr_t> g_objAuthoritySlcCallerOffset{IW6Offsets::Profile::Rva_232562};
static std::atomic<uintptr_t> g_objAuthorityTypeCheckCallerOffset{0};

static uintptr_t ObjRev_GetObjectiveAuthoritySlcCallerOffset() {
  return g_objAuthoritySlcCallerOffset.load(std::memory_order_relaxed);
}

static uintptr_t ObjRev_GetObjectiveAuthorityTypeCheckCallerOffset() {
  return g_objAuthorityTypeCheckCallerOffset.load(std::memory_order_relaxed);
}

static bool ObjRev_IsObjectiveAuthoritySlcCaller(uintptr_t callerOffset) {
  return callerOffset != 0 &&
         callerOffset == ObjRev_GetObjectiveAuthoritySlcCallerOffset();
}

static bool ObjRev_IsObjectiveAuthorityTypeCheckCaller(uintptr_t callerOffset) {
  return callerOffset != 0 &&
         callerOffset == ObjRev_GetObjectiveAuthorityTypeCheckCallerOffset();
}

static void ObjRev_UpdateObjectiveAuthorityCallers(uintptr_t typeCheckCaller,
                                                   uintptr_t slcCaller,
                                                   const char *reason) {
  if (typeCheckCaller == 0 || slcCaller == 0) {
    return;
  }
  // Structural invariant in this path:
  // cmp/typecheck return site is immediately followed by SL_ConvertToString call.
  if ((typeCheckCaller + 0x0Eu) != slcCaller) {
    return;
  }

  const unsigned long now = GetTickCount();
  const uint64_t sig = ((uint64_t)(typeCheckCaller & 0xFFFFFu) << 20) ^
                       (uint64_t)(slcCaller & 0xFFFFFu);
  struct ObjAuthCallerEvidence {
    unsigned long tick = 0;
    unsigned int hits = 0;
  };
  static std::unordered_map<uint64_t, ObjAuthCallerEvidence> s_authCallerEvidence;
  ObjAuthCallerEvidence &ev = s_authCallerEvidence[sig];
  if (ev.tick == 0 || now < ev.tick || (now - ev.tick) > 1800) {
    ev.hits = 0;
  }
  ev.tick = now;
  if (ev.hits < 0xFFFFu) {
    ++ev.hits;
  }

  const uintptr_t prevSlc = ObjRev_GetObjectiveAuthoritySlcCallerOffset();
  const uintptr_t prevType = ObjRev_GetObjectiveAuthorityTypeCheckCallerOffset();
  if (prevSlc == slcCaller && prevType == typeCheckCaller) {
    return;
  }

  constexpr unsigned int kPromoteHits = 2;
  if (ev.hits < kPromoteHits) {
    static std::unordered_map<uint64_t, unsigned long> s_authCallerCandLogTick;
    auto itLog = s_authCallerCandLogTick.find(sig);
    if (itLog == s_authCallerCandLogTick.end() || (now - itLog->second) > 1500) {
      s_authCallerCandLogTick[sig] = now;
      char cbuf[256];
      sprintf_s(cbuf,
                "[OBJ-AUTH-CALLER-CAND] slc=+0x%llX type=+0x%llX hits=%u reason=%s",
                (unsigned long long)slcCaller, (unsigned long long)typeCheckCaller,
                ev.hits, reason ? reason : "unspecified");
      LogToFile(cbuf);
    }
    return;
  }

  g_objAuthorityTypeCheckCallerOffset.store(typeCheckCaller,
                                            std::memory_order_relaxed);
  g_objAuthoritySlcCallerOffset.store(slcCaller, std::memory_order_relaxed);

  char cbuf[256];
  sprintf_s(cbuf,
            "[OBJ-AUTH-CALLER-RESOLVE] slc=+0x%llX type=+0x%llX prevSlc=+0x%llX prevType=+0x%llX hits=%u reason=%s",
            (unsigned long long)slcCaller,
            (unsigned long long)typeCheckCaller,
            (unsigned long long)prevSlc,
            (unsigned long long)prevType, ev.hits,
            reason ? reason : "unspecified");
  LogToFile(cbuf);

  if (s_authCallerEvidence.size() > 64) {
    for (auto it = s_authCallerEvidence.begin(); it != s_authCallerEvidence.end();) {
      if (now >= it->second.tick && (now - it->second.tick) > 20000) {
        it = s_authCallerEvidence.erase(it);
      } else {
        ++it;
      }
    }
  }
}

static const std::array<unsigned int, 8> &TextHook_GetConfirmedStatusConsumerCallers() {
  // Confirmed status consumers from runtime logs (not objective authority callsites).
  static const std::array<unsigned int, 8> kCallers = {
      IW6Offsets::Profile::Rva_232562, IW6Offsets::Profile::Rva_18465B, IW6Offsets::Profile::Rva_18477E, IW6Offsets::Profile::Rva_389BF7,
      IW6Offsets::Profile::Rva_2454FC, 0, IW6Offsets::Profile::Rva_4156EC, IW6Offsets::Profile::Rva_4DAC50};
  return kCallers;
}

static bool TextHook_IsConfirmedStatusConsumerCaller(unsigned int callerOffset) {
  if (callerOffset == 0) {
    return false;
  }
  const auto &callers = TextHook_GetConfirmedStatusConsumerCallers();
  return std::find(callers.begin(), callers.end(), callerOffset) !=
         callers.end();
}

static bool TextHook_IsPrimaryStatusProducerCaller(unsigned int callerOffset) {
  return (callerOffset == IW6Offsets::Profile::Rva_232562 || callerOffset == IW6Offsets::Profile::Rva_4DAC50);
}

static bool TextHook_GetRecentObjectiveStatusProbeFromConsumers(
    unsigned long maxAgeMs, std::string &outKey, uint32_t *outSlc,
    bool *outHasRenderParams, unsigned int *outCallerOffset = nullptr) {
  outKey.clear();
  if (outSlc) {
    *outSlc = 0;
  }
  if (outHasRenderParams) {
    *outHasRenderParams = false;
  }
  if (outCallerOffset) {
    *outCallerOffset = 0;
  }

  bool found = false;
  bool foundWithParams = false;
  std::string bestKey;
  uint32_t bestSlc = 0;
  bool bestHasParams = false;
  unsigned int bestCaller = 0;
  for (unsigned int caller : TextHook_GetConfirmedStatusConsumerCallers()) {
    std::string key;
    uint32_t slc = 0;
    bool hasParams = false;
    if (!TextHook_GetRecentObjectiveStatusProbe(maxAgeMs, caller, key, &slc,
                                                &hasParams)) {
      continue;
    }
    if (!found || (hasParams && !foundWithParams)) {
      found = true;
      foundWithParams = hasParams;
      bestKey = key;
      bestSlc = slc;
      bestHasParams = hasParams;
      bestCaller = caller;
      if (foundWithParams) {
        break;
      }
    }
  }

  if (!found) {
    return false;
  }
  outKey = bestKey;
  if (outSlc) {
    *outSlc = bestSlc;
  }
  if (outHasRenderParams) {
    *outHasRenderParams = bestHasParams;
  }
  if (outCallerOffset) {
    *outCallerOffset = bestCaller;
  }
  return true;
}

static bool TextHook_GetRecentObjectiveStatusProbeByKeyFromConsumers(
    unsigned long maxAgeMs, const char *requiredKey, std::string &outKey,
    uint32_t *outSlc, bool *outHasRenderParams,
    unsigned int *outCallerOffset = nullptr) {
  outKey.clear();
  if (outSlc) {
    *outSlc = 0;
  }
  if (outHasRenderParams) {
    *outHasRenderParams = false;
  }
  if (outCallerOffset) {
    *outCallerOffset = 0;
  }
  if (!requiredKey || !requiredKey[0]) {
    return false;
  }

  bool found = false;
  bool foundWithParams = false;
  std::string bestKey;
  uint32_t bestSlc = 0;
  bool bestHasParams = false;
  unsigned int bestCaller = 0;
  for (unsigned int caller : TextHook_GetConfirmedStatusConsumerCallers()) {
    std::string key;
    uint32_t slc = 0;
    bool hasParams = false;
    unsigned int probeCaller = 0;
    if (!TextHook_GetRecentObjectiveStatusProbeByKey(maxAgeMs, requiredKey, caller,
                                                     key, &slc, &hasParams,
                                                     &probeCaller)) {
      continue;
    }
    if (!found || (hasParams && !foundWithParams)) {
      found = true;
      foundWithParams = hasParams;
      bestKey = key;
      bestSlc = slc;
      bestHasParams = hasParams;
      bestCaller = (probeCaller != 0) ? probeCaller : caller;
      if (foundWithParams) {
        break;
      }
    }
  }

  if (!found) {
    return false;
  }
  outKey = bestKey;
  if (outSlc) {
    *outSlc = bestSlc;
  }
  if (outHasRenderParams) {
    *outHasRenderParams = bestHasParams;
  }
  if (outCallerOffset) {
    *outCallerOffset = bestCaller;
  }
  return true;
}

static void ObjRev_UpdateObjectiveAuthoritySlcCaller(uintptr_t slcCaller,
                                                     const char *reason) {
  if (slcCaller == 0) {
    return;
  }

  const uintptr_t prevSlc = ObjRev_GetObjectiveAuthoritySlcCallerOffset();
  if (prevSlc == slcCaller) {
    return;
  }

  const unsigned long now = GetTickCount();
  struct ObjAuthSlcCallerEvidence {
    unsigned long tick = 0;
    unsigned int hits = 0;
  };
  static std::unordered_map<uintptr_t, ObjAuthSlcCallerEvidence>
      s_authSlcCallerEvidence;
  ObjAuthSlcCallerEvidence &ev = s_authSlcCallerEvidence[slcCaller];
  if (ev.tick == 0 || now < ev.tick || (now - ev.tick) > 1800) {
    ev.hits = 0;
  }
  ev.tick = now;
  if (ev.hits < 0xFFFFu) {
    ++ev.hits;
  }

  constexpr unsigned int kPromoteHits = 2;
  if (ev.hits < kPromoteHits) {
    static std::unordered_map<uintptr_t, unsigned long> s_authSlcCandLogTick;
    auto itLog = s_authSlcCandLogTick.find(slcCaller);
    if (itLog == s_authSlcCandLogTick.end() || (now - itLog->second) > 1200) {
      s_authSlcCandLogTick[slcCaller] = now;
      char cbuf[256];
      sprintf_s(cbuf,
                "[OBJ-AUTH-SLC-CALLER-CAND] slc=+0x%llX hits=%u reason=%s",
                (unsigned long long)slcCaller, ev.hits,
                reason ? reason : "unspecified");
      LogToFile(cbuf);
    }
    return;
  }

  const uintptr_t prevType = ObjRev_GetObjectiveAuthorityTypeCheckCallerOffset();
  g_objAuthoritySlcCallerOffset.store(slcCaller, std::memory_order_relaxed);
  if ((prevType + 0x0Fu) != slcCaller && slcCaller > 0x0Fu) {
    g_objAuthorityTypeCheckCallerOffset.store(slcCaller - 0x0Fu,
                                              std::memory_order_relaxed);
  }

  char cbuf[320];
  sprintf_s(
      cbuf,
      "[OBJ-AUTH-SLC-CALLER-RESOLVE] slc=+0x%llX prevSlc=+0x%llX prevType=+0x%llX hits=%u reason=%s",
      (unsigned long long)slcCaller, (unsigned long long)prevSlc,
      (unsigned long long)prevType, ev.hits, reason ? reason : "unspecified");
  LogToFile(cbuf);

  if (s_authSlcCallerEvidence.size() > 64) {
    for (auto it = s_authSlcCallerEvidence.begin();
         it != s_authSlcCallerEvidence.end();) {
      if (now >= it->second.tick && (now - it->second.tick) > 20000) {
        it = s_authSlcCallerEvidence.erase(it);
      } else {
        ++it;
      }
    }
  }
}

struct ObjConsumerResolveEvent {
  uint32_t cfgIdx = 0;
  uint32_t bias = 0;
  uint32_t queryIdx = 0;
  uint32_t slcIdx = 0;
  unsigned int cfgCallerOffset = 0;
  uint32_t typeValue = 0;
  uint32_t ordinal = 0;
  uintptr_t r12 = 0;
  uintptr_t rsi = 0;
  uintptr_t rbp = 0;
  uintptr_t rsp = 0;
  DWORD tick = 0;
  bool objectiveCaller = false;
  uint32_t keyClass = 0; // 1=objective, 2=status, 3=other localizable
  std::string keyName;
};
static std::mutex g_objConsumerResolveMutex;
static std::deque<ObjConsumerResolveEvent> g_objConsumerResolveEvents;

static std::string ObjRev_SanitizeEnglishForKeyResolve(const std::string &in) {
  if (in.empty()) {
    return in;
  }
  std::string out;
  out.reserve(in.size());
  for (unsigned char uc : in) {
    if (uc < 0x20) {
      if (uc == '\n' || uc == '\r' || uc == '\t') {
        out.push_back((char)uc);
      }
      continue;
    }
    out.push_back((char)uc);
  }
  size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) {
    return "";
  }
  size_t last = out.find_last_not_of(' ');
  return out.substr(first, last - first + 1);
}

// =============================================================================
// Menu Font Size Bucketing (3 tiers)
// =============================================================================
namespace {
constexpr float kMenuScaleLarge = 1.5f;
constexpr float kMenuScaleNormal = 1.0f;
constexpr float kMenuScaleSmall = 0.85f;
constexpr float kEdgeMarginBase = 200.0f;    // 기준: 2560x1440에서 200px
constexpr float kCenterBandYBase = 80.0f;    // 중앙 근처(세로) 판단 밴드
constexpr float kCenterBandXBase = 120.0f;   // 중앙 근처(가로) 판단 밴드
constexpr float kBaseScreenHeight = 1440.0f; // 기준 해상도
constexpr float kBaseScreenWidth = 2560.0f;  // 기준 해상도

static std::atomic<int> g_LastScreenWidth{0};
static std::atomic<int> g_LastScreenHeight{0};

bool IsKoreanMenuText(const char *text) {
  if (!text)
    return false;
  return strstr(text, "캠페인") || strstr(text, "멀티플레이") ||
         strstr(text, "스쿼드") || strstr(text, "분대") ||
         strstr(text, "익스팅션") || strstr(text, "소멸") ||
         strstr(text, "설정") || strstr(text, "비디오") ||
         strstr(text, "오디오") || strstr(text, "컨트롤") ||
         strstr(text, "밝기") || strstr(text, "종료") || strstr(text, "재개") ||
         strstr(text, "돌아") || strstr(text, "시작") || strstr(text, "선택") ||
         strstr(text, "목록") || strstr(text, "저장합니다") ||
         strstr(text, "알림") || strstr(text, "계속");
}

float GetScreenHeightApprox() {
  int h = g_LastScreenHeight.load();
  if (h <= 0)
    h = GetSystemMetrics(SM_CYSCREEN);
  if (h <= 0)
    h = (int)kBaseScreenHeight;
  return (float)h;
}

float GetScreenWidthApprox() {
  int w = g_LastScreenWidth.load();
  if (w <= 0)
    w = GetSystemMetrics(SM_CXSCREEN);
  if (w <= 0)
    w = (int)kBaseScreenWidth;
  return (float)w;
}

std::string TrimSpaces(const std::string &s) {
  size_t start = s.find_first_not_of(' ');
  if (start == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(' ');
  return s.substr(start, end - start + 1);
}

int CountWords(const std::string &s) {
  int count = 0;
  bool inWord = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (inWord)
        inWord = false;
    } else {
      if (!inWord) {
        inWord = true;
        count++;
      }
    }
  }
  return count;
}

std::string ToUpperAscii(const std::string &s) {
  std::string ret = s;
  for (char &c : ret) {
    if (c >= 'a' && c <= 'z')
      c -= 32;
  }
  return ret;
}

std::string NormalizeWhitespaceAscii(const std::string &s, bool treatTabsAsSpace,
                                     bool toUpper) {
  std::string out;
  out.reserve(s.size());

  bool lastWasSpace = true; // Start true to trim leading spaces.
  for (char ch : s) {
    char c = ch;
    if (c == '\n' || c == '\r' || (treatTabsAsSpace && c == '\t')) {
      c = ' ';
    }

    if (c == ' ') {
      if (!lastWasSpace) {
        out.push_back(' ');
      }
      lastWasSpace = true;
      continue;
    }

    if (toUpper && c >= 'a' && c <= 'z') {
      c = (char)(c - 32);
    }

    out.push_back(c);
    lastWasSpace = false;
  }

  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }

  return out;
}

static bool ObjRev_TryExtractStructuredObjectiveKey(const std::string &payload,
                                                    std::string &outKey) {
  outKey.clear();
  if (payload.empty() || payload[0] != '\\') {
    return false;
  }

  size_t searchPos = 0;
  while (true) {
    const size_t strPos = payload.find("\\str\\", searchPos);
    if (strPos == std::string::npos) {
      return false;
    }
    const size_t keyStart = strPos + 5;
    if (keyStart >= payload.size()) {
      return false;
    }
    const size_t keyEnd = payload.find('\\', keyStart);
    const std::string candidate =
        payload.substr(keyStart, keyEnd == std::string::npos
                                     ? std::string::npos
                                     : (keyEnd - keyStart));
    if (ObjRev_LooksLikeLocalizationKey(candidate.c_str()) &&
        !IsObjectiveStatusMessageKeyName(candidate) &&
        IsObjectiveListBootstrapKeyName(candidate)) {
      outKey = candidate;
      return true;
    }
    searchPos = keyStart;
  }
}

std::string StripVariantMarkers(const std::string &s) {
  std::string out = s;
  const std::string open = "⟦";
  const std::string close = "⟧";
  size_t start = out.find(open);
  while (start != std::string::npos) {
    size_t end = out.find(close, start + open.size());
    if (end == std::string::npos) {
      out.erase(start);
      break;
    }
    out.erase(start, end - start + close.size());
    start = out.find(open, start);
  }
  return out;
}

std::string StripTrailingMenuIndex(const std::string &s) {
  size_t end = s.size();
  while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1])))
    end--;

  size_t digitEnd = end;
  size_t digitStart = digitEnd;
  while (digitStart > 0 &&
         std::isdigit(static_cast<unsigned char>(s[digitStart - 1])))
    digitStart--;

  if (digitStart < digitEnd) {
    size_t spaceStart = digitStart;
    while (spaceStart > 0 &&
           std::isspace(static_cast<unsigned char>(s[spaceStart - 1])))
      spaceStart--;
    // Only strip if digits are separated by whitespace (e.g., "예 3")
    if (spaceStart < digitStart) {
      return TrimSpaces(s.substr(0, spaceStart));
    }
  }

  return TrimSpaces(s.substr(0, end));
}

std::string NormalizeMenuText(const std::string &s) {
  // Fast path: if the string contains none of the artifacts that the slow path
  // strips (color codes, control chars, spaces, variant markers, trailing menu
  // index), return the input as-is with zero heap allocations.
  if (!s.empty()) {
    const unsigned char *p =
        reinterpret_cast<const unsigned char *>(s.data());
    const size_t len = s.size();
    bool clean = (p[0] != ' ' && p[len - 1] != ' ');
    if (clean) {
      // Trailing menu index pattern: digits preceded by whitespace
      size_t i = len;
      while (i > 0 && p[i - 1] >= '0' && p[i - 1] <= '9')
        --i;
      if (i < len && i > 0 && p[i - 1] == ' ')
        clean = false;
    }
    if (clean) {
      for (size_t i = 0; i < len; ++i) {
        if (p[i] == '^' || p[i] < 0x20 || p[i] == 0x7F) {
          clean = false;
          break;
        }
        // ⟦ = UTF-8: E2 9F A6
        if (p[i] == 0xE2 && i + 2 < len &&
            p[i + 1] == 0x9F && p[i + 2] == 0xA6) {
          clean = false;
          break;
        }
      }
    }
    if (clean)
      return s;
  }

  // Slow path: full normalization (~9 heap allocations)
  std::string cleaned = StripColorCodes(s);
  // Remove stray C0 control bytes (e.g. \x01) that can break menu-key matching
  // and may leak as visible glyph artifacts in some controller layout entries.
  std::string noCtrl;
  noCtrl.reserve(cleaned.size());
  for (unsigned char uc : cleaned) {
    if ((uc >= 0x20 && uc != 0x7F) || uc >= 0x80) {
      noCtrl.push_back((char)uc);
    }
  }
  cleaned.swap(noCtrl);
  cleaned = TrimSpaces(cleaned);
  cleaned = StripVariantMarkers(cleaned);
  cleaned = TrimSpaces(cleaned);
  cleaned = StripTrailingMenuIndex(cleaned);
  cleaned = TrimSpaces(cleaned);
  return cleaned;
}

bool IsYesNoText(const std::string &text) {
  std::string upper = ToUpperAscii(text);
  if (upper == "YES" || upper == "NO")
    return true;
  if (text == "예" || text == "예." || text == "예!" || text == "예?" ||
      text == "아니오" || text == "아니요" || text == "아뇨")
    return true;
  return false;
}

bool IsRorkeFileStatusText(const std::string &text) {
  if (text.find("로크 파일을 찾을 수 없습니다") != std::string::npos ||
      text.find("로크 파일을 회수했습니다") != std::string::npos ||
      text.find("로그 파일을 찾을 수 없습니다") != std::string::npos ||
      text.find("로그 파일을 회수했습니다") != std::string::npos ||
      text.find("로크 파일을 찾지 못했습니다") != std::string::npos ||
      text.find("로그 파일을 찾지 못했습니다") != std::string::npos)
    return true;

  // Space-insensitive fallback for variants like "로크파일".
  std::string compact;
  compact.reserve(text.size());
  for (char c : text) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      compact.push_back(c);
  }
  if (compact.find("로크파일을회수했습니다") != std::string::npos ||
      compact.find("로그파일을회수했습니다") != std::string::npos ||
      compact.find("로크파일을찾을수없습니다") != std::string::npos ||
      compact.find("로그파일을찾을수없습니다") != std::string::npos ||
      compact.find("로크파일을찾지못했습니다") != std::string::npos ||
      compact.find("로그파일을찾지못했습니다") != std::string::npos)
    return true;

  std::string upper = ToUpperAscii(text);
  if (upper.find("RORKE FILE NOT FOUND") != std::string::npos ||
      upper.find("RORKE FILE RECOVERED") != std::string::npos)
    return true;
  return false;
}

bool IsAdvancedVideoHeaderText(const std::string &text) {
  if (text.find("고급 비디오") != std::string::npos)
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper.find("ADVANCED VIDEO") != std::string::npos)
    return true;
  return false;
}

bool IsOptionsHeaderText(const std::string &text) {
  if (text.empty())
    return false;
  if (text.find("옵션") != std::string::npos ||
      text.find("비디오 옵션") != std::string::npos ||
      text.find("오디오 옵션") != std::string::npos ||
      text.find("컨트롤 옵션") != std::string::npos ||
      text.find("고급 비디오") != std::string::npos)
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper.find("OPTIONS") != std::string::npos ||
      upper.find("VIDEO OPTIONS") != std::string::npos ||
      upper.find("AUDIO OPTIONS") != std::string::npos ||
      upper.find("CONTROL OPTIONS") != std::string::npos ||
      upper.find("ADVANCED VIDEO") != std::string::npos)
    return true;
  return false;
}

bool IsPauseRootHeaderText(const std::string &text) {
  if (text.empty())
    return false;
  if (text == "일시 정지" || text == "일시 중지" || text == "임무 목표")
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper == "MISSION OBJECTIVES")
    return true;
  return false;
}

bool IsObjectiveLikeMenuKeyName(const std::string &key) {
  if (key.empty())
    return false;
  if (key.find("_OBJ_") != std::string::npos ||
      key.find("_OBJECTIVE_") != std::string::npos ||
      HasObjectiveSuffixOnlyKey(key) ||
      key.rfind("GAME_OBJECTIVE", 0) == 0 ||
      key.rfind("CGAME_OBJECTIVE", 0) == 0 ||
      key.rfind("CORNERED_OBJ_", 0) == 0 ||
      key.rfind("CORNERED_OBJECTIVE_", 0) == 0)
    return true;
  return false;
}

bool IsAdvancedVideoValueText(const std::string &text) {
  if (text.empty())
    return false;
  if (IsYesNoText(text))
    return true;

  if (text == "자동" || text == "일반" || text == "낮음" || text == "보통" ||
      text == "높음" || text == "예" || text == "아니오" || text == "아니요" ||
      text == "아뇨" || text == "켜기" || text == "끄기" || text == "꺼짐" ||
      text == "엑스트라" ||
      text == "표준")
    return true;

  if (text.find("표준") != std::string::npos ||
      text.find("와이드") != std::string::npos ||
      text.find("MSAA") != std::string::npos ||
      text.find("Hz") != std::string::npos)
    return true;

  bool hasDigit = false;
  bool hasColon = false;
  bool hasX = false;
  for (char c : text) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (std::isdigit(uc))
      hasDigit = true;
    if (c == ':')
      hasColon = true;
    if (c == 'x' || c == 'X')
      hasX = true;
  }
  if (hasDigit && (hasColon || hasX))
    return true;

  std::string upper = ToUpperAscii(text);
  if (upper == "AUTO" || upper == "NORMAL" || upper == "LOW" ||
      upper == "MEDIUM" || upper == "HIGH" || upper == "ULTRA" ||
      upper == "EXTRA" ||
      upper == "ON" || upper == "OFF" || upper == "YES" || upper == "NO")
    return true;
  if (upper.find("MSAA") != std::string::npos ||
      upper.find("FXAA") != std::string::npos ||
      upper.find("SMAA") != std::string::npos ||
      upper.find("TAA") != std::string::npos ||
      upper.find("HZ") != std::string::npos ||
      upper.find("STANDARD") != std::string::npos ||
      upper.find("WIDE") != std::string::npos)
    return true;

  return false;
}

bool IsMenuHeaderForceLarge(const std::string &text) {
  if (text == "난이도 선택" || text == "알림" ||
      text == "설정 적용" || text == "임무 목표")
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper == "SELECT DIFFICULTY" || upper == "NOTICE" ||
      upper == "APPLY SETTINGS" || upper == "MISSION OBJECTIVES")
    return true;
  return false;
}

bool IsCenterPopupHeaderOnly(const std::string &text) {
  if (text == "새 게임")
    return true;
  if (text == "임무 다시 시작")
    return true;
  if (text == "난이도 낮추기")
    return true;
  if (text == "마지막 체크포인트")
    return true;
  if (text == "정말 종료하시겠습니까?")
    return true;
  if (text == "기본값 복원")
    return true;
  if (text == "비디오 최적화")
    return true;
  if (text == "알림")
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper == "NEW GAME")
    return true;
  if (upper == "RESTART MISSION" || upper == "MISSION RESTART")
    return true;
  if (upper == "LOWER DIFFICULTY" || upper == "DECREASE DIFFICULTY")
    return true;
  if (upper == "LAST CHECKPOINT")
    return true;
  if (upper == "ARE YOU SURE YOU WANT TO QUIT?" ||
      upper == "ARE YOU SURE YOU WANT TO EXIT?")
    return true;
  if (upper == "RESTORE DEFAULTS")
    return true;
  if (upper == "OPTIMAL VIDEO")
    return true;
  if (upper == "NOTICE")
    return true;
  return false;
}

bool IsPauseMenuText(const std::string &text) {
  if (text.empty())
    return false;
  std::string upper = ToUpperAscii(text);
  if (upper == "PAUSED" || upper == "RESUME GAME" ||
      upper == "MISSION SELECT" || upper == "RESTART MISSION" ||
      upper == "MISSION RESTART" || upper == "SAVE AND QUIT" ||
      upper == "OPTIONS" || upper == "PAUSE MENU")
    return true;
  if (text == "일시 정지" || text == "일시 중지" || text == "게임 재개" ||
      text == "임무 선택" || text == "임무 다시 시작" ||
      text == "저장하고 종료" || text == "옵션")
    return true;
  return false;
}

bool IsCreditsPauseMenuItemText(const std::string &text) {
  if (text == "크레딧 재개" || text == "종료")
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper == "RESUME CREDITS" || upper == "QUIT")
    return true;
  return false;
}



bool IsPopupQuestionHeader(const std::string &text) {
  if (text.find("종료") != std::string::npos &&
      text.find("겠습니까") != std::string::npos)
    return false;
  if (text.find("적용") != std::string::npos &&
      text.find("겠습니까") != std::string::npos)
    return false;
  std::string upper = ToUpperAscii(text);
  if (upper.find("QUIT") != std::string::npos ||
      upper.find("EXIT") != std::string::npos)
    return false;
  if (upper.find("APPLY") != std::string::npos &&
      upper.find("SETTINGS") != std::string::npos)
    return false;
  if (text.find('?') != std::string::npos && text.size() <= 48)
    return true;
  if (upper.find("ARE YOU SURE") != std::string::npos)
    return true;
  if (upper.find("RESUME") != std::string::npos &&
      upper.find("GAME") != std::string::npos)
    return true;
  return false;
}

bool IsMenuItemForceNormal(const std::string &text) {
  if (IsYesNoText(text))
    return true;
  if (text == "취소")
    return true;
  if (text == "종료")
    return true;
  if (text == "계속")
    return true;
  if (text.find("종료하면") != std::string::npos)
    return true;
  if (text.find("낮추기") != std::string::npos) {
    if (text.find("훈련병") != std::string::npos ||
        text.find("쉬움") != std::string::npos ||
        text.find("보통") != std::string::npos ||
        text.find("어려움") != std::string::npos ||
        text.find("베테랑") != std::string::npos)
      return true;
  }
  std::string upperLower = ToUpperAscii(text);
  if (upperLower == "CANCEL")
    return true;
  if (upperLower == "CONTINUE")
    return true;
  if (upperLower.find("QUIT NOW") != std::string::npos)
    return true;
  if (upperLower.find("LOWER") != std::string::npos &&
      upperLower.find("DIFFICULTY") != std::string::npos)
    return true;
  if (text == "쉬움" || text == "보통" || text == "일반" ||
      text == "어려움" || text == "베테랑")
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper == "EASY" || upper == "REGULAR" || upper == "NORMAL" ||
      upper == "HARDENED" || upper == "VETERAN" ||
      upper == "QUIT" || upper == "EXIT")
    return true;
  return false;
}

bool IsDifficultyLabelText(const std::string &text) {
  if (text.find("쉬움") != std::string::npos ||
      text.find("보통") != std::string::npos ||
      text.find("일반") != std::string::npos ||
      text.find("어려움") != std::string::npos ||
      text.find("베테랑") != std::string::npos)
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper.find("RECRUIT") != std::string::npos ||
      upper.find("REGULAR") != std::string::npos ||
      upper.find("HARDENED") != std::string::npos ||
      upper.find("VETERAN") != std::string::npos)
    return true;
  return false;
}

bool IsMenuItemForceSmall(const std::string &text) {
  if (text.find("종료") != std::string::npos &&
      text.find("겠습니까") != std::string::npos)
    return true;
  if (text.find("지금 종료하면 마지막 체크포인트 이후의 진행 상황을 잃게 됩니다") !=
      std::string::npos)
    return true;
  if (text.find("지금 종료하면 마지막 체크포인트 이후의") != std::string::npos &&
      text.find("진행 상황을 잃게 됩니다") != std::string::npos)
    return true;
  if (text.find("설정") != std::string::npos &&
      text.find("적용") != std::string::npos &&
      text.find("겠습니까") != std::string::npos)
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper.find("IF YOU QUIT NOW") != std::string::npos &&
      upper.find("CHECKPOINT") != std::string::npos)
    return true;
  if (upper.find("ARE YOU SURE") != std::string::npos &&
      (upper.find("QUIT") != std::string::npos ||
       upper.find("EXIT") != std::string::npos))
    return true;
  if (upper.find("APPLY SETTINGS") != std::string::npos &&
      upper.find("ARE YOU SURE") != std::string::npos)
    return true;
  return false;
}

bool IsPopupDescText(const std::string &text) {
  if (text.empty())
    return false;
  if (text.find("저장 후 종료") != std::string::npos ||
      text.find("저장후 종료") != std::string::npos)
    return false;
  if (text.find('?') != std::string::npos)
    return true;
  if (text.find("습니까") != std::string::npos)
    return true;
  if (text.size() >= 20) {
    if (text.find("체크포인트") != std::string::npos)
      return true;
    if (text.find("진행") != std::string::npos)
      return true;
    if (text.find("저장") != std::string::npos)
      return true;
    if (text.find("종료") != std::string::npos)
      return true;
    if (text.find("재시작") != std::string::npos ||
        text.find("다시 시작") != std::string::npos)
      return true;
  }
  if (text.size() >= 12 &&
      (text.find("난이도") != std::string::npos ||
       text.find("임무") != std::string::npos))
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper.find("SAVE AND QUIT") != std::string::npos)
    return false;
  if (text.size() >= 12 &&
      (upper.find("DIFFICULTY") != std::string::npos ||
       upper.find("MISSION") != std::string::npos))
    return true;
  if (text.size() >= 20 &&
      (upper.find("CHECKPOINT") != std::string::npos ||
       upper.find("PROGRESS") != std::string::npos ||
       upper.find("SAVE") != std::string::npos ||
       upper.find("QUIT") != std::string::npos ||
       upper.find("RESTART") != std::string::npos ||
       upper.find("RESUME") != std::string::npos))
    return true;
  return false;
}

bool IsLongDescriptionText(const std::string &text) {
  std::string compact;
  compact.reserve(text.size());
  for (char c : text) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      compact.push_back(c);
  }

  // Exceptions - these are NOT descriptions
  if (compact.find("화면공간주변폐색") != std::string::npos)
    return false;
  if (compact.find("screenspaceambientocclusion") != std::string::npos)
    return false;

  // [FIX] Check for punctuation that indicates a real description
  bool hasPeriod = (text.find('.') != std::string::npos);
  bool hasQuestion = (text.find('?') != std::string::npos);
  bool hasPunctuation = hasPeriod || hasQuestion;

  // [PRIORITY] Text with period is almost always a description sentence
  // This should be checked FIRST to catch descriptions reliably
  if (hasPeriod) {
    // Exception: very short text with period might not be a description
    // e.g., "1." or "A.I." - only treat as description if 10+ chars
    if (text.size() >= 10)
      return true;
  }

  // Campaign footer / checkpoint resume lines should be small even without punctuation
  if (text.find("체크포인트") != std::string::npos &&
      (text.find("저장") != std::string::npos ||
       text.find("다시") != std::string::npos))
    return true;
  std::string upper = ToUpperAscii(text);
  if (upper.find("CHECKPOINT") != std::string::npos &&
      (upper.find("RESUME") != std::string::npos ||
       upper.find("SAVED") != std::string::npos))
    return true;

  // Korean formal sentence endings indicate descriptions
  if (text.find("합니다") != std::string::npos)
    return true;
  if (text.find("하십시오") != std::string::npos)
    return true;
  if (text.find("입니다") != std::string::npos)
    return true;
  if (text.find("됩니다") != std::string::npos)
    return true;
  if (text.find("습니다") != std::string::npos)
    return true;
  if (text.find("하세요") != std::string::npos)
    return true;

  // Question mark with reasonable length is a description
  if (hasQuestion && text.size() >= 10)
    return true;

  // Long text (24+ chars) without punctuation is NOT a description
  // (These are menu option labels like "수류탄 투척/장비 사용")
  if (text.size() >= 24 && text.find(' ') != std::string::npos) {
    return false;
  }

  return false;
}

float GetMenuFontScaleMul(float fontHeight, bool isMenuContext,
                          const char *rawText, float x, float y,
                          float screenWidth, float screenHeight,
                          float textWidth) {
  if (!isMenuContext || fontHeight <= 0.0f)
    return kMenuScaleNormal;
  if (!rawText || !rawText[0])
    return kMenuScaleNormal;

  float safeHeight =
      (screenHeight > 0.0f) ? screenHeight : GetScreenHeightApprox();
  float safeWidth = (screenWidth > 0.0f) ? screenWidth : GetScreenWidthApprox();

  // Remap coordinates from full backbuffer space to 16:9 active area space.
  if (g_ActiveArea.height > 1.0f && g_ActiveArea.height < safeHeight - 1.0f) {
    y -= g_ActiveArea.offsetY;
    x -= g_ActiveArea.offsetX;
    safeHeight = g_ActiveArea.height;
    safeWidth = g_ActiveArea.width;
  }

  float edgeMargin = safeHeight * (kEdgeMarginBase / kBaseScreenHeight);
  float centerBandY = safeHeight * (kCenterBandYBase / kBaseScreenHeight);
  float centerBandX = safeWidth * (kCenterBandXBase / kBaseScreenWidth);

  // Cache NormalizeMenuText + ToUpperAscii results to avoid repeated string
  // processing for the same text (called multiple times per AddCmd path).
  // NOTE: Compare by string CONTENT, not pointer address. Pointer-only
  // comparison caused stale cache hits when the heap reused an address for
  // a different string during loading, poisoning scale classification (~10%
  // of launches).
  static thread_local std::string s_lastRawContent;
  static thread_local std::string s_lastCleaned;
  static thread_local std::string s_lastCleanedUpper;
  const std::string &cleaned = [&]() -> const std::string & {
    if (!rawText || s_lastRawContent.empty() ||
        strcmp(rawText, s_lastRawContent.c_str()) != 0) {
      s_lastRawContent = rawText ? rawText : "";
      s_lastCleaned = NormalizeMenuText(s_lastRawContent);
      s_lastCleanedUpper = ToUpperAscii(s_lastCleaned);
    }
    return s_lastCleaned;
  }();
  if (cleaned.empty())
    return kMenuScaleNormal;
  const std::string &cleanedUpper = s_lastCleanedUpper;
  float centerY = safeHeight * 0.5f;
  float centerX = safeWidth * 0.5f;
  float textCenterX = (textWidth > 0.0f) ? (x + textWidth * 0.5f) : x;
  float popupHeaderBandY = safeHeight * 0.30f;
  float popupHeaderBandX = centerBandX * 2.0f;
  const DWORD nowMenuTick = GetTickCount();
  static DWORD s_MissionSelectHeaderSeenTick = 0;
  static bool s_MainRootMenuContext = false;
  static bool s_AdvancedVideoMenuContext = false;
  const bool isMainRootEntryText =
      (cleaned == "캠페인" || cleaned == "멀티플레이어" ||
       cleaned == "스쿼드" || cleaned == "익스팅션" ||
       cleanedUpper == "CAMPAIGN" || cleanedUpper == "MULTIPLAYER" ||
       cleanedUpper == "SQUADS" || cleanedUpper == "EXTINCTION");
  if (isMainRootEntryText && x <= safeWidth * 0.45f &&
      y >= safeHeight * 0.20f && y <= safeHeight * 0.72f) {
    s_MainRootMenuContext = true;
  }
  if (IsPauseRootHeaderText(cleaned) || IsOptionsHeaderText(cleaned) ||
      cleanedUpper == "MISSION SELECT" || cleanedUpper == "RORKE FILES") {
    s_MainRootMenuContext = false;
  }
  if (IsAdvancedVideoHeaderText(cleaned) && y <= edgeMargin) {
    s_AdvancedVideoMenuContext = true;
  }
  if ((IsOptionsHeaderText(cleaned) && !IsAdvancedVideoHeaderText(cleaned) &&
       y <= edgeMargin) ||
      IsPauseRootHeaderText(cleaned) || cleanedUpper == "MISSION SELECT" ||
      cleanedUpper == "CAMPAIGN" || cleanedUpper == "RORKE FILES") {
    s_AdvancedVideoMenuContext = false;
  }
  if ((cleaned == "임무 선택" || cleanedUpper == "MISSION SELECT") &&
      y <= edgeMargin) {
    s_MissionSelectHeaderSeenTick = nowMenuTick;
  }
  const bool missionSelectHeaderActive =
      (s_MissionSelectHeaderSeenTick != 0) &&
      ((nowMenuTick - s_MissionSelectHeaderSeenTick) <= 900);
  static bool s_QuitTokenPopupHeaderLatch = false;
  static bool s_NoticePopupHeaderLatch = false;
  static DWORD s_KoreanQuitPopupHeaderTick = 0;
  static DWORD s_KoreanQuitPopupQuestionTick = 0;
  static DWORD s_KoreanQuitPopupConfirmTick = 0;
  static DWORD s_KoreanQuitPopupCancelTick = 0;
  const DWORD kQuitPopupComposeHoldMs = 900;
  auto IsRecentPopupTick = [&](DWORD tick) -> bool {
    return tick != 0 && ((nowMenuTick - tick) <= kQuitPopupComposeHoldMs);
  };
  auto ResetKoreanQuitPopupTicks = [&]() {
    s_KoreanQuitPopupHeaderTick = 0;
    s_KoreanQuitPopupQuestionTick = 0;
    s_KoreanQuitPopupConfirmTick = 0;
    s_KoreanQuitPopupCancelTick = 0;
  };
  float dx = textCenterX - centerX;
  if (dx < 0.0f)
    dx = -dx;
  float dxAnchor = x - centerX;
  if (dxAnchor < 0.0f)
    dxAnchor = -dxAnchor;
  bool nearPopupCenter =
      ((dx <= popupHeaderBandX) || (dxAnchor <= popupHeaderBandX * 1.15f)) &&
      (y >= (centerY - popupHeaderBandY) && y <= (centerY + popupHeaderBandY));
  const bool isQuitTokenPopupHeader =
      (cleaned == "종료" || cleanedUpper == "QUIT" || cleanedUpper == "EXIT") &&
      nearPopupCenter && y <= centerY;
  const bool isNoticePopupHeader =
      (cleaned == "알림" || cleanedUpper == "NOTICE") &&
      nearPopupCenter && y <= centerY;
  if (isQuitTokenPopupHeader) {
    s_QuitTokenPopupHeaderLatch = true;
    s_NoticePopupHeaderLatch = false;
  }
  if (isNoticePopupHeader) {
    s_NoticePopupHeaderLatch = true;
    s_QuitTokenPopupHeaderLatch = false;
  }
  const bool isQuitQuestionText =
      (cleaned == "정말 종료하시겠습니까?" ||
       cleanedUpper == "ARE YOU SURE YOU WANT TO QUIT?" ||
       cleanedUpper == "ARE YOU SURE YOU WANT TO EXIT?");
  const bool isQuitQuestionStableSmallBand =
      isQuitQuestionText &&
      (y >= safeHeight * 0.40f && y <= safeHeight * 0.68f) &&
      ((dx <= popupHeaderBandX * 1.35f) ||
       (dxAnchor <= popupHeaderBandX * 1.35f));
  const bool isQuitQuestionBodyByLayout =
      isQuitQuestionText && nearPopupCenter &&
      (y >= (centerY - safeHeight * 0.08f));
  const bool isKoreanQuitQuestionText =
      (cleaned == "정말 종료하시겠습니까?");
  const bool isKoreanQuitConfirmButton =
      (cleaned == "종료") && nearPopupCenter && (y > centerY);
  const bool isKoreanQuitCancelButton =
      (cleaned == "취소") && nearPopupCenter && (y > centerY);

  if (!IsRecentPopupTick(s_KoreanQuitPopupHeaderTick))
    s_KoreanQuitPopupHeaderTick = 0;
  if (!IsRecentPopupTick(s_KoreanQuitPopupQuestionTick))
    s_KoreanQuitPopupQuestionTick = 0;
  if (!IsRecentPopupTick(s_KoreanQuitPopupConfirmTick))
    s_KoreanQuitPopupConfirmTick = 0;
  if (!IsRecentPopupTick(s_KoreanQuitPopupCancelTick))
    s_KoreanQuitPopupCancelTick = 0;

  if (isQuitTokenPopupHeader && cleaned == "종료")
    s_KoreanQuitPopupHeaderTick = nowMenuTick;
  if (isKoreanQuitQuestionText && nearPopupCenter)
    s_KoreanQuitPopupQuestionTick = nowMenuTick;
  if (isKoreanQuitConfirmButton)
    s_KoreanQuitPopupConfirmTick = nowMenuTick;
  if (isKoreanQuitCancelButton)
    s_KoreanQuitPopupCancelTick = nowMenuTick;

  const bool isKoreanQuitPopupCompositionActive =
      IsRecentPopupTick(s_KoreanQuitPopupHeaderTick) &&
      IsRecentPopupTick(s_KoreanQuitPopupQuestionTick) &&
      IsRecentPopupTick(s_KoreanQuitPopupConfirmTick) &&
      IsRecentPopupTick(s_KoreanQuitPopupCancelTick);

  // Rorke Files top-left classified token should remain header-sized.
  if ((cleaned == "기밀" || cleanedUpper == "CLASSIFIED") &&
      x <= safeWidth * 0.45f && y <= safeHeight * 0.42f) {
    return kMenuScaleLarge;
  }

  // Main title-screen quit button (ESC 종료) should stay submenu-sized.
  if (s_MainRootMenuContext &&
      (cleaned == "종료" || cleanedUpper == "QUIT" || cleanedUpper == "EXIT") &&
      x <= safeWidth * 0.22f && y >= safeHeight * 0.84f) {
    return kMenuScaleSmall;
  }

  if (IsRorkeFileStatusText(cleaned)) {
    if (missionSelectHeaderActive)
      return kMenuScaleSmall;
    return kMenuScaleLarge;
  }

  if (cleaned == "임무 선택" || cleanedUpper == "MISSION SELECT") {
    if (y <= edgeMargin)
      return kMenuScaleLarge;
    if (nearPopupCenter)
      return kMenuScaleLarge;
    return kMenuScaleNormal;
  }

  // Mission Select center-panel mission title should be header-sized.
  // Keep left list / description / difficulty/footer out via strict bounds.
  const bool isMissionSelectCenterTitle =
      missionSelectHeaderActive &&
      x >= safeWidth * 0.30f && x <= safeWidth * 0.72f &&
      y >= safeHeight * 0.62f && y <= safeHeight * 0.86f &&
      !cleaned.empty() && cleaned.size() <= 28 &&
      !IsLongDescriptionText(cleaned) &&
      !IsDifficultyLabelText(cleaned) &&
      !IsRorkeFileStatusText(cleaned) &&
      !IsYesNoText(cleaned) &&
      cleaned.find('?') == std::string::npos &&
      cleaned.find('.') == std::string::npos &&
      cleaned.find('!') == std::string::npos;
  if (isMissionSelectCenterTitle) {
    return kMenuScaleLarge;
  }

  if (cleaned == "임무 다시 시작" || cleanedUpper == "RESTART MISSION" ||
      cleanedUpper == "MISSION RESTART") {
    if (nearPopupCenter && y <= centerY)
      return kMenuScaleLarge;
    return kMenuScaleNormal;
  }

  // In Advanced Video menu, "일반"/"NORMAL" is a value token and must keep
  // normal size (not footer/small) even near lower rows.
  if (s_AdvancedVideoMenuContext &&
      (cleaned == "일반" || cleanedUpper == "NORMAL") &&
      x >= safeWidth * 0.24f && x <= safeWidth * 0.70f &&
      y >= safeHeight * 0.12f && y <= safeHeight * 0.92f) {
    return kMenuScaleNormal;
  }

  if (cleaned == "난이도 낮추기" || cleanedUpper == "LOWER DIFFICULTY" ||
      cleanedUpper == "DECREASE DIFFICULTY") {
    if (nearPopupCenter && y <= centerY)
      return kMenuScaleLarge;
    return kMenuScaleNormal;
  }

  // Layout-first rule to avoid 1~few-frame latch delay:
  // when quit question is rendered as popup body line (e.g. NOTICE popup),
  // force submenu size immediately.
  if (isQuitQuestionStableSmallBand && s_NoticePopupHeaderLatch) {
    return kMenuScaleSmall;
  }
  if (isQuitQuestionBodyByLayout && s_NoticePopupHeaderLatch) {
    return kMenuScaleSmall;
  }

  // Korean quit popup uses a top "종료" token plus lower "종료/취소"
  // buttons. Keep the question body at footer/submenu size once that popup
  // composition is detected, and also honor the top token immediately to
  // avoid a one-frame oversized body line.
  if (isKoreanQuitQuestionText && nearPopupCenter &&
      (s_QuitTokenPopupHeaderLatch || isKoreanQuitPopupCompositionActive)) {
    return kMenuScaleSmall;
  }

  // If popup uses a dedicated top token header ("종료"/"QUIT"), the question
  // line ("정말 종료하시겠습니까?") should render as submenu size.
  if (isQuitQuestionText && nearPopupCenter && s_NoticePopupHeaderLatch) {
    s_QuitTokenPopupHeaderLatch = false;
    s_NoticePopupHeaderLatch = false;
    return kMenuScaleSmall;
  }

  // Reset stale latch when a different popup header is encountered.
  if (nearPopupCenter && !isQuitTokenPopupHeader && !isNoticePopupHeader &&
      !isQuitQuestionText && IsCenterPopupHeaderOnly(cleaned)) {
    s_QuitTokenPopupHeaderLatch = false;
    s_NoticePopupHeaderLatch = false;
    ResetKoreanQuitPopupTicks();
  }
  if (y <= edgeMargin) {
    s_QuitTokenPopupHeaderLatch = false;
    s_NoticePopupHeaderLatch = false;
    ResetKoreanQuitPopupTicks();
  }

  if (IsCenterPopupHeaderOnly(cleaned)) {
    if (nearPopupCenter)
      return kMenuScaleLarge;
    return kMenuScaleNormal;
  }

  if (cleaned == "임무 진행상황" || cleanedUpper == "MISSION STATUS" ||
      cleanedUpper == "MISSION PROGRESS") {
    if (y <= edgeMargin)
      return kMenuScaleLarge;
    return kMenuScaleNormal;
  }

  // Stick/Button layout should be treated as header-sized only when rendered
  // in the top header strip, not in gameplay/controller list rows.
  if (cleaned == "스틱 배치" || cleaned == "스택 배치" ||
      cleaned == "버튼 배치" ||
      cleanedUpper == "STICK LAYOUT" || cleanedUpper == "BUTTON LAYOUT") {
    return (y <= edgeMargin) ? kMenuScaleLarge : kMenuScaleNormal;
  }

  if (IsMenuHeaderForceLarge(cleaned))
    return kMenuScaleLarge;

  // Quit popup top token ("종료"/"QUIT") should stay header-sized only
  // when it is rendered in the center popup header band.
  if (isQuitTokenPopupHeader) {
    return kMenuScaleLarge;
  }

  if (IsMenuItemForceSmall(cleaned))
    return kMenuScaleSmall;

  if (IsDifficultyLabelText(cleaned)) {
    if (y >= safeHeight * 0.70f)
      return kMenuScaleSmall;
  }

  if (IsMenuItemForceNormal(cleaned))
    return kMenuScaleNormal;

  if (IsPopupQuestionHeader(cleaned) && nearPopupCenter)
    return kMenuScaleLarge;

  // 1) Top edge -> Large
  if (y <= edgeMargin)
    return kMenuScaleLarge;

  // 2) Bottom edge -> Small
  if (y >= (safeHeight - edgeMargin))
    return kMenuScaleSmall;

  // 3) Center popup (1~2 words) -> Large (except Yes/No)
  int wordCount = CountWords(cleaned);
  if (IsLongDescriptionText(cleaned))
    return kMenuScaleSmall;

  if (wordCount >= 1 && wordCount <= 2 && !IsYesNoText(cleaned)) {
    if (y >= (centerY - centerBandY) && y <= (centerY + centerBandY) &&
        textCenterX >= (centerX - centerBandX) &&
        textCenterX <= (centerX + centerBandX))
      return kMenuScaleLarge;
  }

  // Fallback: keep middle size
  return kMenuScaleNormal;
}
} // namespace

void TextHook_UpdateScreenSize(int width, int height) {
  if (width > 0)
    g_LastScreenWidth.store(width);
  if (height > 0)
    g_LastScreenHeight.store(height);
}

void TextHook_GetScreenSize(int &width, int &height) {
  width = g_LastScreenWidth.load();
  height = g_LastScreenHeight.load();
}

// =============================================================================

// TextHook - Hooks R_AddCmdDrawText to replace text before rendering

// XMM-SAFE: Uses lock-free queue to avoid register corruption

// =============================================================================

// Use IW6Font::Font_s

using Font_s = IW6Font::Font_s;

typedef void(__fastcall *R_AddCmdDrawText_t)(const char *text, int maxChars,

                                             Font_s *font, float x, float y,

                                             float xScale, float yScale,

                                             float rotation, float *color,

                                             int style);

// R_AddCmdDrawTextWithCursor - Text with cursor (for input fields, etc.)
typedef void(__fastcall *R_AddCmdDrawTextWithCursor_t)(
    const char *text, int maxChars, Font_s *font, float x, float y,
    float xScale, float yScale, float rotation, const float *color, int style,
    int cursorPos, char cursorChar);
static R_AddCmdDrawTextWithCursor_t Original_R_AddCmdDrawTextWithCursor =
    nullptr;
static void *g_CursorTextTargetAddr = nullptr;
static bool g_CursorTextHookApplied = false;

// R_TextWidth - Game engine function for measuring text width accurately
typedef int(__fastcall *R_TextWidth_t)(const char *text, int maxChars,
                                       Font_s *font);
// Published by the delayed hook thread, consumed by the render thread.
static std::atomic<R_TextWidth_t> g_R_TextWidth{nullptr};
static void *g_R_TextWidthAddrPrimary = nullptr;
static void *g_R_TextWidthAddrFallback = nullptr;

static R_AddCmdDrawText_t Original_R_AddCmdDrawText = nullptr;

static void *g_TargetAddrPrimary = nullptr;
static void *g_TargetAddrFallback = nullptr;
static void *g_TargetAddr = nullptr;

static bool g_HookApplied = false;

// =============================================================================
// SEH_StringEd_GetString Hook - For in-game subtitles and localized strings
// =============================================================================
typedef const char *(__fastcall *SEH_StringEd_GetString_t)(
    const char *reference);
static SEH_StringEd_GetString_t Original_SEH_StringEd_GetString = nullptr;
static void *g_SEH_TargetAddr = nullptr;
static bool g_SEH_HookApplied = false;

// =============================================================================
// SL_ConvertToString Hook - CUSTOM DETOUR (no gateway needed)
// Function is only 24 bytes with short jump at offset 2, so standard
// Detour::Create can't be used. We reimplemented the function logic in C++
// and write a raw 14-byte JMP to redirect calls.
// =============================================================================
typedef const char *(__fastcall *SL_ConvertToString_t)(unsigned int stringValue);
static void *g_SLC_TargetAddr = nullptr;
static bool g_SLC_HookApplied = false;
static uintptr_t g_SLC_StringTableGlobal = 0; // address of string table pointer
static unsigned char g_SLC_OriginalBytes[14] = {0}; // saved for unhook
static void *g_SLC_PreStub = nullptr; // assembly pre-stub that saves caller regs

// ConfigString index -> SLC resolver hook (fundamental cfg mapping capture).
typedef unsigned int(__fastcall *ConfigString_IndexToSlc_t)(unsigned int query);
static ConfigString_IndexToSlc_t Original_ConfigString_IndexToSlc = nullptr;
static void *g_CfgToSlc_TargetAddr = nullptr;
static bool g_CfgToSlc_HookApplied = false;

// Objective SLC type-check function (callsite observed at +0x34E24A -> +0x3DE320).
// This path runs immediately before SL_ConvertToString(+0x34E25E).
typedef unsigned int(__fastcall *SL_StringTypeCheck_t)(unsigned int stringValue);
static SL_StringTypeCheck_t Original_SL_StringTypeCheck = nullptr;
static void *g_SL_StringTypeCheck_TargetAddr = nullptr;
static bool g_SL_StringTypeCheck_HookApplied = false;
static void *g_SL_StringTypeCheck_PreStub = nullptr;

// CG_GameMessage hook (status event source).
typedef void(__fastcall *CG_GameMessage_t)(int localClientNum,
                                    const char *message);
static CG_GameMessage_t Original_CG_GameMessage = nullptr;
static void *g_CG_GameMessage_TargetAddr = nullptr;
static bool g_CG_GameMessage_HookApplied = false;

// =============================================================================
// Cbuf_AddText Hook - Intercepts ALL console commands from LUI menus
// Detects: loadgame_continue (checkpoint restart),
//          loadgame_continue_missionfailed (mission restart),
//          fast_restart, disconnect
// =============================================================================
typedef void(__fastcall *Cbuf_AddText_hook_t)(int localClientNum,
                                              const char *text);
static Cbuf_AddText_hook_t Original_Cbuf_AddText = nullptr;
static void *g_Cbuf_AddText_TargetAddr = nullptr;
static bool g_Cbuf_AddText_HookApplied = false;

// HUDHINT writer runtime reverse probe (INT3 + VEH).
// Target instruction (SP): iw6sp64_ship.exe+0x5DC950
//   mov [rdx+rcx-04], eax
static bool g_HudHintWriterBpEnabled = false;
static bool g_HudHintWriterBpInstalled = false;
static uintptr_t g_HudHintWriterBpAddr = 0;
static unsigned char g_HudHintWriterBpOrigByte = 0;
static PVOID g_HudHintWriterVehHandle = nullptr;
static std::atomic<unsigned long> g_HudHintWriterBpHitCount{0};
static std::atomic<unsigned long> g_HudHintWriterBpLogCount{0};
static std::atomic<unsigned long> g_HudHintWriterBpLastLogTick{0};
static std::atomic<DWORD> g_HudHintWriterFocusUntil{0};
static std::atomic<unsigned long> g_HudHintWriterFocusBudget{0};
static std::atomic<unsigned int> g_HudHintWriterFocusCaller{0};
static std::atomic<unsigned long> g_HudHintWriterFocusLastArmTick{0};
static std::mutex g_HudHintWriterFocusMutex;
static std::string g_HudHintWriterFocusKey;

struct RecentHudHintRuntimeSlot {
  uintptr_t base = 0;
  unsigned long firstSeenTick = 0;
  unsigned long lastSeenTick = 0;
  unsigned int callerHint = 0;
  unsigned int ret0Off = 0;
  unsigned int ret1Off = 0;
  unsigned int lastWriteOff = 0;
  uint32_t lastWriteValue = 0;
  unsigned int hitCount = 0;
  unsigned long paramTick = 0;
  float paramX = 0.0f;
  float paramY = 0.0f;
  float paramSX = 0.0f;
  float paramSY = 0.0f;
  unsigned short paramVW = 0;
  unsigned short paramVH = 0;
  int paramSlotBucket = 0;
  bool hasParamSnapshot = false;
  bool hasColorSnapshot = false;
  float paramColor[4] = {1.0f, 1.0f, 1.0f, 0.6f};
  std::string keyHint;
};
static std::mutex g_HudHintRuntimeSlotsMutex;
static std::unordered_map<uintptr_t, RecentHudHintRuntimeSlot>
    g_HudHintRuntimeSlots;
static constexpr DWORD HUD_HINT_RUNTIME_SLOT_TTL = 2500;

struct RecentHudHintRuntimeParams {
  unsigned long tick = 0;
  unsigned int callerHint = 0;
  float x = 0.0f;
  float y = 0.0f;
  float sx = 1.0f;
  float sy = 1.0f;
  unsigned short virtualW = 0;
  unsigned short virtualH = 0;
  int slotBucket = 0;
  bool hasColor = false;
  float color[4] = {1.0f, 1.0f, 1.0f, 0.6f};
  unsigned int hitCount = 0;
};
static std::mutex g_HudHintRuntimeParamsMutex;
static std::unordered_map<std::string, RecentHudHintRuntimeParams>
    g_HudHintRuntimeParams;

// Caller register state captured by assembly pre-stub at SLC entry point.
// The pre-stub runs BEFORE the C++ compiler-generated prologue, so these
// contain the caller's actual non-volatile register values (especially RDI
// which points to hudelem_s for caller 0x1F07DE).
struct SLC_CallerRegs {
  uintptr_t rsp;  // +0  (original caller stack pointer)
  uintptr_t rdi;  // +8
  uintptr_t rsi;  // +16
  uintptr_t rbx;  // +24
  uintptr_t rbp;  // +32
  uintptr_t r12;  // +40
  uintptr_t r13;  // +48
  uintptr_t r14;  // +56
  uintptr_t r15;  // +64
  uintptr_t rcx;  // +72 (volatile arg reg at SLC entry)
  uintptr_t rdx;  // +80
  uintptr_t r8;   // +88
  uintptr_t r9;   // +96
  uintptr_t r10;  // +104
  uintptr_t r11;  // +112
  uintptr_t ret;  // +120 (caller return address from [RSP])
  uintptr_t tid;  // +128 (captured from GS:[0x48])
};
static SLC_CallerRegs g_SLC_CallerRegs = {};
static SLC_CallerRegs g_SL_StringTypeCheck_CallerRegs = {};

// =============================================================================
// HudTextResolve (module+0x1FC8B0) hook
// Text resolution function called by HUD rendering parent (0x1F9400).
// Hooking to determine: (a) is it called per-frame? (b) capture render params
// =============================================================================
typedef const char *(*HudTextResolve_t)(uint64_t, uint64_t, uint64_t, uint64_t);
static HudTextResolve_t Original_HudTextResolve = nullptr;
static void *g_HTR_Addr = nullptr;
static bool g_HTR_Hooked = false;
struct HTR_CallerRegs {
  uintptr_t rdi, rsi, rbx, rbp, r12, r13, r14, r15;
};
static HTR_CallerRegs g_HTR_CallerRegs = {};
static uintptr_t g_HTR_CallerRSP = 0; // Original RSP at hooked function entry
static void *g_HTR_PreStub = nullptr;

// =============================================================================
// IntroTextLayout (0x3F3DE0) - Captures intro/chyron text rendering
// Called from IntroRenderFn(0x1F1740) after SLC_Wrapper resolves the string
// Signature: void* IntroTextLayout(const char* text, void* layoutTable, int flags)
// stolenBytes=15: MOV [RSP+10],RBX(5) + MOV [RSP+18],R8D(5) + PUSH RBP(1) +
//                 PUSH RSI(1) + PUSH RDI(1) + PUSH R12(2) = 15 bytes
// =============================================================================
typedef void *(*IntroTextLayout_t)(const char *text, void *layoutTable, int flags);
static IntroTextLayout_t Original_IntroTextLayout = nullptr;
static bool g_ITL_Hooked = false;

// =============================================================================
// IntroRenderFn (0x1F1740) - Wrapper that calls SLC_Wrapper ??IntroTextLayout
// ??tail-calls 0x416AE0 (actual rendering function)
// Captures arg3 (R8) which is a pointer passed to the rendering function
// Signature: void IntroRenderFn(int type, int slcIndex, void* renderCtx, int count)
// stolenBytes=15: MOV [RSP+8],RBX(5) + MOV [RSP+10],RSI(5) + PUSH RDI(1) +
//                 SUB RSP,0x20(4) = 15 bytes
// =============================================================================
typedef void (*IntroRenderFn_t)(int type, int slcIndex, void *renderCtx,
                                int count);
static IntroRenderFn_t Original_IntroRenderFn = nullptr;
static bool g_IRF_Hooked = false;

// =============================================================================
// IntroActualRender (0x416AE0) - actual intro/timer render consumer
// Signature: void IntroActualRender(int type, void* layoutResult,
//                                   void* renderCtx, int count)
// Read-only detour used for runtime argument/text probing.
// =============================================================================
typedef void (*IntroActualRender_t)(int type, void *layoutResult,
                                    void *renderCtx, int count);
static IntroActualRender_t Original_IntroActualRender = nullptr;
static bool g_IAR_Hooked = false;

// Forward declarations for g_nativeIntroMap (defined further below, near IntroRenderFn)
// Needed here for COORD_CORR correlation in Detour_HUD_DrawText560.
static std::unordered_map<uint32_t, NativeIntroRenderState> g_nativeIntroMap;
static std::mutex g_nativeIntroMutex;

// =============================================================================
// OBJECTIVE REVERSE: captured intro HudElem pointer for array discovery
// =============================================================================
#if GHOSTSKOR_OBJECTIVE_REVERSE
static volatile uintptr_t g_introHudElemPtr = 0;      // Last known valid HudElem* from intro
static volatile unsigned long g_introHudElemTick = 0;  // Tick when captured
static volatile int g_introHudElemServerTime = 0;      // +0x78 (time) from that HudElem

// Discovered array info
static struct {
  uintptr_t baseAddr = 0;
  int stride = 0;
  int count = 0;
  bool valid = false;        // confirmed (has objective keys)
  bool tentative = false;    // plausible but not yet confirmed
} g_hudElemArray;
static std::mutex g_hudElemArrayMutex;

// Objective HudElem suppression: x-coordinate offscreen approach.
// Instead of zeroing type (which kills typewriter sounds), we move x offscreen
// so the engine still processes the element (sounds play) but renders it
// off-screen.  The scanner reads the latest native x cached here while the
// live element itself remains suppressed at x=9999 in memory.
struct ObjSuppressEntry {
  uint32_t currentXBits;    // IEEE 754 bits of the latest native x float
  DWORD lastRefreshTick;
};
static std::unordered_map<uintptr_t, ObjSuppressEntry> g_objSuppressedElems;
static std::mutex g_objSuppressMutex;
// Fast-path hint for CG_DrawHudElem hook: true when at least one element is
// suppressed. Avoids mutex lock on every CG_DrawHudElem call (~30/frame).
static std::atomic<bool> g_hasObjSuppressed{false};


// CG_DrawHudElem context: set by the detour BEFORE calling original, cleared
// after return.  SEH_StringEd_GetString (which runs inside CG_DrawHudElem) can
// read this to obtain the hudelem_s pointer reliably — no RDI heuristic needed.
// Only valid on the engine's main render thread (single-threaded game loop).
static std::atomic<uintptr_t> g_CGDrawHudElem_ElemPtr{0};

uintptr_t TextHook_GetCGDrawHudElemPtr() {
  return g_CGDrawHudElem_ElemPtr.load(std::memory_order_acquire);
}

// CG_DrawHudElem → R_AddCmdDrawText variable capture.
// Stores the last rendered text per HudElem pointer so we can extract
// engine-substituted &&1/&&2 values from the final English string.
static std::mutex g_hudElemRenderedTextMutex;
static std::unordered_map<uintptr_t, std::string> g_hudElemRenderedText;

void TextHook_CaptureHudElemRenderedText(uintptr_t elemPtr,
                                         const char *text) {
  if (!text || !text[0] || elemPtr == 0) return;
  std::lock_guard<std::mutex> lk(g_hudElemRenderedTextMutex);
  g_hudElemRenderedText[elemPtr] = text;
}

// Extract the integer that replaced &&N in the English rendered text,
// by matching the English template pattern around the placeholder.
// Returns -1 if not found.
int TextHook_GetHudElemVarValue(uintptr_t elemPtr,
                                const std::string &englishTemplate,
                                int placeholderIndex) {
  if (elemPtr == 0) return -1;
  std::string rendered;
  {
    std::lock_guard<std::mutex> lk(g_hudElemRenderedTextMutex);
    auto it = g_hudElemRenderedText.find(elemPtr);
    if (it == g_hudElemRenderedText.end()) return -1;
    rendered = it->second;
  }
  // Find the text surrounding &&N in the English template.
  // e.g. template="[ &&1 Remaining ]" → look for "[ " and " Remaining ]"
  std::string ph = "&&" + std::to_string(placeholderIndex);
  size_t phPos = englishTemplate.find(ph);
  if (phPos == std::string::npos) return -1;
  std::string prefix = englishTemplate.substr(
      phPos >= 4 ? phPos - 4 : 0,
      phPos >= 4 ? 4 : phPos);
  std::string suffix = englishTemplate.substr(
      phPos + ph.size(),
      (std::min)((size_t)12, englishTemplate.size() - phPos - ph.size()));
  // Find prefix in rendered text, then extract number.
  size_t pStart = rendered.find(prefix);
  if (pStart == std::string::npos && !prefix.empty()) return -1;
  size_t numStart = pStart + prefix.size();
  // Skip whitespace
  while (numStart < rendered.size() && rendered[numStart] == ' ') numStart++;
  // Extract digits
  size_t numEnd = numStart;
  while (numEnd < rendered.size() && rendered[numEnd] >= '0' &&
         rendered[numEnd] <= '9') numEnd++;
  if (numEnd == numStart) return -1;
  int val = atoi(rendered.substr(numStart, numEnd - numStart).c_str());
  return val;
}

// --- &&1 value capture via R_AddCmdDrawText text matching ---
struct VarKeyPrefix {
  std::string cleanPrefix; // English text before &&1 (color codes stripped)
  std::string keyName;     // localization key
};
static std::vector<VarKeyPrefix> g_varKeyPrefixes;
static bool g_varPrefixesBuilt = false;
std::mutex g_varCapturedMutex;
std::unordered_map<std::string, int> g_varCapturedValueByKey;

static std::string VarCapture_StripColors(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '^' && i + 1 < s.size() && s[i + 1] >= '0' &&
        s[i + 1] <= '9') {
      ++i;
      continue;
    }
    out += s[i];
  }
  return out;
}

void TextHook_BuildVarCapturePrefixes() {
  const auto &engMap = TranslationStore::KeyToEnglish();
  const auto &korMap = TranslationStore::KeyToKorean();
  g_varKeyPrefixes.clear();
  for (const auto &kv : engMap) {
    const std::string &key = kv.first;
    const std::string &eng = kv.second;
    size_t pos = eng.find("&&1");
    if (pos == std::string::npos) continue;
    auto kit = korMap.find(key);
    if (kit == korMap.end() || kit->second.find("&&1") == std::string::npos)
      continue;
    std::string prefix = VarCapture_StripColors(eng.substr(0, pos));
    if (prefix.length() >= 3) {
      g_varKeyPrefixes.push_back({prefix, key});
    }
  }
  // Sort by prefix length descending (longest match first)
  std::sort(g_varKeyPrefixes.begin(), g_varKeyPrefixes.end(),
            [](const VarKeyPrefix &a, const VarKeyPrefix &b) {
              return a.cleanPrefix.length() > b.cleanPrefix.length();
            });
  g_varPrefixesBuilt = true;
  LogToFile("[VAR-PREFIX] Built " + std::to_string(g_varKeyPrefixes.size()) +
            " capture prefixes for &&1 keys");
}

void TextHook_TryCaptureVarValue(const char *text) {
  if (!text || !text[0]) return;
  if (!g_varPrefixesBuilt || g_varKeyPrefixes.empty()) return;
  // Quick check: does text contain a digit?
  bool hasDigit = false;
  for (const char *p = text; *p; ++p) {
    if (*p >= '0' && *p <= '9') {
      hasDigit = true;
      break;
    }
  }
  if (!hasDigit) return;
  // Strip color codes from drawn text
  std::string clean = VarCapture_StripColors(text);
  for (const auto &vkp : g_varKeyPrefixes) {
    if (clean.length() >= vkp.cleanPrefix.length() &&
        clean.compare(0, vkp.cleanPrefix.length(), vkp.cleanPrefix) == 0) {
      size_t numStart = vkp.cleanPrefix.length();
      while (numStart < clean.size() && clean[numStart] == ' ') numStart++;
      size_t numEnd = numStart;
      while (numEnd < clean.size() && clean[numEnd] >= '0' &&
             clean[numEnd] <= '9')
        numEnd++;
      if (numEnd > numStart) {
        int val = atoi(clean.substr(numStart, numEnd - numStart).c_str());
        {
          std::lock_guard<std::mutex> lk(g_varCapturedMutex);
          int prev = -1;
          auto it = g_varCapturedValueByKey.find(vkp.keyName);
          if (it != g_varCapturedValueByKey.end()) prev = it->second;
          g_varCapturedValueByKey[vkp.keyName] = val;
          if (prev != val) {
            LogToFile("[VAR-CAPTURE] key=" + vkp.keyName +
                      " val=" + std::to_string(val) +
                      (prev >= 0 ? " prev=" + std::to_string(prev) : ""));
          }
        }
      }
      break;
    }
  }
}

int TextHook_GetCapturedVarValue(const std::string &key,
                                  int placeholderIndex) {
  if (placeholderIndex != 1) return -1;
  std::lock_guard<std::mutex> lk(g_varCapturedMutex);
  auto it = g_varCapturedValueByKey.find(key);
  if (it != g_varCapturedValueByKey.end()) return it->second;
  return -1;
}

// Per-key fontScale from ObjRev native HudElem scan (render source).
static std::unordered_map<std::string, float> g_NativeCfgFontScaleByKey;
static std::mutex g_NativeCfgFontScaleMutex;

float TextHook_GetNativeCfgFontScale(const std::string &key) {
  std::lock_guard<std::mutex> lk(g_NativeCfgFontScaleMutex);
  auto it = g_NativeCfgFontScaleByKey.find(key);
  if (it != g_NativeCfgFontScaleByKey.end()) return it->second;
  return 0.0f;
}

// Per-key alpha from ObjRev native HudElem scan (render source).
// Solves: HUDHINT entries have alpha=0 but ObjRev sees real alpha>0.
static std::unordered_map<std::string, float> g_NativeCfgAlphaByKey;
static std::mutex g_NativeCfgAlphaMutex;

float TextHook_GetNativeCfgAlpha(const std::string &key) {
  std::lock_guard<std::mutex> lk(g_NativeCfgAlphaMutex);
  auto it = g_NativeCfgAlphaByKey.find(key);
  if (it != g_NativeCfgAlphaByKey.end()) return it->second;
  return 0.0f;
}

// Per-key fade parameters from ObjRev native HudElem scan.
// Enables HUDHINT synth renderer to reproduce native fadeOverTime pulsing.
struct NativeFadeParams {
  float fromAlpha;   // fromColor alpha (+0x34 >> 24)
  float toAlpha;     // color alpha (+0x30 >> 24)
  int fadeStartTime; // +0x38 (serverTime)
  int fadeTime;      // +0x3C (ms)
  int serverTime;    // serverTime at capture
  unsigned long wallTick; // GetTickCount() at capture
};
static std::unordered_map<std::string, NativeFadeParams> g_NativeFadeByKey;
static std::mutex g_NativeFadeMutex;

void TextHook_SetNativeFadeParams(const std::string &key,
                                   float fromAlpha, float toAlpha,
                                   int fadeStartTime, int fadeTime,
                                   int serverTime) {
  std::lock_guard<std::mutex> lk(g_NativeFadeMutex);
  g_NativeFadeByKey[key] = {fromAlpha, toAlpha, fadeStartTime, fadeTime,
                             serverTime, GetTickCount()};
}

float TextHook_GetNativeFadeAlpha(const std::string &key) {
  std::lock_guard<std::mutex> lk(g_NativeFadeMutex);
  auto it = g_NativeFadeByKey.find(key);
  if (it == g_NativeFadeByKey.end()) return -1.0f;
  const NativeFadeParams &p = it->second;
  if (p.fadeTime <= 0) return p.toAlpha;
  // Estimate current serverTime from wallclock delta.
  // ObjRev captured (serverTime, wallTick); now = wallTick + dt.
  // serverTime advances ~1:1 with real time during gameplay.
  unsigned long nowWall = GetTickCount();
  int dt = (int)(nowWall - p.wallTick);
  int estServerTime = p.serverTime + dt;
  int elapsed = estServerTime - p.fadeStartTime;
  if (elapsed < 0) return p.fromAlpha;
  if (elapsed >= p.fadeTime) return p.toAlpha;
  float t = (float)elapsed / (float)p.fadeTime;
  return p.fromAlpha + (p.toAlpha - p.fromAlpha) * t;
}

// Per-key live pulse alpha from SLC-LIVEBRIDGE.
// Updated every frame by the SLC hook when it captures a HUDHINT key with
// color params.  The rendering pipeline reads this to get the actual native
// pulse oscillation, which may not flow through HintOverlayEntry.color[3]
// due to slot/caller filters in the route-free native intake path.
static std::unordered_map<std::string, std::pair<float, DWORD>>
    g_SlcLivePulseAlpha;
static std::mutex g_SlcLivePulseAlphaMutex;

void TextHook_SetSlcLivePulseAlpha(const std::string &key, float alpha) {
  std::lock_guard<std::mutex> lk(g_SlcLivePulseAlphaMutex);
  g_SlcLivePulseAlpha[key] = {alpha, GetTickCount()};
}

float TextHook_GetSlcLivePulseAlpha(const std::string &key,
                                     DWORD maxAgeMs) {
  std::lock_guard<std::mutex> lk(g_SlcLivePulseAlphaMutex);
  auto it = g_SlcLivePulseAlpha.find(key);
  if (it == g_SlcLivePulseAlpha.end()) return -1.0f;
  const DWORD age = GetTickCount() - it->second.second;
  if (age > maxAgeMs) return -1.0f;
  return it->second.first;
}

void TextHook_SetNativeVarValue(const std::string &key, int val) {
  std::lock_guard<std::mutex> lk(g_varCapturedMutex);
  int prev = -1;
  auto it = g_varCapturedValueByKey.find(key);
  if (it != g_varCapturedValueByKey.end()) prev = it->second;
  g_varCapturedValueByKey[key] = val;
  if (prev != val) {
    LogToFile("[NATIVE-VAR-SET] key=" + key + " val=" + std::to_string(val) +
              (prev >= 0 ? " prev=" + std::to_string(prev) : ""));
  }
}

// Forward declaration — defined later in this TU (TextHook.ObjectiveStatusReverse.inl).
static std::string ObjRev_ReadConfigStringRaw(uint32_t cfgIdx);

// Read live configstring raw text by cfg index (public wrapper).
std::string TextHook_ReadLiveConfigString(uint32_t cfgIdx) {
  return ObjRev_ReadConfigStringRaw(cfgIdx);
}

static bool ObjSuppress_SafeWrite(uintptr_t addr, uint32_t value) {
  if (addr < 0x10000 || addr >= 0x7FFFFFFFFFFF) return false;
  if (IsBadWritePtr((void *)addr, sizeof(uint32_t)) != 0) return false;
  *(volatile uint32_t *)addr = value;
  return true;
}

void TextHook_ObjSuppressElem(uintptr_t elemPtr, uint32_t originalXBits) {
  if (elemPtr < 0x10000) return;
  std::lock_guard<std::mutex> lk(g_objSuppressMutex);
  auto it = g_objSuppressedElems.find(elemPtr);
  if (it != g_objSuppressedElems.end()) {
    // Refresh tick; keep the most recent x provided by the caller until
    // CG_DrawHudElem updates it from the true native value for this frame.
    it->second.currentXBits = originalXBits;
    it->second.lastRefreshTick = GetTickCount();
  } else {
    g_objSuppressedElems[elemPtr] = {originalXBits, GetTickCount()};
  }
  // Apply x=9999 immediately — no deferred ReapplyOffscreen needed.
  // The live native x is refreshed by CG_DrawHudElem before suppression,
  // eliminating the borrow-restore race that caused flicker.
  static const float kOffscreen = 9999.0f;
  uint32_t offBits;
  memcpy(&offBits, &kOffscreen, sizeof(uint32_t));
  ObjSuppress_SafeWrite(elemPtr + 0x04, offBits);
  g_hasObjSuppressed.store(true, std::memory_order_relaxed);
  // Purge stale entries (> 2s without refresh from renderer).
  {
    DWORD now = GetTickCount();
    for (auto it2 = g_objSuppressedElems.begin();
         it2 != g_objSuppressedElems.end();) {
      if (it2->first != elemPtr &&
          (now - it2->second.lastRefreshTick) > 2000)
        it2 = g_objSuppressedElems.erase(it2);
      else
        ++it2;
    }
    if (g_objSuppressedElems.empty()) {
      g_hasObjSuppressed.store(false, std::memory_order_relaxed);
    }
  }
}

bool TextHook_IsObjSuppressedElem(uintptr_t elemPtr) {
  if (elemPtr < 0x10000) return false;
  std::lock_guard<std::mutex> lk(g_objSuppressMutex);
  auto it = g_objSuppressedElems.find(elemPtr);
  if (it == g_objSuppressedElems.end()) return false;
  if ((GetTickCount() - it->second.lastRefreshTick) > 500) {
    return false;
  }
  return true;
}

// Restore latest cached native x for expired elements and remove them.
void TextHook_ObjSuppressRestoreStale() {
  std::lock_guard<std::mutex> lk(g_objSuppressMutex);
  if (g_objSuppressedElems.empty()) return;
  DWORD now = GetTickCount();
  for (auto it = g_objSuppressedElems.begin();
       it != g_objSuppressedElems.end();) {
    if ((now - it->second.lastRefreshTick) > 500) {
      ObjSuppress_SafeWrite(it->first + 0x04, it->second.currentXBits);
      it = g_objSuppressedElems.erase(it);
    } else {
      ++it;
    }
  }
  g_hasObjSuppressed.store(!g_objSuppressedElems.empty(),
                           std::memory_order_relaxed);
}

// Restore all and clear (map load / mission restart).
void TextHook_ObjSuppressClearAll() {
  std::lock_guard<std::mutex> lk(g_objSuppressMutex);
  for (auto &kv : g_objSuppressedElems) {
    ObjSuppress_SafeWrite(kv.first + 0x04, kv.second.currentXBits);
  }
  g_objSuppressedElems.clear();
  g_hasObjSuppressed.store(false, std::memory_order_relaxed);
}

// No-op: borrow-restore pattern removed.  x stays at 9999 permanently;
// the scanner reads the latest cached native x from g_objSuppressedElems instead.
void TextHook_ObjSuppressTempRestore() {
  // Intentionally empty — kept for ABI compatibility.
}

// No-op: x=9999 is now applied immediately in TextHook_ObjSuppressElem.
void TextHook_ObjSuppressReapplyOffscreen() {
  // Intentionally empty — kept for ABI compatibility.
}

// SLC index → localization key mapping (populated on game thread)
static std::unordered_map<uint32_t, std::string> g_slcToKey;
static std::mutex g_slcToKeyMutex;

// Credits guard: activated when the final story subtitle
// (SUBTITLE_SKYWAY_HSH_IMPROUDOFYOU1111 = "I'm proud of you, Logan.") is seen.
// Suppresses HUD hint detection and objective processing during credits.
// Reset on OVL-RESET (checkpoint restart, mission restart, main menu).
std::atomic<bool> g_creditsGuardActive{false};

// Soft guard: SLC raw hook disabled only.  All other hooks (IntroTextLayout,
// IntroRenderFn, Cbuf, R_AddCmdDrawText) remain active.  Activated by
// ui_play_credits SLC detection.  Upgraded to full guard when IntroTextLayout
// sees CREDITS_* keys (credits rendering), or cancelled when IntroTextLayout
// sees non-CREDITS_* keys (gameplay started).
std::atomic<bool> g_creditsSoftGuard{false};


// Legacy Method 1+2 globals (kept to avoid linker errors; no longer used for guard logic).
std::atomic<DWORD>    g_lastBinkVideoCloseTick{0};
std::atomic<DWORD>    g_lastBinkVideoOpenTick{0};
std::atomic<uint64_t> g_presentFrameCount{0};
std::atomic<uint64_t> g_presentFrameAtLastVideoClose{0};




// =============================================================================
// Soft SLC suspend: disable ONLY the SLC raw hook.  All MinHook hooks stay
// active so IntroTextLayout can distinguish credits vs gameplay rendering.
// =============================================================================
static std::atomic<bool> g_slcSoftSuspended{false};

void TextHook_SoftSuspendSLC() {
  if (g_slcSoftSuspended.load(std::memory_order_relaxed))
    return;
  if (g_SLC_HookApplied && g_SLC_TargetAddr) {
    DWORD oldProtect;
    if (VirtualProtect(g_SLC_TargetAddr, 14, PAGE_EXECUTE_READWRITE,
                       &oldProtect)) {
      memcpy(g_SLC_TargetAddr, g_SLC_OriginalBytes, 14);
      VirtualProtect(g_SLC_TargetAddr, 14, oldProtect, &oldProtect);
      FlushInstructionCache(GetCurrentProcess(), g_SLC_TargetAddr, 14);
    }
  }
  g_slcSoftSuspended.store(true, std::memory_order_relaxed);
  LogToFile("[CREDITS-HOOKS] SLC soft-suspended");
}

void TextHook_SoftResumeSLC() {
  if (!g_slcSoftSuspended.load(std::memory_order_relaxed))
    return;
  if (g_SLC_HookApplied && g_SLC_TargetAddr && g_SLC_PreStub) {
    DWORD oldProtect;
    if (VirtualProtect(g_SLC_TargetAddr, 14, PAGE_EXECUTE_READWRITE,
                       &oldProtect)) {
      unsigned char *bytes = (unsigned char *)g_SLC_TargetAddr;
      bytes[0] = 0xFF;
      bytes[1] = 0x25;
      *(uint32_t *)(bytes + 2) = 0;
      *(uint64_t *)(bytes + 6) = (uint64_t)g_SLC_PreStub;
      VirtualProtect(g_SLC_TargetAddr, 14, oldProtect, &oldProtect);
      FlushInstructionCache(GetCurrentProcess(), g_SLC_TargetAddr, 14);
    }
  }
  g_slcSoftSuspended.store(false, std::memory_order_relaxed);
  LogToFile("[CREDITS-HOOKS] SLC soft-resumed");
}

// =============================================================================
// Credits hook suspension: disable ALL hooks during credits to eliminate
// trampoline overhead completely.  Re-enable on overlay reset.
// Cbuf_AddText is kept alive for restart/disconnect detection.
// =============================================================================
static std::atomic<bool> g_hooksSuspendedForCredits{false};

void TextHook_SuspendHooksForCredits() {
  if (g_hooksSuspendedForCredits.load(std::memory_order_relaxed))
    return;

  // 1. Restore SLC original bytes (custom raw hook, not MinHook)
  if (g_SLC_HookApplied && g_SLC_TargetAddr) {
    DWORD oldProtect;
    if (VirtualProtect(g_SLC_TargetAddr, 14, PAGE_EXECUTE_READWRITE,
                       &oldProtect)) {
      memcpy(g_SLC_TargetAddr, g_SLC_OriginalBytes, 14);
      VirtualProtect(g_SLC_TargetAddr, 14, oldProtect, &oldProtect);
      FlushInstructionCache(GetCurrentProcess(), g_SLC_TargetAddr, 14);
    }
  }

  // 2. Disable ALL MinHook hooks, then re-enable essential ones:
  //    - Cbuf_AddText: restart/disconnect detection → credits guard reset
  //    - R_AddCmdDrawText: Korean menu text during credits pause menu
  MH_DisableHook(MH_ALL_HOOKS);
  if (g_Cbuf_AddText_TargetAddr) {
    MH_EnableHook(g_Cbuf_AddText_TargetAddr);
  }
  if (g_TargetAddr) { // R_AddCmdDrawText
    MH_EnableHook(g_TargetAddr);
  }

  g_hooksSuspendedForCredits.store(true, std::memory_order_relaxed);
  LogToFile("[CREDITS-HOOKS] All hooks SUSPENDED for credits");
}

void TextHook_ResumeHooksAfterCredits() {
  if (!g_hooksSuspendedForCredits.load(std::memory_order_relaxed))
    return;

  // 1. Re-apply SLC raw hook
  if (g_SLC_HookApplied && g_SLC_TargetAddr && g_SLC_PreStub) {
    DWORD oldProtect;
    if (VirtualProtect(g_SLC_TargetAddr, 14, PAGE_EXECUTE_READWRITE,
                       &oldProtect)) {
      unsigned char *bytes = (unsigned char *)g_SLC_TargetAddr;
      bytes[0] = 0xFF;
      bytes[1] = 0x25;
      *(uint32_t *)(bytes + 2) = 0;
      *(uint64_t *)(bytes + 6) = (uint64_t)g_SLC_PreStub;
      VirtualProtect(g_SLC_TargetAddr, 14, oldProtect, &oldProtect);
      FlushInstructionCache(GetCurrentProcess(), g_SLC_TargetAddr, 14);
    }
  }

  // 2. Re-enable ALL MinHook hooks
  MH_EnableHook(MH_ALL_HOOKS);

  g_hooksSuspendedForCredits.store(false, std::memory_order_relaxed);
  g_slcSoftSuspended.store(false, std::memory_order_relaxed); // clear soft flag too
  LogToFile("[CREDITS-HOOKS] All hooks RESUMED after credits");
}

struct AuthoritativeObjectiveTextEvent {
  uint32_t cfgIndex = 0;
  uint32_t slcIndex = 0;
  std::string key;
  unsigned int caller = 0;
  unsigned int thread = 0;
  unsigned long tick = 0;
  unsigned long frameStamp = 0;
  int fxBirth = 0;
  int elemIndex = -1;
};

static std::mutex g_objAuthoritativeCaptureMutex;
static std::unordered_map<uint64_t, AuthoritativeObjectiveTextEvent>
    g_objAuthoritativeByInstance;
struct PendingObjectiveAuthorityEvent {
  std::string key;
  uint32_t slcIndex = 0;
  unsigned int caller = 0;
  unsigned int thread = 0;
  unsigned long tick = 0;
  uint32_t ordinal = 0;
};
static std::mutex g_objPendingAuthorityMutex;
static std::deque<PendingObjectiveAuthorityEvent> g_objPendingAuthorityEvents;

struct CfgDirectMapEntry {
  uint32_t slcIdx = 0;
  uint32_t bias = 0;
  uintptr_t callerOffset = 0;
  unsigned long tick = 0;
  uint32_t hits = 0;
  int quality = 0;
  unsigned long objectiveTick = 0;
  uint32_t objectiveHits = 0;
  uint32_t keyClass = 0; // ObjRev_ClassifyConsumerKey: 1=objective, 2=status
};
static std::unordered_map<uint32_t, CfgDirectMapEntry> g_cfgDirectMap;
static std::mutex g_cfgDirectMapMutex;
struct ObjCfgSlcRawObservation {
  uint32_t queryIdx = 0;
  uintptr_t callerOffset = 0;
  unsigned long tick = 0;
};
static std::unordered_map<uint32_t, ObjCfgSlcRawObservation> g_objCfgSlcRawBySlc;
static std::mutex g_objCfgSlcRawMutex;
struct ObjCfgSlcTlsContext {
  uint32_t queryIdx = 0;
  uint32_t slcIdx = 0;
  uintptr_t callerOffset = 0;
  unsigned long tick = 0;
};
static thread_local ObjCfgSlcTlsContext g_objCfgSlcTlsCtx;
static thread_local std::deque<ObjCfgSlcTlsContext> g_objCfgSlcTlsRecent;
struct ObjTypeChkTlsContext {
  uint32_t queryIdx = 0;
  uint32_t resultType = 0;
  uint32_t ordinal = 0;
  uintptr_t callerOffset = 0;
  uintptr_t r12 = 0;
  uintptr_t rsi = 0;
  uintptr_t rbp = 0;
  uintptr_t rsp = 0;
  unsigned long tick = 0;
};
static thread_local ObjTypeChkTlsContext g_objTypeChkTlsCtx;
static std::unordered_map<uint32_t, unsigned long> g_activeObjectiveCfgTick;
static std::mutex g_activeObjectiveCfgMutex;

static bool ObjRev_IsObservedObjectiveKey(const std::string &key,
                                          unsigned long maxAgeMs = 600000) {
  if (key.empty() || IsObjectiveStatusMessageKeyName(key)) {
    return false;
  }
  const unsigned long now = GetTickCount();
  std::lock_guard<std::mutex> lk(g_objSlcObsMutex);
  for (auto it = g_objSlcObservations.rbegin(); it != g_objSlcObservations.rend();
       ++it) {
    if (it->key != key) {
      continue;
    }
    const unsigned long age = (now >= it->tick) ? (now - it->tick) : 0;
    if (age <= maxAgeMs) {
      return true;
    }
    return false;
  }
  return false;
}

static bool ObjRev_IsAuthoritativeObjectiveKey(
    const std::string &key, unsigned long maxAgeMs = 180000) {
  if (key.empty() || IsObjectiveStatusMessageKeyName(key)) {
    return false;
  }
  const unsigned long now = GetTickCount();
  std::lock_guard<std::mutex> lk(g_objAuthoritativeCaptureMutex);
  for (const auto &kv : g_objAuthoritativeByInstance) {
    const AuthoritativeObjectiveTextEvent &cap = kv.second;
    if (cap.key != key) {
      continue;
    }
    // Root rule:
    // strict objective authority must originate from the dedicated objective
    // SLC producer (resolved authority caller). Runtime/lane captures are not
    // identity roots.
    if (!ObjRev_IsObjectiveAuthoritySlcCaller(cap.caller)) {
      continue;
    }
    const unsigned long age = (now >= cap.tick) ? (now - cap.tick) : 0;
    if (age <= maxAgeMs) {
      return true;
    }
  }
  return false;
}

static bool ObjRev_IsObjectiveListKeyStrict(const std::string &key) {
  if (key.empty() || IsObjectiveStatusMessageKeyName(key)) {
    return false;
  }
  bool hasUpper = false;
  for (unsigned char uc : key) {
    if ((uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || uc == '_') {
      if (uc >= 'A' && uc <= 'Z') {
        hasUpper = true;
      }
      continue;
    }
    return false;
  }
  if (!hasUpper) {
    return false;
  }
  // Objective list identity must come from authoritative objective producer
  // evidence, not key-name pattern matching.
  if (ObjRev_IsObservedObjectiveKey(key)) {
    return true;
  }
  if (ObjRev_IsAuthoritativeObjectiveKey(key)) {
    return true;
  }
  return false;
}

static bool ObjRev_IsBulkCfgProducerCaller(uintptr_t callerOffset) {
  if (callerOffset == 0) {
    return false;
  }
  // 0x328Dxx family are high-volume cfg walkers and not objective/status
  // authority producers.
  if ((callerOffset >= IW6Offsets::Profile::Rva_367F60 && callerOffset < IW6Offsets::Profile::Rva_3682F7)) {
    return true;
  }
  // Additional broad producer traversals observed in this build.
  return (callerOffset == IW6Offsets::Profile::Rva_18423B || callerOffset == IW6Offsets::Profile::Rva_183FEC ||
          callerOffset == IW6Offsets::Profile::Rva_1843DB);
}

static bool ObjRev_IsObjectiveConsumerCandidateCaller(uintptr_t callerOffset) {
  // Objective consumer candidates only.
  // Broad producer/bulk traversals are filtered by
  // ObjRev_IsBulkCfgProducerCaller().
  return (callerOffset == IW6Offsets::Profile::Rva_18465B);
}

static bool ObjRev_LooksLikeConsumerLocKey(const std::string &key) {
  if (key.size() < 3 || key.size() > 128) {
    return false;
  }
  bool hasUpper = false;
  for (unsigned char uc : key) {
    if ((uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || uc == '_') {
      if (uc >= 'A' && uc <= 'Z') {
        hasUpper = true;
      }
      continue;
    }
    return false;
  }
  return hasUpper;
}

static uint32_t ObjRev_ClassifyConsumerKey(const std::string &key) {
  if (key.empty()) {
    return 0;
  }
  if (key == "GAME_OBJECTIVESUPDATED" || key == "GAME_OBJECTIVECOMPLETED" ||
      key == "EXE_GAMESAVED" || key == "CGAME_NOW_SAVING") {
    return 2;
  }
  if (ObjRev_IsObjectiveListKeyStrict(key)) {
    return 1;
  }
  if (ObjRev_LooksLikeConsumerLocKey(key)) {
    return 3;
  }
  return 0;
}

static void ObjRev_RecordCfgSlcTlsRecent(uint32_t queryIdx, uint32_t slcIdx,
                                         uintptr_t callerOffset,
                                         unsigned long tick) {
  if (queryIdx == 0 || slcIdx == 0) {
    return;
  }
  ObjCfgSlcTlsContext ev{};
  ev.queryIdx = queryIdx;
  ev.slcIdx = slcIdx;
  ev.callerOffset = callerOffset;
  ev.tick = tick;
  g_objCfgSlcTlsRecent.push_back(ev);
  constexpr size_t kMaxRecent = 64;
  while (g_objCfgSlcTlsRecent.size() > kMaxRecent) {
    g_objCfgSlcTlsRecent.pop_front();
  }
  constexpr unsigned long kKeepMs = 1400;
  while (!g_objCfgSlcTlsRecent.empty()) {
    const auto &front = g_objCfgSlcTlsRecent.front();
    if (tick >= front.tick && (tick - front.tick) > kKeepMs) {
      g_objCfgSlcTlsRecent.pop_front();
    } else {
      break;
    }
  }
}

static bool ObjRev_FindRecentCfgSlcTlsForSlc(uint32_t slcIdx, DWORD now,
                                             DWORD maxAgeMs,
                                             ObjCfgSlcTlsContext &out) {
  out = {};
  if (slcIdx == 0) {
    return false;
  }
  int bestScore = -1000000;
  for (auto it = g_objCfgSlcTlsRecent.rbegin(); it != g_objCfgSlcTlsRecent.rend();
       ++it) {
    if (it->slcIdx == 0 || it->queryIdx == 0) {
      continue;
    }
    const DWORD age = (now >= it->tick) ? (now - it->tick) : 0;
    if (age > maxAgeMs) {
      continue;
    }
    int score = 0;
    if (it->slcIdx == slcIdx) {
      score += 1200;
    }
    if (it->callerOffset == IW6Offsets::Profile::Rva_18465B) {
      score += 220;
    } else if (it->callerOffset == IW6Offsets::Profile::Rva_18477E) {
      score += 210;
    } else if (ObjRev_IsBulkCfgProducerCaller(it->callerOffset)) {
      score -= 120;
    }
    score += (int)((maxAgeMs > age) ? (maxAgeMs - age) : 0);
    if (score > bestScore) {
      bestScore = score;
      out = *it;
    }
  }
  return (bestScore > -1000000 && out.queryIdx != 0);
}

static std::vector<ObjCfgSlcTlsContext> ObjRev_GetRecentCfgSlcTlsList(
    DWORD now, DWORD maxAgeMs, size_t maxItems) {
  std::vector<ObjCfgSlcTlsContext> out;
  if (maxItems == 0) {
    return out;
  }
  out.reserve(maxItems);
  for (auto it = g_objCfgSlcTlsRecent.rbegin(); it != g_objCfgSlcTlsRecent.rend();
       ++it) {
    const DWORD age = (now >= it->tick) ? (now - it->tick) : 0;
    if (age > maxAgeMs) {
      continue;
    }
    out.push_back(*it);
    if (out.size() >= maxItems) {
      break;
    }
  }
  return out;
}

static bool ObjRev_TryJoinTypeCheckForQuery(uint32_t queryIdx, DWORD now,
                                            uint32_t &outType,
                                            uint32_t &outOrdinal,
                                            uintptr_t &outR12,
                                            uintptr_t &outRsi,
                                            uintptr_t &outRbp,
                                            uintptr_t &outRsp) {
  outType = 0;
  outOrdinal = 0;
  outR12 = 0;
  outRsi = 0;
  outRbp = 0;
  outRsp = 0;
  if (queryIdx == 0) {
    return false;
  }
  if (g_objTypeChkTlsCtx.queryIdx != queryIdx ||
      !ObjRev_IsObjectiveAuthorityTypeCheckCaller(
          g_objTypeChkTlsCtx.callerOffset) ||
      g_objTypeChkTlsCtx.tick == 0 || now < g_objTypeChkTlsCtx.tick ||
      (now - g_objTypeChkTlsCtx.tick) > 96) {
    return false;
  }
  outType = g_objTypeChkTlsCtx.resultType;
  outOrdinal = g_objTypeChkTlsCtx.ordinal;
  outR12 = g_objTypeChkTlsCtx.r12;
  outRsi = g_objTypeChkTlsCtx.rsi;
  outRbp = g_objTypeChkTlsCtx.rbp;
  outRsp = g_objTypeChkTlsCtx.rsp;
  return true;
}

static void ObjRev_RecordObjectiveConsumerResolve(uint32_t cfgIdx,
                                                  uint32_t bias,
                                                  uint32_t queryIdx,
                                                  uint32_t slcIdx,
                                                  uintptr_t cfgCallerOffset,
                                                  const char *sourceTag) {
  if (cfgIdx == 0 || slcIdx == 0) {
    return;
  }

  const DWORD now = GetTickCount();
  ObjConsumerResolveEvent ev{};
  ev.cfgIdx = cfgIdx;
  ev.bias = bias;
  ev.queryIdx = queryIdx;
  ev.slcIdx = slcIdx;
  ev.cfgCallerOffset = (unsigned int)cfgCallerOffset;
  ev.tick = now;
  ev.objectiveCaller = ObjRev_IsObjectiveConsumerCandidateCaller(cfgCallerOffset);
  ObjRev_TryJoinTypeCheckForQuery(queryIdx, now, ev.typeValue, ev.ordinal, ev.r12,
                                  ev.rsi, ev.rbp, ev.rsp);
  {
    std::string keyByMap;
    std::string raw = ObjRev_ReadSlcStringRaw(slcIdx);
    const bool hasRawLocKey =
        (!raw.empty() && ObjRev_LooksLikeConsumerLocKey(raw));
    if (TextHook_GetSlcKey(slcIdx, keyByMap) && !keyByMap.empty() &&
        ObjRev_LooksLikeConsumerLocKey(keyByMap)) {
      bool acceptKeyByMap = false;
      if (!IsObjectiveStatusMessageKeyName(keyByMap)) {
        acceptKeyByMap = true;
      } else if (hasRawLocKey && raw == keyByMap) {
        // Best-case corroboration: raw SLC holds the same status key token.
        acceptKeyByMap = true;
      } else {
        // Corroborate status key by the direct cfg->slc map for this cfg.
        // Status raws are often localized display strings, not key tokens.
        uint32_t cfgDirectSlc = 0;
        uint32_t cfgDirectBias = 0;
        if (ObjRev_GetDirectCfgSlc(cfgIdx, cfgDirectSlc, cfgDirectBias, false) &&
            cfgDirectSlc > 1 && cfgDirectSlc == slcIdx) {
          acceptKeyByMap = true;
        }
      }
      if (acceptKeyByMap) {
        ev.keyName = keyByMap;
      }
    }
    if (ev.keyName.empty() && hasRawLocKey) {
      ev.keyName = raw;
    }
    ev.keyClass = ObjRev_ClassifyConsumerKey(ev.keyName);
  }

  // Seed status cfg trust from objective consumer resolve itself.
  // This path is the actual cfg->slc consumer path (+0x1480BB/+0x1481D0).
  // Only accept cfg that are currently active objective lanes to prevent
  // broad consumer scans from widening trust cfg sets.
  const bool cfgActiveNow = ObjRev_IsObjectiveCfgActive(cfgIdx, now);
  if (ev.objectiveCaller && cfgActiveNow && ev.keyClass == 2 &&
      IsObjectiveStatusMessageKeyName(ev.keyName) &&
      cfgIdx > 0 && cfgIdx <= 0xFFFFu) {
    static std::unordered_map<uint64_t, std::pair<DWORD, unsigned int>>
        s_statusConsumeTrustCand;
    const uint64_t sig =
        ((uint64_t)(cfgIdx & 0xFFFFu) << 32) ^
        (uint64_t)(ev.keyName == "GAME_OBJECTIVESUPDATED"
                       ? 1u
                       : (ev.keyName == "GAME_OBJECTIVECOMPLETED"
                              ? 2u
                              : (ev.keyName == "EXE_GAMESAVED" ? 3u : 4u)));
    auto &cand = s_statusConsumeTrustCand[sig];
    const DWORD age = (cand.first != 0 && now >= cand.first) ? (now - cand.first) : 0;
    if (cand.first == 0 || age > 1400) {
      cand.first = now;
      cand.second = 1;
    } else {
      cand.first = now;
      cand.second += 1;
    }
    if (cand.second >= 2) {
      TextHook_RecordObjectiveStatusCfgTrust(ev.keyName, cfgIdx, now,
                                             "obj_consume_cap");
    }
    if (s_statusConsumeTrustCand.size() > 256) {
      for (auto it = s_statusConsumeTrustCand.begin();
           it != s_statusConsumeTrustCand.end();) {
        const DWORD stale =
            (now >= it->second.first) ? (now - it->second.first) : 0;
        if (stale > 8000) {
          it = s_statusConsumeTrustCand.erase(it);
        } else {
          ++it;
        }
      }
    }
  } else if (ev.objectiveCaller && ev.keyClass == 2 &&
             IsObjectiveStatusMessageKeyName(ev.keyName) &&
             cfgIdx > 0 && cfgIdx <= 0xFFFFu && !cfgActiveNow) {
    static std::unordered_map<uint64_t, DWORD> s_statusConsumeSkipInactiveLogTick;
    const uint64_t skipSig =
        ((uint64_t)(cfgIdx & 0xFFFFu) << 32) ^
        (uint64_t)(ev.keyName == "GAME_OBJECTIVESUPDATED"
                       ? 1u
                       : (ev.keyName == "GAME_OBJECTIVECOMPLETED"
                              ? 2u
                              : (ev.keyName == "EXE_GAMESAVED" ? 3u : 4u)));
    auto itSkip = s_statusConsumeSkipInactiveLogTick.find(skipSig);
    if (itSkip == s_statusConsumeSkipInactiveLogTick.end() ||
        (now - itSkip->second) > 2000) {
      s_statusConsumeSkipInactiveLogTick[skipSig] = now;
      char sbuf[320];
      sprintf_s(
          sbuf,
          "[STATUS-CFG-TRUST-SKIP] key=%s cfg=0x%X src=obj_consume_cap reason=cfg_inactive",
          ev.keyName.c_str(), cfgIdx);
      LogToFile(sbuf);
    }
  }

  if (RuntimeFlags_ObjectiveStatusEnableConsumeCap() && ev.objectiveCaller &&
      ev.keyClass == 2 && IsObjectiveStatusMessageKeyName(ev.keyName) &&
      cfgIdx > 0 && cfgIdx <= 0xFFFFu) {
    const bool isSaveStatusKey =
        (ev.keyName == "EXE_GAMESAVED" || ev.keyName == "CGAME_NOW_SAVING");
    const bool canPushByCfgState = cfgActiveNow || isSaveStatusKey;
    if (canPushByCfgState) {
      uint64_t instanceSig =
          ((uint64_t)(cfgIdx & 0xFFFFu) << 32) ^
          (uint64_t)(slcIdx & 0x00FFFFFFu);
      if (instanceSig == 0) {
        instanceSig =
            ((uint64_t)(ev.cfgCallerOffset & 0xFFFFu) << 16) ^
            (uint64_t)(queryIdx & 0xFFFFu);
      }
      TextHook_RecordObjectiveStatusEvent(ev.keyName, (unsigned int)cfgIdx,
                                          "consume_cap", instanceSig);
      static std::unordered_map<uint64_t, DWORD> s_statusConsumePushLogTick;
      const uint64_t pushSig =
          ((uint64_t)(cfgIdx & 0xFFFFu) << 40) ^
          ((uint64_t)(slcIdx & 0xFFFFu) << 24) ^
          (uint64_t)(ev.keyName == "GAME_OBJECTIVESUPDATED"
                         ? 1u
                         : (ev.keyName == "GAME_OBJECTIVECOMPLETED"
                                ? 2u
                                : (ev.keyName == "EXE_GAMESAVED" ? 3u : 4u)));
      auto itPush = s_statusConsumePushLogTick.find(pushSig);
      if (itPush == s_statusConsumePushLogTick.end() ||
          (now - itPush->second) > 1000) {
        s_statusConsumePushLogTick[pushSig] = now;
        char sbuf[320];
        sprintf_s(sbuf,
                  "[STATUS-CONSUME-CAP-PUSH] key=%s cfg=0x%X slc=0x%X caller=+0x%X",
                  ev.keyName.c_str(), cfgIdx, slcIdx, ev.cfgCallerOffset);
        LogToFile(sbuf);
      }
    }
  }

  {
    std::lock_guard<std::mutex> lk(g_objConsumerResolveMutex);
    g_objConsumerResolveEvents.push_back(ev);
    constexpr size_t kMaxEvents = 512;
    constexpr DWORD kRetentionMs = 20000;
    while (g_objConsumerResolveEvents.size() > kMaxEvents) {
      g_objConsumerResolveEvents.pop_front();
    }
    for (auto it = g_objConsumerResolveEvents.begin();
         it != g_objConsumerResolveEvents.end();) {
      if (now >= it->tick && (now - it->tick) > kRetentionMs) {
        it = g_objConsumerResolveEvents.erase(it);
      } else {
        ++it;
      }
    }
  }

  static std::unordered_map<uint64_t, DWORD> s_consumeCapLogTick;
  const uint64_t sig = ((uint64_t)(cfgIdx & 0xFFFFu) << 48) ^
                       ((uint64_t)(slcIdx & 0xFFFFu) << 32) ^
                       ((uint64_t)(queryIdx & 0xFFFFu) << 16) ^
                       (uint64_t)(ev.cfgCallerOffset & 0xFFFFu);
  auto itLog = s_consumeCapLogTick.find(sig);
  if (itLog == s_consumeCapLogTick.end() || (now - itLog->second) > 1200) {
    s_consumeCapLogTick[sig] = now;
    char cbuf[512];
    sprintf_s(
        cbuf,
        "[OBJ-CONSUME-CAP] cfg=0x%X bias=0x%X q=0x%X slc=0x%X caller=+0x%X src=%s type=%u ord=%u oc=%d kcls=%u key=%s",
        cfgIdx, bias, queryIdx, slcIdx, ev.cfgCallerOffset,
        sourceTag ? sourceTag : "unknown", ev.typeValue, ev.ordinal,
        ev.objectiveCaller ? 1 : 0, ev.keyClass,
        ev.keyName.empty() ? "(none)" : ev.keyName.c_str());
    LogToFile(cbuf);
  }
}

static bool ObjRev_GetRecentObjectiveConsumerResolve(
    uint32_t cfgIdx, DWORD maxAgeMs, ObjConsumerResolveEvent &outEvent) {
  outEvent = ObjConsumerResolveEvent{};
  if (cfgIdx == 0) {
    return false;
  }
  const DWORD now = GetTickCount();
  std::lock_guard<std::mutex> lk(g_objConsumerResolveMutex);
  for (auto it = g_objConsumerResolveEvents.rbegin();
       it != g_objConsumerResolveEvents.rend(); ++it) {
    if (it->cfgIdx != cfgIdx) {
      continue;
    }
    const DWORD age = (now >= it->tick) ? (now - it->tick) : 0;
    if (age > maxAgeMs) {
      continue;
    }
    if (!it->objectiveCaller) {
      continue;
    }
    if (it->keyName.empty() || it->keyClass == 0) {
      continue;
    }
    outEvent = *it;
    return true;
  }
  return false;
}

static bool ObjRev_GetRecentStatusConsumerResolveForKey(
    const std::string &statusKey, uint32_t preferredSlc, DWORD maxAgeMs,
    ObjConsumerResolveEvent &outEvent, bool requireActiveCfg = true) {
  outEvent = ObjConsumerResolveEvent{};
  if (statusKey.empty() || !IsObjectiveStatusMessageKeyName(statusKey)) {
    return false;
  }
  const DWORD now = GetTickCount();
  std::lock_guard<std::mutex> lk(g_objConsumerResolveMutex);
  for (auto it = g_objConsumerResolveEvents.rbegin();
       it != g_objConsumerResolveEvents.rend(); ++it) {
    if (!it->objectiveCaller || it->keyClass != 2 ||
        it->cfgIdx == 0 || it->cfgIdx > 0xFFFFu) {
      continue;
    }
    if (it->keyName != statusKey) {
      continue;
    }
    if (requireActiveCfg && !ObjRev_IsObjectiveCfgActive(it->cfgIdx, now)) {
      continue;
    }
    const DWORD age = (now >= it->tick) ? (now - it->tick) : 0;
    if (age > maxAgeMs) {
      continue;
    }
    if (preferredSlc > 1 && it->slcIdx != preferredSlc) {
      continue;
    }
    outEvent = *it;
    return true;
  }
  return false;
}

std::vector<HudHintConsumerSample> TextHook_GetRecentHudHintConsumerSamples(
    unsigned long maxAgeMs, unsigned int maxItems) {
  std::vector<HudHintConsumerSample> out;
  if (maxItems == 0) {
    return out;
  }
  if (maxItems > 128) {
    maxItems = 128;
  }
  const DWORD now = GetTickCount();
  std::unordered_set<std::string> seen;
  seen.reserve((size_t)maxItems * 2 + 8);

  std::lock_guard<std::mutex> lk(g_objConsumerResolveMutex);
  for (auto it = g_objConsumerResolveEvents.rbegin();
       it != g_objConsumerResolveEvents.rend(); ++it) {
    const ObjConsumerResolveEvent &ev = *it;
    if (!ev.objectiveCaller || ev.keyName.empty()) {
      continue;
    }
    const DWORD age = (now >= ev.tick) ? (now - ev.tick) : 0;
    if (age > maxAgeMs) {
      continue;
    }
    if (ev.keyClass == 2 || IsObjectiveStatusMessageKeyName(ev.keyName)) {
      continue;
    }
    std::string hudReason;
    if (!IsGameplayHudKeyName(ev.keyName, &hudReason)) {
      continue;
    }
    const std::string sig =
        ev.keyName + "|" + std::to_string(ev.cfgCallerOffset);
    if (seen.find(sig) != seen.end()) {
      continue;
    }
    seen.insert(sig);

    HudHintConsumerSample sample{};
    sample.key = ev.keyName;
    sample.callerOffset = ev.cfgCallerOffset;
    sample.cfgIndex = ev.cfgIdx;
    sample.slcIndex = ev.slcIdx;
    sample.queryIndex = ev.queryIdx;
    sample.tick = ev.tick;
    out.push_back(std::move(sample));
    if (out.size() >= maxItems) {
      break;
    }
  }
  return out;
}

// Live HudElem snapshot (struct defined in TextHook.h)
static std::vector<LiveHudElem> g_liveHudElems;
static std::mutex g_liveHudElemsMutex;
static unsigned long g_liveHudElemsLastUpdate = 0;
static std::vector<LiveHudScriptCountdown> g_liveHudScriptCountdowns;
static std::mutex g_liveHudScriptCountdownMutex;
#endif // GHOSTSKOR_OBJECTIVE_REVERSE

// =============================================================================
// HUD_DrawText (0x3FE560) - Text draw function (HUD hints / non-intro text)
// Hooked to correlate virtual coords (from IntroRenderFn HudElem) with
// screen coords, deriving the engine's coordinate transform formula.
// stolenBytes=15: MOV [RSP+8],RBX(5) + MOV [RSP+10],RBP(5) + MOV [RSP+18],RSI(5)
// Params (x64 ABI): RCX=p1, RDX=p2, R8=p3, R9=p4, stack=[5th+]
// Screen coords are among the stack float params - dump all to identify.
// =============================================================================
// Use void* for all params to ensure 8-byte passthrough on x64 stack.
// Register params (1-4): RCX, RDX, R8, R9 ??all pointers or ints (not float).
// Stack params (5+): copied verbatim preserving bit patterns (float or int).
typedef void (*HUD_DrawText560_t)(void *p1, void *p2, void *p3, void *p4,
                                   void *p5, void *p6, void *p7, void *p8,
                                   void *p9, void *p10, void *p11, void *p12);
static HUD_DrawText560_t Original_HUD_DrawText560 = nullptr;
static bool g_HDT_Hooked = false;

// =============================================================================
// TimeScript HUD draw (module+0x555EA0) - dedicated countdown HUD structure
// Uses RBX as the active HUD struct instead of the prompt/objective systems.
// We hook it with a pre/post assembly stub so we can:
//   1. read RBX + 0x50/+0x1C/+0x0C/+0x10/+0x14 directly
//   2. hide the native English just for this draw
//   3. queue a Korean overlay with the live timer tail
// =============================================================================
struct TimeScriptCallerRegs {
  uintptr_t rsp;
  uintptr_t rbx;
  uintptr_t rbp;
  uintptr_t rdi;
  uintptr_t rsi;
  uintptr_t r12;
  uintptr_t r13;
  uintptr_t r14;
  uintptr_t r15;
  uintptr_t ret;
};
static TimeScriptCallerRegs g_TimeScriptCallerRegs = {};
static void *Original_TimeScriptDraw = nullptr;
static void *g_TimeScriptPreStub = nullptr;
static bool g_TimeScriptHooked = false;

// OSREV chain hook globals removed — OSAUTH is the sole authority.

// Last slcIdx processed by IntroRenderFn (persists after detour returns,
// so the convergence code's call to 0x3FE560 can be matched to the slcIdx)
static volatile int g_lastIntroSlcIdx = -1;

// =============================================================================
// IRF_Sub1 (0x1F0CE0) - Called 3x from IntroRenderFn
// Likely the actual text render/coord-transform function for HUD elements.
// Hooking with generic 12-param signature to identify parameter layout.
// =============================================================================
typedef void (*IRFSub1_t)(void *p1, void *p2, void *p3, void *p4,
                           void *p5, void *p6, void *p7, void *p8,
                           void *p9, void *p10, void *p11, void *p12);
static IRFSub1_t Original_IRFSub1 = nullptr;
static bool g_IRFSub1_Hooked = false;

// Subtitle wrapper discovered from runtime stack trace:
// +0x2351E0 calls SEH_StringEd_GetString(+0x3F42D0) then forwards to +0x235620.
// Hook this to capture enqueue-time parameters (possible native timing fields).
typedef uintptr_t(__fastcall *SubtitleWrap2351E0_t)(int a1, const char *key,
                                                    int a3, int a4);
static SubtitleWrap2351E0_t Original_SubtitleWrap2351E0 = nullptr;
static bool g_SubtitleWrap2351E0_Hooked = false;

// +0x235620 is called by +0x2351E0 as:
//   ECX=a1, EDX=4, R8=localizedText, R9=a3, [rsp+20]=a4, [rsp+28]=0x20
// This is a strong candidate for native subtitle queue lifecycle.
typedef uintptr_t(__fastcall *SubtitleEnqueue235620_t)(
    int a1, int windowId, const char *localizedText, int a3, int a4, int a6);
static SubtitleEnqueue235620_t Original_SubtitleEnqueue235620 = nullptr;
static bool g_SubtitleEnqueue235620_Hooked = false;

struct SubtitleWrapCallCtx {
  bool active = false;
  std::string key;
  int a1 = 0;
  int a3 = 0;
  int a4 = 0;
  DWORD tick = 0;
};

static thread_local SubtitleWrapCallCtx g_SubWrapCallCtx;



// =============================================================================
// IRF_Sub2 (0x6275A0) - Called 2x from IntroRenderFn
// Prologue: 48 83 EC 58 = SUB RSP, 0x58 (no register saves before this)
// Then: 45 33 C0 = XOR R8D, R8D | 0F 28 C8 = MOVAPS XMM1, XMM0
// This function receives a float in XMM0. Might be the draw call.
// =============================================================================
typedef void (*IRFSub2_t)(void *p1, void *p2, void *p3, void *p4,
                           void *p5, void *p6, void *p7, void *p8);
static IRFSub2_t Original_IRFSub2 = nullptr;
static bool g_IRFSub2_Hooked = false;





// Implemented later in this file; used to trigger a "late" dvar dump from the
// subtitle enqueue hook once windowId=4 is observed.
static void DumpSubtitleDvarCandidatesLateOnce();



// Per-slcIdx screen coordinates captured from 0x3FE560
// NOTE: This path is HUD hint/object text, not dialogue subtitles.
struct ScreenCoord {
  float x, y;
  float xScale, yScale;
  unsigned long tick;
};
static std::unordered_map<int, ScreenCoord> g_introScreenCoords;
static std::mutex g_screenCoordMutex;
























// Thread-safe storage for translated strings (to avoid returning dangling
// pointers)
#define MAX_TRANSLATED_STRINGS 256
#define MAX_TRANSLATED_LEN 1024
static char g_TranslatedStrings[MAX_TRANSLATED_STRINGS][MAX_TRANSLATED_LEN];
static std::atomic<int> g_TranslatedIndex{0};



// =============================================================================
// Cross-reference maps (Phase 6: moved up for FindOverlayTranslation chain)
// =============================================================================
static std::unordered_map<std::string, std::string> g_EnglishToKey;
static std::unordered_map<std::string, std::string> g_KeyToKorean;
static std::unordered_map<std::string, std::string> g_KeyToEnglish;
static std::unordered_map<std::string, std::string> g_RuntimeEnglishToKey;
static std::unordered_map<std::string, std::string> g_RuntimeBindingTemplateToKey;
static std::unordered_map<std::string, std::string> g_RuntimeKeyToEnglish;
static std::unordered_map<std::string, DWORD> g_RuntimeKeyLastSeenTick;
static std::shared_mutex g_RuntimeEnglishMutex;

// =============================================================================
// ENGLISH->KOREAN OVERLAY MAP - For HUD translations from FSHook LOCALIZE
// =============================================================================
static std::unordered_map<std::string, std::string> g_EngToKorOverlayMap;
static std::shared_mutex g_OverlayMapMutex;

struct HudScriptCountdownMatch {
  std::string key;
  std::string englishPrefix;
  std::string koreanPrefix;
  std::string tail;
  std::string renderText;
};

static bool IsLikelyHudScriptCountdownTail(const std::string &rawTail) {
  const std::string tail = TrimSpaces(rawTail);
  if (tail.size() < 2 || tail.size() > 16) {
    return false;
  }

  int digits = 0;
  int colons = 0;
  int dots = 0;
  for (char c : tail) {
    const unsigned char uc = (unsigned char)c;
    if (std::isdigit(uc)) {
      digits++;
      continue;
    }
    if (c == ':') {
      colons++;
      continue;
    }
    if (c == '.' || c == ',') {
      dots++;
      continue;
    }
    return false;
  }

  if (digits < 2 || colons > 2 || dots > 1) {
    return false;
  }
  if (tail.front() == ':' || tail.back() == ':' || tail.back() == '.' ||
      tail.back() == ',') {
    return false;
  }
  return (colons > 0 || dots > 0);
}

static bool TryResolveHudScriptCountdownText(const std::string &rawText,
                                             HudScriptCountdownMatch &out) {
  out = {};

  const std::string cleaned = TrimSpaces(StripColorCodes(rawText));
  if (cleaned.empty()) {
    return false;
  }

  EnsureTranslationsLoaded();

  auto TrimRightAscii = [](const std::string &s) -> std::string {
    size_t end = s.size();
    while (end > 0 && std::isspace((unsigned char)s[end - 1])) {
      --end;
    }
    return s.substr(0, end);
  };
  auto TrimLeftAscii = [](const std::string &s) -> std::string {
    size_t start = 0;
    while (start < s.size() && std::isspace((unsigned char)s[start])) {
      ++start;
    }
    return s.substr(start);
  };

  static const std::array<const char *, 4> kCountdownKeys = {
      "CLOCKWORK_POWERDOWN", "CLOCKWORK_EXFIL", "SATFARM_TIME_IMACT",
      "EXE_DOWNLOADING"};

  const std::string cleanedUpper = ToUpperAscii(cleaned);
  for (const char *keyName : kCountdownKeys) {
    auto itEng = g_KeyToEnglish.find(keyName);
    auto itKor = g_KeyToKorean.find(keyName);
    if (itEng == g_KeyToEnglish.end() || itKor == g_KeyToKorean.end() ||
        itEng->second.empty() || itKor->second.empty()) {
      continue;
    }

    const std::string englishPrefix = StripColorCodes(itEng->second);
    const std::string englishPrefixTrim = TrimRightAscii(englishPrefix);

    std::string usedPrefix;
    std::string tail;

    const std::string englishUpper = ToUpperAscii(englishPrefix);
    const bool allowDroppedLead =
        (strcmp(keyName, "CLOCKWORK_POWERDOWN") == 0);
    if (!englishUpper.empty() && cleanedUpper.rfind(englishUpper, 0) == 0) {
      usedPrefix = englishPrefix;
      tail = cleaned.substr(englishPrefix.size());
    } else {
      const std::string englishTrimUpper = ToUpperAscii(englishPrefixTrim);
      bool matched = false;
      size_t consumed = 0;
      if (!englishTrimUpper.empty() &&
          cleanedUpper.rfind(englishTrimUpper, 0) == 0) {
        usedPrefix = englishPrefixTrim;
        consumed = englishPrefixTrim.size();
        matched = true;
      } else if (allowDroppedLead && englishTrimUpper.size() > 4) {
        const std::string englishTrimUpperDropped = englishTrimUpper.substr(1);
        if (cleanedUpper.rfind(englishTrimUpperDropped, 0) == 0) {
          usedPrefix = englishPrefixTrim;
          consumed = englishPrefixTrim.size() - 1;
          matched = true;
        }
      }
      if (!matched) {
        continue;
      }
      tail = cleaned.substr(consumed);
    }

    tail = TrimLeftAscii(tail);
    if (!IsLikelyHudScriptCountdownTail(tail)) {
      continue;
    }

    std::string renderText = itKor->second;
    if (!renderText.empty() &&
        !std::isspace((unsigned char)renderText.back())) {
      renderText.push_back(' ');
    }
    renderText += tail;

    out.key = keyName;
    out.englishPrefix = usedPrefix;
    out.koreanPrefix = itKor->second;
    out.tail = tail;
    out.renderText = renderText;
    return true;
  }

  return false;
}

static bool BuildHudScriptCountdownMatchForKeyTail(const std::string &key,
                                                   const std::string &rawTail,
                                                   HudScriptCountdownMatch &out) {
  out = {};

  const std::string tail = TrimSpaces(StripColorCodes(rawTail));
  if (!IsLikelyHudScriptCountdownTail(tail)) {
    return false;
  }

  EnsureTranslationsLoaded();

  auto itEng = g_KeyToEnglish.find(key);
  auto itKor = g_KeyToKorean.find(key);
  if (itEng == g_KeyToEnglish.end() || itKor == g_KeyToKorean.end() ||
      itEng->second.empty() || itKor->second.empty()) {
    return false;
  }

  std::string renderText = itKor->second;
  if (!renderText.empty() &&
      !std::isspace((unsigned char)renderText.back())) {
    renderText.push_back(' ');
  }
  renderText += tail;

  out.key = key;
  out.englishPrefix = StripColorCodes(itEng->second);
  out.koreanPrefix = itKor->second;
  out.tail = tail;
  out.renderText = renderText;
  return true;
}

static bool IsHudScriptCountdownKeyName(const std::string &key) {
  return key == "CLOCKWORK_POWERDOWN" || key == "CLOCKWORK_EXFIL" ||
         key == "SATFARM_TIME_IMACT" || key == "EXE_DOWNLOADING";
}

static bool IsTimeScriptManagedCountdownKeyName(const std::string &key) {
  return key == "CLOCKWORK_POWERDOWN" || key == "CLOCKWORK_EXFIL" ||
         key == "SATFARM_TIME_IMACT";
}

static bool IsHudElemTimerType(uint32_t type) {
  switch (type) {
    case 0x5:
    case 0x6:
    case 0x7:
    case 0x8:
    case 0x9:
    case 0xA:
    case 0xB:
    case 0xC:
      return true;
    default:
      return false;
  }
}

static bool TryResolveHudScriptCountdownKeyFromRawText(const std::string &rawText,
                                                       std::string &outKey) {
  outKey.clear();

  const std::string cleaned = TrimSpaces(StripColorCodes(rawText));
  if (cleaned.empty()) {
    return false;
  }

  if (LooksLikeHudLocalizationKey(cleaned) &&
      IsHudScriptCountdownKeyName(cleaned)) {
    outKey = cleaned;
    return true;
  }

  std::string resolvedKey;
  if (ResolveKeyFromEnglishInternal(cleaned, resolvedKey) &&
      IsHudScriptCountdownKeyName(resolvedKey)) {
    outKey = resolvedKey;
    return true;
  }

  EnsureTranslationsLoaded();

  auto TrimRightAscii = [](const std::string &s) -> std::string {
    size_t end = s.size();
    while (end > 0 && std::isspace((unsigned char)s[end - 1])) {
      --end;
    }
    return s.substr(0, end);
  };

  const std::string cleanedUpper = ToUpperAscii(cleaned);
  static const std::array<const char *, 4> kCountdownKeys = {
      "CLOCKWORK_POWERDOWN", "CLOCKWORK_EXFIL", "SATFARM_TIME_IMACT",
      "EXE_DOWNLOADING"};
  for (const char *keyName : kCountdownKeys) {
    auto itEng = g_KeyToEnglish.find(keyName);
    if (itEng == g_KeyToEnglish.end() || itEng->second.empty()) {
      continue;
    }
    const std::string englishPrefix =
        TrimRightAscii(StripColorCodes(itEng->second));
    if (englishPrefix.empty()) {
      continue;
    }
    const std::string englishUpper = ToUpperAscii(englishPrefix);
    if (cleanedUpper == englishUpper) {
      outKey = keyName;
      return true;
    }
    if (strcmp(keyName, "CLOCKWORK_POWERDOWN") == 0 &&
        englishUpper.size() > 4 && cleanedUpper == englishUpper.substr(1)) {
      outKey = keyName;
      return true;
    }
  }

  return false;
}

static bool TryResolveHudScriptCountdownKeyFromCfg(uint32_t cfgIdx,
                                                   std::string &outKey) {
  outKey.clear();
  if (!ObjRev_IsPlausibleConfigIndex(cfgIdx)) {
    return false;
  }

  std::string resolvedKey = ObjRev_ResolveConfigKey(cfgIdx, false, true);
  if (resolvedKey.empty()) {
    resolvedKey = ObjRev_ResolveConfigKey(cfgIdx, false, false);
  }
  if (!IsHudScriptCountdownKeyName(resolvedKey)) {
    return false;
  }

  outKey = resolvedKey;
  return true;
}

static bool FormatHudScriptCountdownTailFromSeconds(float remainingSeconds,
                                                    std::string &outTail);

static bool TryResolveHudScriptCountdownFromHudElem(
    uint32_t hudElemType, uint32_t labelSlc, uint32_t textSlc, uintptr_t textPtr,
    float timerValue, std::string &outRawText, HudScriptCountdownMatch &outMatch) {
  outRawText.clear();
  outMatch = {};

  std::string rawText;
  if (textPtr > 0x10000 && textPtr < 0x7FFFFFFFFFFF) {
    TryReadCStringPreview((const char *)textPtr, 160, rawText);
  }

  if (!rawText.empty()) {
    HudScriptCountdownMatch directMatch{};
    if (TryResolveHudScriptCountdownText(rawText, directMatch)) {
      outRawText = rawText;
      outMatch = std::move(directMatch);
      return true;
    }

    std::string rawKey;
    std::string tail;
    if (timerValue > 0.05f &&
        TryResolveHudScriptCountdownKeyFromRawText(rawText, rawKey) &&
        FormatHudScriptCountdownTailFromSeconds(timerValue, tail) &&
        BuildHudScriptCountdownMatchForKeyTail(rawKey, tail, outMatch)) {
      outRawText = rawText;
      return true;
    }
  }

  if (!IsHudElemTimerType(hudElemType) || !(timerValue > 0.05f)) {
    return false;
  }

  std::string countdownKey;
  if (!TryResolveHudScriptCountdownKeyFromCfg(textSlc, countdownKey) &&
      !TryResolveHudScriptCountdownKeyFromCfg(labelSlc, countdownKey)) {
    return false;
  }

  std::string tail;
  if (!FormatHudScriptCountdownTailFromSeconds(timerValue, tail) ||
      !BuildHudScriptCountdownMatchForKeyTail(countdownKey, tail, outMatch)) {
    return false;
  }

  outRawText = rawText.empty() ? countdownKey : rawText;
  return true;
}

static bool FormatHudScriptCountdownTailFromSeconds(float remainingSeconds,
                                                    std::string &outTail) {
  outTail.clear();
  if (!std::isfinite(remainingSeconds)) {
    return false;
  }

  if (remainingSeconds < 0.0f) {
    remainingSeconds = 0.0f;
  }
  if (remainingSeconds > 36000.0f) {
    return false;
  }

  const int totalTenths =
      (int)std::floor((double)remainingSeconds * 10.0 + 0.0001);
  if (totalTenths < 0) {
    return false;
  }

  const int totalSeconds = totalTenths / 10;
  const int minutes = totalSeconds / 60;
  const int seconds = totalSeconds % 60;
  const int tenths = totalTenths % 10;

  char buf[32];
  sprintf_s(buf, "%d:%02d.%d", minutes, seconds, tenths);
  outTail = buf;
  return true;
}

static void QueueHudScriptCountdownOverlay(const HudScriptCountdownMatch &match,
                                           float screenX, float screenY,
                                           float scale, float fontHeight,
                                           int style, const float *color,
                                           bool yIsTopOfText,
                                           unsigned int callerOffset,
                                           const char *sourceTag,
                                           int forceAlign = 0) {
  if (match.renderText.empty() || !std::isfinite(screenX) ||
      !std::isfinite(screenY)) {
    return;
  }

  auto Clamp01Local = [](float v) -> float {
    if (v < 0.0f)
      return 0.0f;
    if (v > 1.0f)
      return 1.0f;
    return v;
  };

  extern std::atomic<bool> g_bQueueFromMenu;
  extern std::atomic<bool> g_bQueueKeepDuringVideo;
  g_bQueueFromMenu.store(false);
  g_bQueueKeepDuringVideo.store(false);

  float finalColor[4] = {0.8f, 1.0f, 0.8f, 1.0f};
  if (color) {
    finalColor[0] = Clamp01Local(color[0]);
    finalColor[1] = Clamp01Local(color[1]);
    finalColor[2] = Clamp01Local(color[2]);
    finalColor[3] = Clamp01Local(color[3]);
  }

  float finalScale = scale;
  if (!std::isfinite(finalScale) || finalScale <= 0.01f || finalScale > 6.0f) {
    finalScale = 1.0f;
  }

  float finalFontHeight = fontHeight;
  if (!std::isfinite(finalFontHeight) || finalFontHeight <= 4.0f ||
      finalFontHeight > 240.0f) {
    finalFontHeight = 42.0f;
  }

  DrawStylePatch stylePatch{};
  stylePatch.applyGlow = true;
  stylePatch.glowR = 0.30f;
  stylePatch.glowG = 0.60f;
  stylePatch.glowB = 0.30f;
  stylePatch.glowA = (finalColor[3] > 0.01f) ? finalColor[3] : 1.0f;

  std::string englishFull = match.englishPrefix;
  if (!englishFull.empty() &&
      !std::isspace((unsigned char)englishFull.back())) {
    englishFull.push_back(' ');
  }
  englishFull += match.tail;
  const float englishWidth = KoreanRenderer::MeasureTextWidthEx(
      englishFull, finalFontHeight, finalScale, 1);

  KoreanRenderer::QueueText(
      match.renderText, screenX, screenY, finalScale, finalColor,
      finalFontHeight, style, englishWidth, false, false, finalScale, false,
      false, true, forceAlign, 0.0f, false, yIsTopOfText, 1, &stylePatch);

  static std::mutex s_timerLogMtx;
  static std::unordered_map<std::string, DWORD> s_timerLogTick;
  const DWORD now = GetTickCount();
  const std::string sig = std::string(sourceTag ? sourceTag : "?") + "|" +
                          match.key + "|" + match.tail + "|" +
                          std::to_string(callerOffset);
  bool shouldLog = false;
  {
    std::lock_guard<std::mutex> lock(s_timerLogMtx);
    auto it = s_timerLogTick.find(sig);
    if (it == s_timerLogTick.end() || (now - it->second) > 900) {
      s_timerLogTick[sig] = now;
      shouldLog = true;
    }
    if (s_timerLogTick.size() > 256) {
      for (auto it = s_timerLogTick.begin(); it != s_timerLogTick.end();) {
        if ((now - it->second) > 30000) {
          it = s_timerLogTick.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  if (shouldLog) {
    char buf[512];
    sprintf_s(buf,
              "[HUD-TIMER-QUEUE] src=%s caller=+0x%X key=%s x=%.1f y=%.1f scale=%.3f fontH=%.1f a=%.2f text=\"%.96s\"",
              sourceTag ? sourceTag : "?", callerOffset, match.key.c_str(),
              screenX, screenY, finalScale, finalFontHeight, finalColor[3],
              match.renderText.c_str());
    LogToFile(buf);
  }
}















// =============================================================================
// HUD RUNTIME CONTAINERS
// =============================================================================
// ─── Hint Pipeline (Phase 3) ────────────────────────────────────────────────
static std::vector<HintOverlayEntry> g_HintEntries;
static std::mutex g_HintMutex;
static constexpr DWORD HINT_ENTRY_TIMEOUT = 8000;
static constexpr size_t HINT_ENTRY_MAX = 32;

// ─── ObjHudHint Pipeline (Phase 4) ─────────────────────────────────────────
static std::vector<ObjHudHintEntry> g_ObjHudHintEntries;
static std::mutex g_ObjHudHintMutex;
static constexpr DWORD OBJHUDHINT_ENTRY_TIMEOUT = 8000;
static constexpr size_t OBJHUDHINT_ENTRY_MAX = 32;

struct HudRuntimeTailFragment {
  std::string key;
  std::string text;
  float x;
  float y;
  unsigned int callerOffset;
  DWORD tick;
};

static std::deque<HudRuntimeTailFragment> g_HudRuntimeTailFragments;
static std::mutex g_HudRuntimeTailMutex;
static constexpr DWORD HUD_RUNTIME_TAIL_TTL_MS = 500;
static constexpr size_t HUD_RUNTIME_TAIL_MAX = 96;

// Objective-only runtime track (native-first). State ownership lives in
// ObjectiveRuntime; TextHook only publishes/consumes via that boundary.
static constexpr DWORD OBJECTIVE_ENTRY_TIMEOUT = 180000;
static constexpr size_t OBJECTIVE_ENTRY_MAX = 96;

// Unified overlay event/claim bus (Objective/Hint/ObjHudHint ownership).
static std::deque<OverlayDrawEvent> g_OverlayEventBus;
static std::mutex g_OverlayBusMutex;
static std::vector<OverlayClaim> g_OverlayClaims;
static uint64_t g_OverlayClaimFrameId = 0;
static OverlaySessionState g_OverlaySessionState = OVERLAY_SESSION_GAMEPLAY;
static OverlaySessionState g_OverlayPrevSessionState = OVERLAY_SESSION_GAMEPLAY;
static uint64_t g_OverlaySkipRenderFrame = 0;
struct OverlayTrackHistory {
  OverlayTrack track = OVERLAY_TRACK_HINT;
  DWORD tick = 0;
};
static std::unordered_map<unsigned int, OverlayTrackHistory>
    g_OverlayCallerTrackHistory;
static std::unordered_map<int, OverlayTrackHistory> g_OverlayLaneTrackHistory;


























// =============================================================================
// COLOR CACHE - For tracking hover color changes
// =============================================================================
struct TextColorEntry {
  float r, g, b, a;
  DWORD lastUpdate;
  bool valid;
};

#define COLOR_CACHE_SIZE 256
static TextColorEntry g_ColorCache[COLOR_CACHE_SIZE];
static bool g_ColorCacheInitialized = false;


// Suffix Suppression State
static DWORD s_LastTranslatedTime = 0;
static float s_LastTranslatedY = 0.0f;


// =============================================================================

// LOCK-FREE STRING QUEUE - No std::string in hot path

// =============================================================================

#define MAX_QUEUE_SIZE 256

#define MAX_STRING_LEN 256

struct StringEntry {

  char text[MAX_STRING_LEN];

  std::atomic<bool> ready;
};

static StringEntry g_Queue[MAX_QUEUE_SIZE];

static std::atomic<int> g_QueueHead{0};

static std::atomic<int> g_LogCount{0};

static constexpr bool kEnableRuntimeTextCapture = false;
static std::atomic<bool> g_LoggingEnabled{kEnableRuntimeTextCapture};


// Filter 1: Hot Path Ring Buffer (Thread-safe enough for loose deduplication)

// We don't need perfect thread safety here, just enough to catch 99% of 60fps

// spam.

static uint32_t g_RecentHashes[64] = {0};

static std::atomic<int> g_RecentHashIndex{0};




// Logger thread - processes queue (Session Deduplication)

#include <set>



#include "vendor/nlohmann/json.hpp" // Use the stub

#include <fstream>

// ...

// =============================================================================

// HELPER TYPES

// =============================================================================

// =============================================================================

// HELPER FUNCTIONS

// =============================================================================

// Expected prologue bytes - R_AddCmdDrawText should start with:
// 48 89 6C 24 18  mov [rsp+18h], rbp
// 56              push rsi
static const unsigned char EXPECTED_PROLOGUE[] = {0x48, 0x89, 0x6C,
                                                  0x24, 0x18, 0x56};





// Dynamic Translation Map

// Cross-Reference Mapping
// (g_EnglishToKey, g_KeyToKorean, g_KeyToEnglish declared earlier near FindOverlayTranslation)

// Prefix map for fragment matching (first 35 chars of long strings -> key)
static std::unordered_map<std::string, std::string> g_PrefixToKey;

// Suffix set for suppressing second fragments (e.g., "killstreaks, and much
// more online.")
static std::set<std::string> g_SuffixToSuppress;

// Binding template matching for English text with [{+command}] placeholders.
// Engine substitutes [{+command}] with actual key names before rendering,
// so "Press [{+changezoom}] to toggle zoom." becomes "Press E to toggle zoom."
// Templates store the text parts before/after the placeholder for matching.
struct BindingTemplate {
  std::string before;  // uppercase text before [{+...}]
  std::string after;   // uppercase text after [{+...}]
  std::string key;     // localization key
};
static std::vector<BindingTemplate> g_BindingTemplates;

// NOTE: g_EngToKorOverlayMap and g_OverlayMapMutex are now declared earlier
// (around line 292) with TextHook_RegisterOverlay and FindOverlayTranslation

// SUBTITLE-ONLY MAP: Only contains SUBTITLE_ key translations
// Used for native state capture - prevents menu text from being suppressed
struct SubtitleOnlyInfo {
  std::string key;
  std::string englishFull;
  int totalChars;
};
static std::unordered_map<std::string, SubtitleOnlyInfo> g_SubtitleOnlyMap;
static std::unordered_map<std::string, SubtitleOnlyInfo> g_SubtitleByKey;
static std::shared_mutex g_SubtitleMapMutex;

static std::atomic<bool> g_MenuContext{false};
static std::atomic<bool> g_FrontendMenuContext{false};
static std::atomic<DWORD> g_MenuDiagUntilTick{0};
static std::atomic<unsigned long long> g_MenuDiagSessionId{0};
static std::atomic<DWORD> g_LastPauseMenuTime{0};
static std::atomic<DWORD> g_LastHudActivityTime{0};
static std::atomic<DWORD> g_LastSubtitleActivityTime{0};
static std::mutex g_SubtitleClockMutex;
static DWORD g_SubtitleClockPausedAccumMs = 0;
static DWORD g_SubtitleClockPauseStartMs = 0;
static bool g_SubtitleClockWasPaused = false;

// Runtime feature flags
static bool g_EnableSubtitleNative = (GHOSTSKOR_NATIVE_SUBTITLE != 0);
static bool g_EnableHudNative = (GHOSTSKOR_NATIVE_HUD != 0);
static bool g_KeepHudEnglishReference = false;

// Native subtitle state (per-line)
static std::unordered_map<std::string, SubtitleNativeEntry>
    g_SubtitleNativeByNorm;
static std::unordered_map<std::string, SubtitleNativeEntry> g_SubtitleNativeByKey;
static std::mutex g_SubtitleNativeMutex;
static constexpr DWORD SUBTITLE_NATIVE_TIMEOUT = 2000;
static constexpr DWORD SUBTITLE_NATIVE_STALE = 10000;

// Native HUD state (captured from render/scanner), keyed by consumer instance.
static std::unordered_map<uint64_t, HudNativeEntry> g_HudNativeMap;
static std::mutex g_HudNativeMutex;
static constexpr DWORD HUD_NATIVE_TIMEOUT = 2500;

// Per-key fontScale from ObjRev native HudElem scan (render source).
// Written by ObjRev scanner, read by HUDHINT synth rendering.
static constexpr DWORD HUD_NATIVE_STALE = 10000;
static std::mutex g_NativeHudSnapshotMutex;
static std::unordered_map<uint64_t, NativeHudInstance> g_NativeHudInstanceCache;
static unsigned long g_NativeHudLastSnapshotTick = 0;
static std::unordered_map<uint64_t, HudRouteChannel> g_HudRouteByConsumer;
static std::unordered_map<uint64_t, DWORD> g_HudRouteQuarantineUntil;
struct HudRouteConflictHoldEntry {
  HudRouteChannel channel = HUD_ROUTE_UNSPECIFIED;
  DWORD until = 0;
};
static std::unordered_map<uint64_t, HudRouteConflictHoldEntry>
    g_HudRouteConflictHold;
static constexpr DWORD HUD_ROUTE_QUARANTINE_MS = 1800;
static constexpr DWORD HUD_ROUTE_CONFLICT_HOLD_MS = 450;
static constexpr float HUD_ROUTE_CONFLICT_LOWCONF_MAX = 0.84f;
static std::mutex g_HudRouteMutex;
static std::unordered_map<std::string, HudRouteChannel> g_HudSeedKeyChannel;
static std::unordered_map<uint64_t, HudRouteChannel> g_HudSeedCallerChannel;
static bool g_HudStrictConsumerAuthority = false;

static int TextHook_MakeSyntheticHudElemIndex(uintptr_t elemBase) {
  if (elemBase == 0) {
    return 0x4000;
  }
  const uint64_t folded =
      ((uint64_t)elemBase >> 4) ^ ((uint64_t)elemBase >> 20) ^
      ((uint64_t)elemBase >> 36);
  return 0x4000 | (int)(folded & 0x3FFFu);
}

static std::string TextHook_MakeHudHintRuntimeParamKey(const std::string &key,
                                                       unsigned int callerHint) {
  return key + "|" + std::to_string(callerHint);
}

static bool TextHook_ShouldTrackMissionPromptHintKey(const std::string &key) {
  if (key.empty() || !ObjRev_LooksLikeLocalizationKey(key.c_str())) {
    return false;
  }
  if (IsObjectiveOverlayKeyName(key) || IsObjectiveStatusMessageKeyName(key)) {
    return false;
  }
  if (HasRecentObjectiveObservationForKey(key, 6000)) {
    return false;
  }
  if (ObjectiveRuntime::HasActiveTopLeftOwnerForKey(key)) {
    return false;
  }

  const HudKeyType keyType = ClassifyHudKey(key);
  switch (keyType) {
  case HUD_KEY_MENU:
  case HUD_KEY_SUBTITLE:
  case HUD_KEY_INTRO:
  case HUD_KEY_EXCLUDE:
  case HUD_KEY_OBJECTIVE_LIST:
  case HUD_KEY_OBJECTIVE_UPDATE:
  case HUD_KEY_OBJECTIVE_HEADER:
  case HUD_KEY_GAME_SAVED:
  case HUD_KEY_NOW_SAVING:
    return false;
  default:
    break;
  }

  if (IsDirectAddCmdObjHudHintKey(key) || IsHudRuntimeTailJoinPromptKey(key)) {
    return false;
  }

  EnsureTranslationsLoaded();
  auto itKor = g_KeyToKorean.find(key);
  if (itKor == g_KeyToKorean.end() || itKor->second.empty()) {
    return false;
  }

  // Native HUDHINT intake must not depend on prompt-text heuristics.
  // Once a translated non-objective, non-menu key reaches an authoritative
  // caller/slot, structure/caller evidence decides promotion.
  return true;
}

// --- QTE initial high-scale capture ---
// When R_AddCmdDrawText is first called for a prompt key with a large scale
// (> 1.8), store the scale and tick.  The DedicatedHint renderer queries this
// via TextHook_ConsumeQteInitialHighScale() to bootstrap the shrink animation.
struct QteInitialCapture {
  float scale;
  DWORD tick;
};
static std::mutex g_QteInitCaptureMutex;
static std::unordered_map<std::string, QteInitialCapture> g_QteInitCapture;

bool TextHook_ConsumeQteInitialHighScale(const std::string &key,
                                         float &outScale,
                                         unsigned long &outTick) {
  std::lock_guard<std::mutex> lock(g_QteInitCaptureMutex);
  auto it = g_QteInitCapture.find(key);
  if (it == g_QteInitCapture.end()) {
    return false;
  }
  const DWORD now = GetTickCount();
  const DWORD age = (now >= it->second.tick) ? (now - it->second.tick) : 0;
  if (age > 10000) {
    g_QteInitCapture.erase(it);
    return false;
  }
  outScale = it->second.scale;
  outTick = it->second.tick;
  return true;
}

void TextHook_ClearQteInitialHighScale(const std::string &key) {
  std::lock_guard<std::mutex> lock(g_QteInitCaptureMutex);
  g_QteInitCapture.erase(key);
}

static void TextHook_RecordHudHintRuntimeParams(const std::string &key,
                                                unsigned int callerHint,
                                                float x,
                                                float y,
                                                float sx,
                                                float sy,
                                                unsigned short virtualW,
                                                unsigned short virtualH,
                                                int slotBucket,
                                                const float color[4],
                                                bool hasColor) {
  if (!TextHook_ShouldTrackMissionPromptHintKey(key)) {
    return;
  }
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(sx) ||
      !std::isfinite(sy)) {
    return;
  }

  const DWORD now = GetTickCount();
  const std::string sig = TextHook_MakeHudHintRuntimeParamKey(key, callerHint);
  bool shouldLog = false;
  unsigned int hitCountForLog = 0;
  {
    std::lock_guard<std::mutex> lock(g_HudHintRuntimeParamsMutex);
    RecentHudHintRuntimeParams &entry = g_HudHintRuntimeParams[sig];
    entry.tick = now;
    entry.callerHint = callerHint;
    entry.x = x;
    entry.y = y;
    entry.sx = sx;
    entry.sy = sy;
    entry.virtualW = virtualW;
    entry.virtualH = virtualH;
    entry.slotBucket = slotBucket;
    entry.hasColor = hasColor;
    if (color) {
      entry.color[0] = color[0];
      entry.color[1] = color[1];
      entry.color[2] = color[2];
      entry.color[3] = color[3];
    }
    if (entry.hitCount < 0xFFFFu) {
      ++entry.hitCount;
    }
    hitCountForLog = entry.hitCount;
    shouldLog = (hitCountForLog == 1 || (hitCountForLog % 32u) == 0u);
    if (g_HudHintRuntimeParams.size() > 512) {
      for (auto it = g_HudHintRuntimeParams.begin();
           it != g_HudHintRuntimeParams.end();) {
        const DWORD age = (now >= it->second.tick) ? (now - it->second.tick) : 0;
        if (age > 12000u) {
          it = g_HudHintRuntimeParams.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  // QTE initial high-scale capture: only for whitelisted QTE keys.
  {
    const float maxSxy = (std::max)(std::fabs(sx), std::fabs(sy));
    if (maxSxy > 1.8f &&
        (key == "SKYWAY_HINT_RELOAD" ||
         key == "FLOOD_ENDING_QTE_2_PROMPT" ||
         key == "NML_HINT_X_KB")) {
      std::lock_guard<std::mutex> lock(g_QteInitCaptureMutex);
      g_QteInitCapture[key] = {maxSxy, now};
    }
  }

  if (shouldLog) {
    char buf[320];
    sprintf_s(
        buf,
        "[NHUD-RT-PARAM-REC] key=%s caller=+0x%X x=%.2f y=%.2f sx=%.4f sy=%.4f slot=%d color=%d hits=%u",
        key.c_str(), callerHint, x, y, sx, sy, slotBucket, hasColor ? 1 : 0,
        hitCountForLog);
    LogToFile(buf);
  }
}

static bool TextHook_TryGetHudHintRuntimeParams(const std::string &key,
                                                unsigned int callerHint,
                                                RecentHudHintRuntimeParams &out) {
  if (key.empty()) {
    return false;
  }
  const DWORD now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_HudHintRuntimeParamsMutex);
  auto findFresh = [&](const std::string &sig,
                       RecentHudHintRuntimeParams &dst) -> bool {
    auto it = g_HudHintRuntimeParams.find(sig);
    if (it == g_HudHintRuntimeParams.end()) {
      return false;
    }
    const DWORD age = (now >= it->second.tick) ? (now - it->second.tick) : 0;
    if (age > HUD_HINT_RUNTIME_SLOT_TTL) {
      return false;
    }
    dst = it->second;
    return true;
  };

  if (callerHint != 0 &&
      findFresh(TextHook_MakeHudHintRuntimeParamKey(key, callerHint), out)) {
    return true;
  }

  // Do not borrow another nonzero caller's layout when a key is emitted by
  // multiple prompt families. That mixed-caller fallback can move one hint
  // into another family's geometry, which is exactly what happened to
  // CLOCKWORK_DISABLE_THE_SECURITY.
  if (callerHint != 0) {
    return false;
  }

  bool found = false;
  unsigned long bestTick = 0;
  const std::string prefix = key + "|";
  for (const auto &kv : g_HudHintRuntimeParams) {
    if (kv.first.rfind(prefix, 0) != 0) {
      continue;
    }
    const DWORD age = (now >= kv.second.tick) ? (now - kv.second.tick) : 0;
    if (age > HUD_HINT_RUNTIME_SLOT_TTL) {
      continue;
    }
    if (!found || kv.second.tick > bestTick) {
      out = kv.second;
      bestTick = kv.second.tick;
      found = true;
    }
  }
  return found;
}

bool TextHook_GetRecentHudHintRuntimeParams(const std::string &key,
                                            unsigned int callerHint,
                                            unsigned long maxAgeMs,
                                            HudHintRuntimeParamSnapshot &out) {
  RecentHudHintRuntimeParams param{};
  if (!TextHook_TryGetHudHintRuntimeParams(key, callerHint, param)) {
    return false;
  }

  const DWORD now = GetTickCount();
  const DWORD age = (now >= param.tick) ? (now - param.tick) : 0;
  if (age > maxAgeMs) {
    return false;
  }

  out.tick = param.tick;
  out.callerHint = param.callerHint;
  out.x = param.x;
  out.y = param.y;
  out.sx = param.sx;
  out.sy = param.sy;
  out.virtualW = param.virtualW;
  out.virtualH = param.virtualH;
  out.slotBucket = param.slotBucket;
  out.hasColor = param.hasColor;
  out.color[0] = param.color[0];
  out.color[1] = param.color[1];
  out.color[2] = param.color[2];
  out.color[3] = param.color[3];
  return true;
}

static void TextHook_RecordHudHintRuntimeSlot(uintptr_t slotBase,
                                              unsigned long now,
                                              unsigned int callerHint,
                                              unsigned int ret0Off,
                                              unsigned int ret1Off,
                                              unsigned int writeOff,
                                              uint32_t writeValue) {
  if (slotBase == 0 || !IsSafeRead((void *)slotBase, 0xA8)) {
    return;
  }

  std::string keyHint;
  if (callerHint != 0) {
    std::lock_guard<std::mutex> focusLock(g_HudHintWriterFocusMutex);
    keyHint = g_HudHintWriterFocusKey;
  }

  bool shouldLog = false;
  unsigned int hitCountForLog = 0;
  unsigned int callerForLog = 0;
  unsigned int writeOffForLog = 0;
  size_t totalSlotsForLog = 0;
  std::string keyForLog;

  {
    std::lock_guard<std::mutex> lock(g_HudHintRuntimeSlotsMutex);
    RecentHudHintRuntimeSlot &slot = g_HudHintRuntimeSlots[slotBase];
    if (slot.base == 0) {
      slot.base = slotBase;
      slot.firstSeenTick = now;
    }
    slot.lastSeenTick = now;
    if (callerHint != 0) {
      slot.callerHint = callerHint;
    }
    if (ret0Off != 0) {
      slot.ret0Off = ret0Off;
    }
    if (ret1Off != 0) {
      slot.ret1Off = ret1Off;
    }
    slot.lastWriteOff = writeOff;
    slot.lastWriteValue = writeValue;
    slot.hitCount++;
    if (!keyHint.empty()) {
      slot.keyHint = keyHint;
    }
    if (!slot.keyHint.empty()) {
      RecentHudHintRuntimeParams param{};
      if (TextHook_TryGetHudHintRuntimeParams(slot.keyHint, slot.callerHint,
                                              param)) {
        slot.paramTick = param.tick;
        slot.paramX = param.x;
        slot.paramY = param.y;
        slot.paramSX = param.sx;
        slot.paramSY = param.sy;
        slot.paramVW = param.virtualW;
        slot.paramVH = param.virtualH;
        slot.paramSlotBucket = param.slotBucket;
        slot.hasParamSnapshot = true;
        slot.hasColorSnapshot = param.hasColor;
        slot.paramColor[0] = param.color[0];
        slot.paramColor[1] = param.color[1];
        slot.paramColor[2] = param.color[2];
        slot.paramColor[3] = param.color[3];
      }
    }

    if (g_HudHintRuntimeSlots.size() > 256) {
      for (auto it = g_HudHintRuntimeSlots.begin();
           it != g_HudHintRuntimeSlots.end();) {
        const DWORD age =
            (now >= it->second.lastSeenTick) ? (now - it->second.lastSeenTick) : 0;
        if (age > HUD_HINT_RUNTIME_SLOT_TTL * 2) {
          it = g_HudHintRuntimeSlots.erase(it);
        } else {
          ++it;
        }
      }
    }

    hitCountForLog = slot.hitCount;
    callerForLog = slot.callerHint;
    writeOffForLog = slot.lastWriteOff;
    totalSlotsForLog = g_HudHintRuntimeSlots.size();
    keyForLog = slot.keyHint;
    shouldLog = (hitCountForLog == 1 || (hitCountForLog % 64u) == 0u);
  }
  if (shouldLog) {
    char lbuf[320];
    sprintf_s(lbuf,
              "[NHUD-DYN-REC] base=0x%llX caller=+0x%X off=0x%X hits=%u total=%zu key=%s",
              (unsigned long long)slotBase, callerForLog, writeOffForLog,
              hitCountForLog, totalSlotsForLog, keyForLog.c_str());
    LogToFile(lbuf);
  }
}

static std::vector<RecentHudHintRuntimeSlot>
TextHook_GetRecentHudHintRuntimeSlots(DWORD maxAgeMs) {
  const unsigned long now = GetTickCount();
  std::vector<RecentHudHintRuntimeSlot> out;
  std::lock_guard<std::mutex> lock(g_HudHintRuntimeSlotsMutex);
  out.reserve(g_HudHintRuntimeSlots.size());
  for (auto it = g_HudHintRuntimeSlots.begin();
       it != g_HudHintRuntimeSlots.end();) {
    const DWORD age =
        (now >= it->second.lastSeenTick) ? (now - it->second.lastSeenTick) : 0;
    if (age > HUD_HINT_RUNTIME_SLOT_TTL) {
      it = g_HudHintRuntimeSlots.erase(it);
      continue;
    }
    if (age <= maxAgeMs) {
      out.push_back(it->second);
    }
    ++it;
  }
  return out;
}

static size_t TextHook_GetHudHintRuntimeSlotCount() {
  std::lock_guard<std::mutex> lock(g_HudHintRuntimeSlotsMutex);
  return g_HudHintRuntimeSlots.size();
}
static int g_HudStrictConsumerAuthorityOverride = -1; // -1: manifest, 0/1: forced
static std::mutex g_HudSeedMutex;
static std::once_flag g_HudSeedLoadOnce;
static std::atomic<bool> g_HudSeedLoaded{false};

static uint64_t MakeHudSeedCallerKey(OverlaySource source,
                                     unsigned int callerOffset,
                                     int slotBucket) {
  const unsigned int caller = callerOffset & 0xFFFFFFu;
  const unsigned int slotEnc =
      (slotBucket < 0) ? 0xFFFFu : ((unsigned int)slotBucket & 0xFFFFu);
  uint64_t v = 0;
  v |= ((uint64_t)((unsigned int)source & 0xFFu)) << 56;
  v |= ((uint64_t)caller) << 16;
  v |= (uint64_t)slotEnc;
  return v;
}

static HudRouteChannel ParseHudRouteChannelSeed(const std::string &v) {
  const std::string u = ToUpper(TrimSpaces(v));
  if (u == "HUD_HINT") {
    return HUD_ROUTE_HINT;
  }
  if (u == "OBJHUD_HINT" || u == "OBJHUDHINT") {
    return HUD_ROUTE_OBJHUDHINT;
  }
  if (u == "OBJECTIVE") {
    return HUD_ROUTE_OBJECTIVE;
  }
  if (u == "IGNORE") {
    return HUD_ROUTE_IGNORE;
  }
  return HUD_ROUTE_UNSPECIFIED;
}

static OverlaySource ParseHudOverlaySourceSeed(const std::string &v,
                                               bool &ok) {
  ok = false;
  const std::string u = ToUpper(TrimSpaces(v));
  if (u == "HUD560" || u == "RENDER" || u == "HUD_NATIVE_RENDER") {
    ok = true;
    return OVERLAY_SOURCE_HUD560;
  }
  if (u == "ADDCMD" || u == "R_ADDCMDDRAWTEXT" ||
      u == "HUD_NATIVE_ADDCMD") {
    ok = true;
    return OVERLAY_SOURCE_ADDCMD;
  }
  if (u == "SLC" || u == "SL_CONVERTTOSTRING") {
    ok = true;
    return OVERLAY_SOURCE_SLC;
  }
  if (u == "HUDELEM_REVERSE" || u == "OBJREV") {
    ok = true;
    return OVERLAY_SOURCE_HUDELEM_REVERSE;
  }
  if (u == "FSHOOK") {
    ok = true;
    return OVERLAY_SOURCE_FSHOOK;
  }
  return OVERLAY_SOURCE_SLC;
}

static bool ParseCallerOffsetSeed(const std::string &raw,
                                  unsigned int &outCaller) {
  outCaller = 0;
  std::string s = TrimSpaces(raw);
  if (s.empty()) {
    return false;
  }
  bool digitsOnly = true;
  for (char c : s) {
    if (!std::isdigit((unsigned char)c)) {
      digitsOnly = false;
      break;
    }
  }
  if (digitsOnly) {
    outCaller = (unsigned int)(strtoul(s.c_str(), nullptr, 10) & 0xFFFFFFFFu);
    return outCaller != 0;
  }
  if (s.rfind("+0x", 0) == 0 || s.rfind("+0X", 0) == 0) {
    s = s.substr(1);
  }
  if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
    s = s.substr(2);
  }
  char *endp = nullptr;
  unsigned long v = strtoul(s.c_str(), &endp, 16);
  if (endp == s.c_str() || (endp && *endp != '\0')) {
    return false;
  }
  outCaller = (unsigned int)(v & 0xFFFFFFFFu);
  return outCaller != 0;
}

static bool ParseBoolSeed(const std::string &raw, bool defaultValue) {
  const std::string u = ToUpper(TrimSpaces(raw));
  if (u == "1" || u == "TRUE" || u == "ON" || u == "YES") {
    return true;
  }
  if (u == "0" || u == "FALSE" || u == "OFF" || u == "NO") {
    return false;
  }
  return defaultValue;
}

static bool LooksLikeSeedLocalizationKey(const std::string &key) {
  if (key.empty() || key.size() > 160) {
    return false;
  }
  bool hasAlpha = false;
  for (char c : key) {
    unsigned char uc = (unsigned char)c;
    if (std::isspace(uc)) {
      return false;
    }
    if (std::isalpha(uc)) {
      hasAlpha = true;
    }
  }
  if (!hasAlpha) {
    return false;
  }
  // Prefer strict localization-style tokens (mostly uppercase + '_' + digits).
  // This intentionally rejects function names like hintprint/sethintstring.
  for (char c : key) {
    unsigned char uc = (unsigned char)c;
    if (std::isdigit(uc) || c == '_' || c == '@' || c == '-' || c == '.') {
      continue;
    }
    if (std::isupper(uc)) {
      continue;
    }
    return false;
  }
  return true;
}

static std::once_flag g_HudStateManifestLoadOnce;
static std::unordered_set<unsigned short> g_HudStateHintOwnerdrawIds;
static std::unordered_set<unsigned short> g_HudStatePauseObjectiveOwnerdrawIds;
static bool g_HudStateManifestLoaded = false;

static void EnsureHudStateManifestLoaded() {
  std::call_once(g_HudStateManifestLoadOnce, []() {
    g_HudStateHintOwnerdrawIds.clear();
    g_HudStatePauseObjectiveOwnerdrawIds.clear();
    g_HudStateManifestLoaded = false;
    LogToFile("[HUD-STATE-MANIFEST] disabled");
  });
}

static void EnsureHudConsumerSeedsLoaded() {
  std::call_once(g_HudSeedLoadOnce, []() {
    {
      std::lock_guard<std::mutex> lk(g_HudSeedMutex);
      g_HudSeedKeyChannel.clear();
      g_HudSeedCallerChannel.clear();
      g_HudStrictConsumerAuthority = false;
    }
    LogToFile("[HUD-SEED] disabled");
    g_HudSeedLoaded.store(true, std::memory_order_release);
  });
}

static bool TryGetHudSeedChannelForKey(const std::string &key,
                                       HudRouteChannel &outChannel) {
  (void)key;
  outChannel = HUD_ROUTE_UNSPECIFIED;
  return false;
}

static bool TryGetHudSeedChannelForConsumer(OverlaySource source,
                                            unsigned int callerOffset,
                                            int slotBucket,
                                            HudRouteChannel &outChannel) {
  (void)source;
  (void)callerOffset;
  (void)slotBucket;
  outChannel = HUD_ROUTE_UNSPECIFIED;
  return false;
}

static bool IsHudStrictConsumerAuthorityEnabled() {
  if (g_HudStrictConsumerAuthorityOverride >= 0) {
    return g_HudStrictConsumerAuthorityOverride != 0;
  }
  return false;
}

bool TextHook_IsMenuContext() { return g_MenuContext.load(); }
bool TextHook_IsFrontendMenuLikely() { return g_FrontendMenuContext.load(); }
bool TextHook_IsMenuDiagActive() {
  const DWORD until = g_MenuDiagUntilTick.load();
  return (until != 0 && GetTickCount() <= until);
}
unsigned long long TextHook_GetMenuDiagSessionId() {
  return g_MenuDiagSessionId.load();
}
void TextHook_ArmMenuDiagWindow(unsigned long durationMs, const char *reason) {
  const DWORD now = GetTickCount();
  const DWORD until = now + durationMs;
  g_MenuDiagUntilTick.store(until);
  const unsigned long long session =
      g_MenuDiagSessionId.fetch_add(1, std::memory_order_relaxed) + 1ull;
  char buf[256];
  sprintf_s(buf, "[MENU-DIAG-ARM] sess=%llu dur=%lu reason=%s", session,
            durationMs, (reason && reason[0]) ? reason : "(none)");
  LogToFile(buf);
}

typedef void *(*Dvar_FindVar_t)(const char *name);
typedef int (*Dvar_GetInt_t)(const char *name);

static Dvar_FindVar_t ResolveDvarFindVar() {
  static uintptr_t s_moduleBase = 0;
  static Dvar_FindVar_t s_findVar = nullptr;
  if (!s_moduleBase) {
    s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  }
  if (s_moduleBase && !s_findVar && IW6Offsets::Dvar_FindVar_SP != 0) {
    s_findVar = (Dvar_FindVar_t)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(s_moduleBase, IW6Offsets::Dvar_FindVar_SP)));
  }
  return s_findVar;
}

static Dvar_GetInt_t ResolveDvarGetInt() {
  static uintptr_t s_moduleBase = 0;
  static Dvar_GetInt_t s_getInt = nullptr;
  if (!s_moduleBase) {
    s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  }
  if (s_moduleBase && !s_getInt && IW6Offsets::Dvar_GetInt_SP != 0) {
    s_getInt = (Dvar_GetInt_t)(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(s_moduleBase, IW6Offsets::Dvar_GetInt_SP)));
  }
  return s_getInt;
}

static bool TryReadDvarBoolValue(void *dvarPtr, bool &outValue) {
  if (!dvarPtr) {
    return false;
  }
  __try {
    const unsigned char *base = (const unsigned char *)dvarPtr;
    const int dvarType = *(const volatile int8_t *)(base + 0x0C);
    if (dvarType == 0) {
      outValue = (*(const volatile unsigned char *)(base + 0x10) != 0);
      return true;
    }
    if (dvarType == 1) {
      const float v = *(const volatile float *)(base + 0x10);
      outValue = (v >= 0.5f || v <= -0.5f);
      return true;
    }
    if (dvarType == 5 || dvarType == 6) {
      const int v = *(const volatile int *)(base + 0x10);
      outValue = (v != 0);
      return true;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryReadDvarFloatValue(void *dvarPtr, float &outValue) {
  if (!dvarPtr) {
    return false;
  }
  __try {
    const unsigned char *base = (const unsigned char *)dvarPtr;
    const int dvarType = *(const volatile int8_t *)(base + 0x0C);
    if (dvarType == 1) {
      const float v = *(const volatile float *)(base + 0x10);
      if (std::isfinite(v)) {
        outValue = v;
        return true;
      }
      return false;
    }
    if (dvarType == 0) {
      outValue =
          (*(const volatile unsigned char *)(base + 0x10) != 0) ? 1.0f : 0.0f;
      return true;
    }
    if (dvarType != 5 && dvarType != 6) {
      return false;
    }
    const int v = *(const volatile int *)(base + 0x10);
    outValue = (float)v;
    return std::isfinite(outValue);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static bool TryReadDvarStringValue(void *dvarPtr, std::string &outValue) {
  outValue.clear();
  if (!dvarPtr) {
    return false;
  }
  __try {
    const unsigned char *base = (const unsigned char *)dvarPtr;
    const int dvarType = *(const volatile int8_t *)(base + 0x0C);
    if (dvarType != 7) {
      return false;
    }
    const char *sv = *(const char *const *)(base + 0x10);
    if (!sv) {
      return false;
    }
    char buf[128];
    size_t n = 0;
    for (; n + 1 < sizeof(buf); ++n) {
      char c = sv[n];
      if (c == '\0') {
        break;
      }
      unsigned char uc = (unsigned char)c;
      if (uc < 0x20 || uc > 0x7E) {
        break;
      }
      buf[n] = c;
    }
    buf[n] = '\0';
    outValue.assign(buf, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    outValue.clear();
    return false;
  }
}

static bool TryParseBoolLikeString(const std::string &raw, bool &outValue) {
  const std::string u = ToUpper(TrimSpaces(raw));
  if (u.empty()) {
    return false;
  }
  if (u == "1" || u == "TRUE" || u == "ON" || u == "YES") {
    outValue = true;
    return true;
  }
  if (u == "0" || u == "FALSE" || u == "OFF" || u == "NO") {
    outValue = false;
    return true;
  }
  return false;
}

HudStateSignalSnapshot TextHook_GetHudStateSignalSnapshot() {
  EnsureHudStateManifestLoaded();
  HudStateSignalSnapshot snap{};
  const DWORD now = GetTickCount();
  snap.hasClPaused = false;
  snap.clPaused = false;
  snap.hideHudFast = false;
  snap.creditsActive = false;
  snap.hudShowObjectives = true;
  snap.hudShowIntel = true;
  snap.uiShowPopup = false;
  snap.uiSecuringActive = false;
  snap.uiSecuringProgress = 0.0f;
  snap.uiPrevMapChanged = false;
  snap.sampleTick = now;

  struct HudStateDvarCache {
    void *clPaused = nullptr;
    void *hideHudFast = nullptr;
    void *creditsActive = nullptr;
    void *hudShowObjectives = nullptr;
    void *hudShowIntel = nullptr;
    void *uiShowPopup = nullptr;
    void *uiSecuring = nullptr;
    void *uiSecuringProgress = nullptr;
    void *uiPrevMap = nullptr;
    DWORD lastResolveTick = 0;
    std::string lastUiPrevMap;
    DWORD lastUiPrevMapChangeTick = 0;
  };
  static HudStateDvarCache s_cache;

  Dvar_FindVar_t findVar = ResolveDvarFindVar();
  if (findVar &&
      (s_cache.lastResolveTick == 0 || (now - s_cache.lastResolveTick) > 900)) {
    s_cache.lastResolveTick = now;
    s_cache.clPaused = findVar("cl_paused");
    s_cache.hideHudFast = findVar("hideHudFast");
    s_cache.creditsActive = findVar("credits_active");
    s_cache.hudShowObjectives = findVar("hud_showObjectives");
    s_cache.hudShowIntel = findVar("hud_showIntel");
    s_cache.uiShowPopup = findVar("ui_showPopup");
    s_cache.uiSecuring = findVar("ui_securing");
    s_cache.uiSecuringProgress = findVar("ui_securing_progress");
    s_cache.uiPrevMap = findVar("ui_prev_map");
  }

  bool bv = false;
  if (TryReadDvarBoolValue(s_cache.clPaused, bv)) {
    snap.hasClPaused = true;
    snap.clPaused = bv;
  }
  if (TryReadDvarBoolValue(s_cache.hideHudFast, bv)) {
    snap.hideHudFast = bv;
  }
  // Credits detection: subtitle-triggered guard + dvar fallback.
  // The guard activates when SUBTITLE_SKYWAY_HSH_IMPROUDOFYOU1111
  // ("I'm proud of you, Logan.") fires pre-credits. Reset on OVL-RESET.
  {
    extern std::atomic<bool> g_creditsGuardActive;
    const bool guard = g_creditsGuardActive.load(std::memory_order_relaxed);
    bool dvarCredits = false;
    if (TryReadDvarBoolValue(s_cache.creditsActive, bv)) {
      dvarCredits = bv;
    }
    snap.creditsActive = guard || dvarCredits;

    if (snap.creditsActive) {
      static DWORD s_lastCreditsSnapLog = 0;
      if ((now - s_lastCreditsSnapLog) > 3000) {
        s_lastCreditsSnapLog = now;
        char csbuf[128];
        sprintf_s(csbuf, "[CREDITS-SNAP] creditsActive=1 guard=%d dvar=%d",
                  guard ? 1 : 0, dvarCredits ? 1 : 0);
        LogToFile(csbuf);
      }
    }
  }
  if (TryReadDvarBoolValue(s_cache.hudShowObjectives, bv)) {
    snap.hudShowObjectives = bv;
  }
  if (TryReadDvarBoolValue(s_cache.hudShowIntel, bv)) {
    snap.hudShowIntel = bv;
  }

  bool popupParsed = false;
  if (TryReadDvarBoolValue(s_cache.uiShowPopup, bv)) {
    snap.uiShowPopup = bv;
    popupParsed = true;
  } else {
    std::string popupRaw;
    if (TryReadDvarStringValue(s_cache.uiShowPopup, popupRaw) &&
        TryParseBoolLikeString(popupRaw, bv)) {
      snap.uiShowPopup = bv;
      popupParsed = true;
    }
  }
  if (!popupParsed) {
    snap.uiShowPopup = false;
  }

  std::string securingRaw;
  if (TryReadDvarStringValue(s_cache.uiSecuring, securingRaw)) {
    snap.uiSecuringActive = !TrimSpaces(securingRaw).empty();
  }
  float securingProgress = 0.0f;
  if (TryReadDvarFloatValue(s_cache.uiSecuringProgress, securingProgress)) {
    if (!std::isfinite(securingProgress)) {
      securingProgress = 0.0f;
    }
    if (securingProgress < 0.0f) {
      securingProgress = 0.0f;
    } else if (securingProgress > 1.5f) {
      securingProgress = 1.5f;
    }
    snap.uiSecuringProgress = securingProgress;
    if (!snap.uiSecuringActive &&
        (securingProgress > 0.0005f && securingProgress < 1.0f)) {
      snap.uiSecuringActive = true;
    }
  }

  std::string prevMapRaw;
  if (TryReadDvarStringValue(s_cache.uiPrevMap, prevMapRaw)) {
    const std::string prevMap = TrimSpaces(prevMapRaw);
    if (!prevMap.empty()) {
      if (!s_cache.lastUiPrevMap.empty() && s_cache.lastUiPrevMap != prevMap) {
        s_cache.lastUiPrevMapChangeTick = now;
      }
      s_cache.lastUiPrevMap = prevMap;
    }
  }
  if (s_cache.lastUiPrevMapChangeTick != 0 &&
      (now - s_cache.lastUiPrevMapChangeTick) <= 1200) {
    snap.uiPrevMapChanged = true;
  }

  return snap;
}

static bool TryReadPauseStateFromDvar(bool &outPaused) {
  const HudStateSignalSnapshot snap = TextHook_GetHudStateSignalSnapshot();
  if (!snap.hasClPaused) {
    return false;
  }
  outPaused = snap.clPaused;
  return true;
}

bool TextHook_IsPauseMenuLikely() {
  DWORD now = GetTickCount();
  DWORD last = g_LastPauseMenuTime.load();
  constexpr DWORD kPauseLikelyHoldMs = 180;
  constexpr DWORD kPauseNativeCacheMs = 2000;
  const bool legacyLikely = (last != 0 && (now - last) <= kPauseLikelyHoldMs);

  bool pausedByDvar = false;
  const bool hasClPaused = TryReadPauseStateFromDvar(pausedByDvar);
  static bool s_haveNativePauseSample = false;
  static bool s_lastNativePauseValue = false;
  static DWORD s_lastNativePauseTick = 0;

  // STRICT decision path:
  // - If cl_paused is readable, use it 1:1.
  // - Fallback to legacy sticky timing only when cl_paused is unavailable.
  bool likely = false;
  const char *source = "fallback";
  if (hasClPaused) {
    source = "native";
    likely = pausedByDvar;
    s_haveNativePauseSample = true;
    s_lastNativePauseValue = pausedByDvar;
    s_lastNativePauseTick = now;
    if (pausedByDvar) {
      // Keep sticky timestamp warm for legacy-only callsites.
      g_LastPauseMenuTime.store(now);
    }
  } else if (s_haveNativePauseSample &&
             (now - s_lastNativePauseTick) <= kPauseNativeCacheMs) {
    source = "native_cache";
    likely = s_lastNativePauseValue;
  } else {
    likely = legacyLikely;
  }

  static bool s_strictLogInit = false;
  static bool s_lastLikely = false;
  static bool s_lastHasClPaused = false;
  static bool s_lastClPaused = false;
  static bool s_lastLegacyLikely = false;
  const bool mismatch = (hasClPaused && (pausedByDvar != legacyLikely));
  if (!s_strictLogInit || likely != s_lastLikely ||
      hasClPaused != s_lastHasClPaused ||
      (hasClPaused && pausedByDvar != s_lastClPaused) ||
      (!hasClPaused && legacyLikely != s_lastLegacyLikely)) {
    std::ostringstream oss;
    oss << "[PAUSE-STRICT] source=" << source << " likely=" << (likely ? 1 : 0)
        << " hasClPaused=" << (hasClPaused ? 1 : 0)
        << " cl_paused=" << (hasClPaused ? (pausedByDvar ? 1 : 0) : -1)
        << " nativeCache=" << (s_haveNativePauseSample ? 1 : 0)
        << " nativeCacheAge=" << ((s_lastNativePauseTick != 0)
                                      ? (int)(now - s_lastNativePauseTick)
                                      : -1)
        << " legacy=" << (legacyLikely ? 1 : 0)
        << " mismatch=" << (mismatch ? 1 : 0)
        << " deltaMs=" << ((last != 0) ? (int)(now - last) : -1)
        << " holdMs=" << kPauseLikelyHoldMs;
    LogToFile(oss.str());
    s_strictLogInit = true;
    s_lastLikely = likely;
    s_lastHasClPaused = hasClPaused;
    s_lastClPaused = pausedByDvar;
    s_lastLegacyLikely = legacyLikely;
  }

  return likely;
}










bool TextHook_IsMenuActiveRaw() {
  // Menu presence is established by actual menu draw calls. The old raw
  // flag addresses were unverified and could latch unrelated memory after an update.
  return g_MenuContext.load();
}

static std::atomic<bool> g_MapLoaded{false};
static std::once_flag g_LoadOnce;





// =============================================================================
// MISSING TRANSLATION COLLECTOR - Collects untranslated SUBTITLE keys
// Saves to missing_subtitles.json for future translation work
// =============================================================================
static std::set<std::string> g_MissingSubtitleKeys;
static std::mutex g_MissingKeysMutex;
static DWORD g_LastMissingSaveTime = 0;
static constexpr DWORD MISSING_SAVE_INTERVAL = 30000; // Save every 30 seconds



// =============================================================================
// IN-GAME SUBTITLE QUEUE - For subtitles that bypass R_AddCmdDrawText
// SEH hook populates this, D3D11Hook::Hook_Present renders it
// =============================================================================
struct InGameSubtitle {
  std::string korean;
  DWORD timestamp;
};
static InGameSubtitle g_InGameSubtitle = {"", 0};
static std::mutex g_InGameSubtitleMutex;
static std::vector<InGameSubtitleEntry> g_InGameSubtitleQueue;
static constexpr size_t MAX_SUBTITLE_QUEUE = 4;
static constexpr DWORD SUBTITLE_NEWEST_PRUNE_MS = 6000;

struct SubtitleRuntimeTiming {
  int msgTimeMs = 0;
  int fadeOutMs = 0;
  int arg6 = 0;
  int windowId = 0;
  DWORD tick = 0;
};
static std::unordered_map<std::string, SubtitleRuntimeTiming>
    g_SubtitleRuntimeTimingByKey;
static std::unordered_map<std::string, SubtitleRuntimeTiming>
    g_SubtitleRuntimeTimingByNormText;
static std::mutex g_SubtitleRuntimeTimingMutex;
static constexpr size_t SUBTITLE_RUNTIME_TIMING_KEY_MAX = 192;
static constexpr size_t SUBTITLE_RUNTIME_TIMING_NORM_MAX = 384;
static constexpr DWORD SUBTITLE_RUNTIME_TIMING_STALE_MS = 15000;

// Subtitle reverse pipeline (native remove sync)
static constexpr bool kEnableSubtitleReverse = (GHOSTSKOR_SUBTITLE_REVERSE != 0);
static constexpr bool kEnableSubtitleReverseVeh =
    kEnableSubtitleReverse && (GHOSTSKOR_SUBTITLE_REVERSE_VEH != 0);
static constexpr bool kEnableSubtitleReverseAutoFailOpen = true;
static constexpr size_t SUBREV_MAX_EVENTS = 256;
static constexpr size_t SUBREV_MAX_BINDINGS = 48;
static constexpr size_t SUBREV_MAX_WATCH_PAGES = 24;
static constexpr int SUBREV_BIND_SCORE_THRESHOLD = 6;
static constexpr size_t SUBREV_SCAN_PRIMARY_MAX_PAGES = 8192;
static constexpr size_t SUBREV_SCAN_PRIMARY_MAX_BYTES = 128 * 1024 * 1024;
static constexpr size_t SUBREV_SCAN_ROLLING_MAX_PAGES = 4096;
static constexpr size_t SUBREV_SCAN_ROLLING_MAX_BYTES = 96 * 1024 * 1024;
static constexpr size_t SUBREV_SCAN_LOW_MAX_PAGES = 16384;
static constexpr size_t SUBREV_SCAN_LOW_MAX_BYTES = 192 * 1024 * 1024;
static constexpr uintptr_t SUBREV_SCAN_WINDOW_BYTES = 0x20000000ull; // 512MB
static constexpr uintptr_t SUBREV_LOW_ADDR_STOP = 0x100000000ull;     // 4GB
static constexpr DWORD SUBREV_SUMMARY_INTERVAL_MS = 5000;
static constexpr DWORD SUBREV_EVENT_STALE_MS = 20000;
static constexpr DWORD SUBREV_REMOVE_SETTLE_MS = 120;

enum SubtitleRemoveReason {
  SUBREV_REMOVE_NONE = 0,
  SUBREV_REMOVE_TEXTPTR = 1,
  SUBREV_REMOVE_FLAG = 2,
  SUBREV_REMOVE_ALPHA = 3
};

struct SubtitleEnqueueInstanceEvent {
  std::string key;
  uintptr_t textPtr = 0;
  int msgTimeMs = 0;
  int fadeOutMs = 0;
  DWORD enqueueTick = 0;
  uint32_t seq = 0;
};

struct SubtitleReverseBinding {
  std::string key;
  uintptr_t slotPtr = 0;
  uintptr_t textPtr = 0;
  int textPtrKind = 0; // 2=ptr, 1=u32
  int msgOff = 0;
  int fadeOff = 0;
  int msgTimeMs = 0;
  int fadeOutMs = 0;
  int bindScore = 0;
  DWORD enqueueTick = 0;
  DWORD boundTick = 0;
  DWORD lastGuardTick = 0;
  uintptr_t lastGuardRip = 0;
  DWORD removeTick = 0;
  int removeReason = SUBREV_REMOVE_NONE;
  int alphaZeroStreak = 0;
  bool flagEverNonZero = false;
  bool alphaPrimed = false;
  // If we can locate the engine's copied subtitle string buffer, prefer that
  // for remove detection (the input pointer can be transient).
  uintptr_t textBufPtr = 0;
  uint32_t textBufSig = 0;
  bool textBufValid = false;
  // Removal arming: ignore early clears right after enqueue/bind.
  DWORD activeSinceTick = 0;
  DWORD lastNonZeroTick = 0;
  int zeroStreak = 0;
  int earlyZeroCount = 0;
  bool strEverNonZero = false;
  int strZeroStreak = 0;
  bool removeArmed = false;
  DWORD lastRehydrateCandLogTick = 0;
  DWORD lastRehydrateMissLogTick = 0;
  bool removeLatched = false;
  bool slotDumped = false;
};

struct SubtitleGuardWatchPage {
  std::atomic<uintptr_t> pageBase;
  std::atomic<DWORD> baseProtect;
  std::atomic<uintptr_t> slotPtr;
  std::atomic<DWORD> lastHitTick;
  std::atomic<unsigned long> hits;
  std::atomic<unsigned char> active;
  SubtitleGuardWatchPage()
      : pageBase(0), baseProtect(0), slotPtr(0), lastHitTick(0), hits(0),
        active(0) {}
};

struct SubRevCandidateScoreDetail {
  uint32_t startTick = 0;
  bool hasStart = false;
  uint32_t msgVal = 0;
  bool hasMsg = false;
  int msgOff = 0;
  uint32_t fadeVal = 0;
  bool hasFade = false;
  int fadeOff = 0;
  uint32_t flagVal = 0;
  bool hasFlag = false;
  int ptrMatchKind = 0; // 2=ptr64, 1=ptr32, 0=none
};

static std::mutex g_SubtitleReverseMutex;
static std::deque<SubtitleEnqueueInstanceEvent> g_SubtitleReverseEvents;
static std::unordered_map<std::string, SubtitleReverseBinding>
    g_SubtitleReverseBindingsByKey;
static std::unordered_map<uintptr_t, std::string> g_SubtitleReverseKeyBySlot;
static std::array<SubtitleGuardWatchPage, SUBREV_MAX_WATCH_PAGES>
    g_SubtitleReverseWatchPages;
static std::atomic<bool> g_SubtitleReverseEnabled{kEnableSubtitleReverse};
static std::atomic<bool> g_SubtitleReverseVehEnabled{kEnableSubtitleReverseVeh};
static std::atomic<bool> g_SubtitleReverseNeedRearm{false};
static std::atomic<uintptr_t> g_SubtitleReverseLastGuardRip{0};
static std::atomic<uintptr_t> g_SubtitleReverseLastGuardAddr{0};
static std::atomic<DWORD> g_SubtitleReverseLastGuardTick{0};
static std::atomic<uintptr_t> g_SubtitleReverseLastGuardOp{0};
static std::atomic<uintptr_t> g_SubtitleReverseLastGuardRcx{0};
static std::atomic<uintptr_t> g_SubtitleReverseLastGuardRdx{0};
static std::atomic<uintptr_t> g_SubtitleReverseLastGuardR8{0};
static std::atomic<uintptr_t> g_SubtitleReverseLastGuardR9{0};
static std::atomic<uintptr_t> g_SubtitleReverseLastGuardRax{0};
static std::atomic<uint32_t> g_SubtitleReverseNextSeq{1};
static std::atomic<DWORD> g_SubtitleReverseLastSummaryTick{0};
static std::atomic<DWORD> g_SubtitleReverseGuardWindowTick{0};
static std::atomic<unsigned long> g_SubtitleReverseGuardWindowHits{0};
static std::atomic<unsigned long> g_SubtitleReverseEventsSeen{0};
static std::atomic<unsigned long> g_SubtitleReverseBindAttempts{0};
static std::atomic<unsigned long> g_SubtitleReverseBindSuccess{0};
static std::atomic<unsigned long> g_SubtitleReverseBindConflicts{0};
static std::atomic<unsigned long> g_SubtitleReverseGuardInstall{0};
static std::atomic<unsigned long> g_SubtitleReverseGuardHits{0};
static std::atomic<unsigned long> g_SubtitleReverseGuardRearms{0};
static std::atomic<unsigned long> g_SubtitleReverseVehGuardSeen{0};
static std::atomic<unsigned long> g_SubtitleReverseVehSingleStepSeen{0};
static std::atomic<unsigned long> g_SubtitleReverseRemoveDetected{0};
static std::atomic<unsigned long> g_SubtitleReverseRemoveByText{0};
static std::atomic<unsigned long> g_SubtitleReverseRemoveByFlag{0};
static std::atomic<unsigned long> g_SubtitleReverseRemoveByAlpha{0};
static std::atomic<unsigned long> g_SubtitleReverseFailOpenCount{0};
static PVOID g_SubtitleReverseVehHandle = nullptr;
static DWORD g_SubtitleReversePageSize = 0;
static uintptr_t g_SubtitleReverseScanCursor = 0;










































































// Native Subtitle State Storage
static NativeSubtitleState g_NativeSubtitleState = {0};
static std::mutex g_NativeStateMutex;

// Diagnostic: last subtitle English seen in SEH hook
static LastSubtitleDiag g_LastSubtitleDiag = {"", "", 0};
static std::mutex g_LastSubtitleDiagMutex;

// Reverse diagnostics: identify which game callsites request SUBTITLE_* keys.
// This helps locate the upstream subtitle queue/remove lifecycle functions.
struct SubtitleSehCallsiteStat {
  uint32_t hits = 0;
  DWORD firstTick = 0;
  DWORD lastTick = 0;
  std::string sampleKey;
};
static std::unordered_map<uintptr_t, SubtitleSehCallsiteStat>
    g_SubtitleSehCallsiteStats;
static std::mutex g_SubtitleSehCallsiteMutex;
static std::atomic<int> g_SubtitleSehStackLogCount{0};
static std::atomic<DWORD> g_SubtitleSehLastSummaryTick{0};












































































// =============================================================================
// INTRO OVERLAY SYSTEM - Mission intro screens (typewriter text)
// =============================================================================
static std::vector<IntroOverlayLine> g_IntroOverlayLines;
static std::mutex g_IntroOverlayMutex;
static DWORD g_IntroOverlayStartTime = 0;
static DWORD g_IntroOverlayLastUpdate = 0;
static bool g_IntroOverlayActive = false;
static std::string g_IntroOverlayTag;
static constexpr DWORD INTRO_OVERLAY_TIMEOUT = 15000; // 15 seconds

// Context: current slcIdx being rendered (set by Detour_IntroRenderFn, read by TextHook_OnIntroKey)
static int g_currentIntroSlcIdx = -1;























// Thread-safe buffer for stripped translations
static char g_StrippedStrings[MAX_TRANSLATED_STRINGS][MAX_TRANSLATED_LEN];
static std::atomic<int> g_StrippedIndex{0};


// Helper: Try to read HudElem fields from a pointer (SEH-protected)
// No C++ objects allowed in this function (__try requirement).
// Returns true and fills outBuf with log message if successful.
#pragma warning(push)
#pragma warning(disable : 4733) // inline asm SEH warning



#pragma warning(pop)




static void LogRelativeCalls(const char *tag, uintptr_t addr, int len);

static std::mutex g_SubRevRipDumpMutex;
static std::unordered_set<uintptr_t> g_SubRevRipDumped;


















// =============================================================================

// DETOUR FUNCTIONS

// =============================================================================

// Track logged font addresses (to log each unique font only once)

static uintptr_t g_LoggedFonts[16] = {0};

static int g_LoggedFontCount = 0;

static float MeasureEnglishWidthForLayout(const char *text, Font_s *font,
                                          float fontHeight, float xScale,
                                          const char *contextTag = nullptr) {
  if (!text || !text[0]) {
    return 0.0f;
  }

  float safeFontHeight = fontHeight;
  if (!(safeFontHeight > 0.0f) && font && font->fontHeight > 0.0f) {
    safeFontHeight = (float)font->fontHeight;
  }
  if (!(safeFontHeight > 0.0f)) {
    safeFontHeight = 44.0f;
  }

  // Try native engine measurement first (fast, no character iteration).
  const auto nativeTextWidth = g_R_TextWidth.load(std::memory_order_acquire);
  if (nativeTextWidth && font) {
    const int nativeWidthPixels = nativeTextWidth(text, -1, font);
    const float nativeWidth = (float)nativeWidthPixels * xScale;
    const bool validNativeWidth =
        (nativeWidthPixels > 0 && nativeWidthPixels != INT_MIN &&
         nativeWidth > 1.0f && nativeWidth <= 8192.0f);
    if (validNativeWidth) {
      return nativeWidth;
    }
  }

  // Fallback: measure via Korean font atlas (iterates all characters).
  float fallbackWidth =
      KoreanRenderer::MeasureTextWidthEx(text, safeFontHeight, xScale) * 1.28f;

  if (fallbackWidth > 1.0f) {
    static DWORD s_LastBadMenuWidthLogTick = 0;
    const DWORD now = GetTickCount();
    if ((now - s_LastBadMenuWidthLogTick) > 250) {
      s_LastBadMenuWidthLogTick = now;
      char buf[320];
      sprintf_s(
          buf,
          "[MENU-WIDTH-FALLBACK] ctx=%s fallbackW=%.1f text=\"%.48s\"",
          (contextTag && contextTag[0]) ? contextTag : "(none)",
          fallbackWidth, text);
      LogToFile(buf);
    }
    return fallbackWidth;
  }

  return 0.0f;
}















// =============================================================================
// MECHANICAL SPLIT INCLUDE MAP (Same-TU)
// Keep this include block order and placement in sync with file classification.
// Placement rule: after global/static/type blocks, before objective reverse defs.
// =============================================================================
// Forward declaration for TimeScript_StoreSnapshotFromHudElemTimer
// (defined in TextHook.TimeScript.inl, called from TextHook.IntroRendering.inl)
static void TimeScript_StoreSnapshotFromHudElemTimer(
    const std::string &key,
    float elemX, float elemY, float fontScale,
    uint32_t alignOrg, uint32_t alignScreen,
    uint32_t colorPacked,
    int timeField, int durationField, float valueField,
    uint32_t labelSlc, uint32_t textSlc, uintptr_t elemPtr);

// Forward declaration for persistent alpha suppression state
// (defined in TextHook.TimeScript.inl, used from TextHook.IntroRendering.inl)
struct TimeScriptPersistentSuppress {
  uintptr_t rbx = 0;
  uintptr_t elemPtr = 0;             // actual HudElem ptr for fontScale suppress
  uint32_t originalColor = 0;
  uint32_t originalFontScaleBits = 0; // raw IEEE 754 bits of original fontScale
  uint32_t cachedTimeField = 0;      // cached HudElem +0x78 (absolute end time ms)
  uint32_t originalType = 0;         // cached HudElem +0x00 (element type, e.g. 7=TIMER_DOWN)
  uint32_t originalXBits = 0;        // raw IEEE 754 bits of original x (offset 0x04)
  uint32_t originalLabel = 0;        // cached HudElem +0x40 (label configstring)
  uint32_t originalTextSlc = 0;      // cached HudElem +0x84 (text SLC, timer=0x0)
  DWORD lastSuppressTick = 0;
  bool active = false;
  // Slot-reuse deactivation cooldown: prevents the scanner/auto-registration
  // from immediately re-registering the same elemPtr after CG_DrawHudElem
  // detected slot reuse.  Without this, the scanner sees stale timer
  // type/label fields on the repurposed slot and re-registers with the new
  // (non-timer) field values, making slot-reuse detection impossible.
  uintptr_t deactivatedElemPtr = 0;
  DWORD deactivatedTick = 0;
};
static TimeScriptPersistentSuppress g_TimeScriptPersistSuppress{};

// Subsystem: Intro rendering
#include "TextHook.IntroRendering.inl"
static int g_countdownTimeField = 0;  // Used by TimeScript.inl and SuppressActiveCountdownLabel
// Subsystem: dedicated time-script HUD countdown
#include "TextHook.TimeScript.inl"
// Subsystem: Subtitle / StringEd
#include "TextHook.SubtitleStringEd.inl"
// Subsystem: HUD / Hint system
#include "TextHook.HudHint.inl"
// Subsystem: Key/Input routing
#include "TextHook.KeyInputRouting.inl"
// Subsystem: Translation / KeyResolver
#include "TextHook.TranslationKeyResolver.inl"
// Subsystem: Objective system
#include "TextHook.ObjectiveSystem.inl"
// Subsystem: Objective authority native capture
#include "TextHook.ObjectiveAuthority.NativeCapture.inl"
// Subsystem: Objective/Status reverse-native snapshot
#include "TextHook.ObjectiveStatusReverse.inl"
// Subsystem: Infrastructure (queue, cache, logging)
#include "TextHook.Infrastructure.inl"
// Subsystem: Hook installation / detours
#include "TextHook.HookInstallAndDetours.inl"
// Forward declaration for session reset (defined after g_ObjRevObjectiveBias).
static void ObjRev_ResetBiasState();
// Subsystem: Overlay management
#include "TextHook.OverlayManagement.inl"
// Subsystem: ObjHudHint system
#include "TextHook.ObjHudHint.inl"


// =============================================================================
// OBJECTIVE REVERSE: HudElem Array Discovery (Phase 1)
// =============================================================================
#if GHOSTSKOR_OBJECTIVE_REVERSE

// Safe memory readers (reusable)
template <typename T>
static __forceinline bool ObjRev_ReadCheckedValue(uintptr_t addr, T &out) {
  if (addr <= 0x10000 || addr >= 0x7FFFFFFFFFFF)
    return false;
  return SafeReadVolatile(addr, &out);
}

static __declspec(noinline) bool ObjRev_ReadU32(uintptr_t addr, uint32_t &out) {
  return ObjRev_ReadCheckedValue(addr, out);
}
static __declspec(noinline) bool ObjRev_ReadI32(uintptr_t addr, int &out) {
  return ObjRev_ReadCheckedValue(addr, out);
}
static __declspec(noinline) bool ObjRev_ReadF32(uintptr_t addr, float &out) {
  if (!ObjRev_ReadCheckedValue(addr, out))
    return false;
  return std::isfinite(out) != 0;
}
static __declspec(noinline) bool ObjRev_ReadU64(uintptr_t addr, uintptr_t &out) {
  return ObjRev_ReadCheckedValue(addr, out);
}
static __declspec(noinline) bool ObjRev_WriteU32(uintptr_t addr, uint32_t value) {
  if (addr <= 0x10000 || addr >= 0x7FFFFFFFFFFF) {
    return false;
  }
  if (IsBadWritePtr((void *)addr, sizeof(uint32_t)) != 0) {
    return false;
  }
  __try {
    *(volatile uint32_t *)addr = value;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
static bool ObjRev_IsLikelyKeyChar(char c) {
  return ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '.');
}

static bool ObjRev_LooksLikeLocalizationKey(const char *s) {
  if (!s || !s[0]) {
    return false;
  }
  int len = 0;
  int underscores = 0;
  int uppercase = 0;
  for (; len < 96; ++len) {
    char c = s[len];
    if (c == '\0') {
      break;
    }
    if (!ObjRev_IsLikelyKeyChar(c)) {
      return false;
    }
    if (c == '_') {
      underscores++;
    } else if (c >= 'A' && c <= 'Z') {
      uppercase++;
    }
  }
  if (len < 6 || len >= 96) {
    return false;
  }
  return (underscores >= 1 && uppercase >= 3);
}

// Early-exit filter: reject strings that are clearly not translatable
// (asset names, VFX paths, pure numbers, single characters).
// Avoids expensive TextHook_ResolveKeyFromEnglish and probe pipeline calls.
static bool ObjRev_IsObviouslyNotTranslatable(const std::string &s) {
  const size_t len = s.size();
  if (len <= 2) return true; // "_", "|", "00" etc.

  bool hasLower = false;
  bool hasUnderscore = false;
  bool hasSpace = false;
  bool allDigitOrDot = true;

  for (size_t i = 0; i < len; ++i) {
    const char c = s[i];
    if (c == '/') return true; // path separator → file/VFX path
    if (c >= 'a' && c <= 'z') hasLower = true;
    else if (c == '_') hasUnderscore = true;
    else if (c == ' ') hasSpace = true;
    if (!((c >= '0' && c <= '9') || c == '.')) allDigitOrDot = false;
  }

  if (allDigitOrDot) return true; // "180", "270", "0.5" etc.
  // lowercase + underscore + no spaces = asset/model name pattern
  // e.g. "saf_missile", "weapon_vks", "oilrocks_gastank1_large_d_red"
  if (hasLower && hasUnderscore && !hasSpace) return true;

  return false;
}

static bool ObjRev_IsReadable(uintptr_t p, size_t bytes);

static constexpr uint32_t kObjRevLocalizedCfgMin = 0x217u;
static constexpr uint32_t kObjRevLocalizedCfgMax = 0x415u;

static inline bool ObjRev_IsLocalizedConfigStringIndex(uint32_t cfgIdx) {
  return (cfgIdx >= kObjRevLocalizedCfgMin && cfgIdx <= kObjRevLocalizedCfgMax);
}

static ObjRev_ConfigIndexToSlcFn g_ObjRevConfigIndexToSlc = nullptr;
static uintptr_t g_ObjRevConfigIndexToSlcAddr = 0;
static std::atomic<uintptr_t> g_ObjRevAltCfgLookupFn{0};
static std::atomic<uintptr_t> g_ObjRevAltCfgLookupTable{0};
static std::atomic<bool> g_ObjRevCfgLookupScanDone{false};
static std::atomic<uint32_t> g_ObjRevObjectiveBias{0};
static std::atomic<unsigned long> g_ObjRevObjectiveBiasTick{0};

// Called from TextHook_ResetObjectiveRuntimeForMapRestart (OverlayManagement.inl
// cannot see these globals directly due to include ordering).
static void ObjRev_ResetBiasState() {
  g_ObjRevObjectiveBias.store(0);
  g_ObjRevObjectiveBiasTick.store(0);
}

static ObjRev_ConfigIndexToSlcFn ObjRev_GetConfigIndexToSlcFn() {
  if (g_ObjRevConfigIndexToSlc != nullptr) {
    return g_ObjRevConfigIndexToSlc;
  }

  auto AcceptFn = [](uintptr_t addr, const char *source) -> bool {
    if (addr == 0 || !IsSafeRead((void *)addr, 16)) {
      return false;
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) {
      return false;
    }
    DWORD p = (mbi.Protect & 0xFF);
    const bool executable = (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
                             p == PAGE_EXECUTE_READWRITE ||
                             p == PAGE_EXECUTE_WRITECOPY);
    if (!executable) {
      return false;
    }
    g_ObjRevConfigIndexToSlcAddr = addr;
    g_ObjRevConfigIndexToSlc = (ObjRev_ConfigIndexToSlcFn)addr;
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      char buf[192];
      sprintf_s(buf, "[CFG-REV] cfg->slc resolver=%s +0x%llX", source,
                (unsigned long long)(addr - (uintptr_t)GetModuleHandleA(NULL)));
      LogToFile(buf);
    }
    return true;
  };

  const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  if (moduleBase == 0) {
    return nullptr;
  }

  const uintptr_t off = IW6Offsets::ConfigString_IndexToSlc_SP;
  if (off != 0 && AcceptFn(reinterpret_cast<uintptr_t>(IW6Offsets::GetAddress(moduleBase, off)), "resolved_pattern")) {
    return g_ObjRevConfigIndexToSlc;
  }

  // Signature fallback: callsite shape observed in runtime code around +0x328E30:
  // lea ecx,[rsi+disp32] ; call cfg_to_slc ; mov ecx,eax ; call SL_ConvertToString
  MODULEINFO mi = GetModuleInfo(nullptr);
  if (mi.lpBaseOfDll != nullptr && mi.SizeOfImage > 0) {
    const char pattern[] =
        "\x8D\x8E\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x8B\xC8\xE8\x00\x00\x00\x00\x4C\x8B\xC3\xBA\x00\x04\x00\x00\x48\x8B\xC8\xE8";
    const char mask[] = "xx????x????xxx????xxxxxxxxxxxx";
    void *hit = FindPattern((char *)mi.lpBaseOfDll, (size_t)mi.SizeOfImage, pattern, mask);
    if (hit != nullptr) {
      int32_t disp = *(int32_t *)((uintptr_t)hit + 2);
      if (disp > 0 && disp <= 0xFFFF) {
        g_ObjRevObjectiveBias.store((uint32_t)disp);
        g_ObjRevObjectiveBiasTick.store(GetTickCount());
      }
      uintptr_t callInstr = (uintptr_t)hit + 6; // points at E8 xx xx xx xx
      int32_t rel = *(int32_t *)(callInstr + 1);
      uintptr_t target = callInstr + 5 + (intptr_t)rel;
      if (AcceptFn(target, "sig_328E30")) {
        return g_ObjRevConfigIndexToSlc;
      }
    }
  }

  static bool s_loggedUnresolved = false;
  if (!s_loggedUnresolved) {
    s_loggedUnresolved = true;
    LogToFile("[CFG-REV] cfg->slc resolver unresolved");
  }
  return nullptr;
}

static void ObjRev_ScanIndexLookupTablesOnce() {
  if (g_ObjRevCfgLookupScanDone.exchange(true)) {
    return;
  }

  MODULEINFO mi = GetModuleInfo(nullptr);
  if (mi.lpBaseOfDll == nullptr || mi.SizeOfImage < 16) {
    LogToFile("[CFGTAB-SCAN] skipped: module info unavailable");
    return;
  }

  const uintptr_t moduleBase = (uintptr_t)mi.lpBaseOfDll;
  const size_t imageSize = (size_t)mi.SizeOfImage;
  const uint8_t *image = (const uint8_t *)mi.lpBaseOfDll;

  auto ReadU32Safe = [&](uintptr_t addr, uint32_t &out) -> bool {
    if (!IsSafeRead((void *)addr, 4)) {
      return false;
    }
    out = *(volatile uint32_t *)addr;
    return true;
  };

  struct ScanHit {
    uintptr_t fn = 0;
    uintptr_t table = 0;
    uint32_t v31 = 0;
    uint32_t v41 = 0;
    uint32_t v44 = 0;
    uint32_t v45 = 0;
    uint32_t v46 = 0;
    uint32_t v53 = 0;
    int objectiveSlcHits = 0;
    std::vector<std::pair<uint32_t, uint32_t>> samples;
  };

  std::vector<ScanHit> hits;
  hits.reserve(16);

  for (size_t i = 0; i + 14 < imageSize; ++i) {
    const uint8_t *p = image + i;

    // Canonical table-lookup leaf pattern:
    //   movsxd rax, ecx
    //   lea    r?, [rip+disp32]
    //   mov    eax, [r? + rax*4]
    //   ret
    if (p[0] != 0x48 || p[1] != 0x63 || p[2] != 0xC1 || p[3] != 0x48 ||
        p[4] != 0x8D || p[10] != 0x8B || p[11] != 0x04 || p[13] != 0xC3) {
      continue;
    }

    const uint8_t leaModrm = p[5];
    // RIP-relative LEA is encoded as mod=00, rm=101 regardless of destination
    // register. Do not over-constrain destination register here.
    if ((leaModrm & 0xC7) != 0x05) {
      continue;
    }
    const uint8_t destReg = (uint8_t)((leaModrm >> 3) & 0x7);
    const uint8_t expectedSib = (uint8_t)(0x80 | destReg); // [r? + rax*4]
    if (p[12] != expectedSib) {
      continue;
    }

    int32_t disp = *(const int32_t *)(p + 6);
    const uintptr_t fnAddr = moduleBase + i;
    const uintptr_t tableAddr = (uintptr_t)(p + 10) + (intptr_t)disp;
    if (!IsSafeRead((void *)tableAddr, 0x200)) {
      continue;
    }

    ScanHit hit = {};
    hit.fn = fnAddr;
    hit.table = tableAddr;
    ReadU32Safe(tableAddr + 0x31u * 4u, hit.v31);
    ReadU32Safe(tableAddr + 0x41u * 4u, hit.v41);
    ReadU32Safe(tableAddr + 0x44u * 4u, hit.v44);
    ReadU32Safe(tableAddr + 0x45u * 4u, hit.v45);
    ReadU32Safe(tableAddr + 0x46u * 4u, hit.v46);
    ReadU32Safe(tableAddr + 0x53u * 4u, hit.v53);

    // Objective-key cluster observed in current logs sits around 0x54F4..0x54FA.
    // Scan the first namespace window for entries in this range.
    for (uint32_t idx = 0; idx <= 0x1FFF; ++idx) {
      uint32_t v = 0;
      if (!ReadU32Safe(tableAddr + (uintptr_t)idx * 4u, v)) {
        continue;
      }
      if (v >= 0x54E0u && v <= 0x551Fu) {
        ++hit.objectiveSlcHits;
        if (hit.samples.size() < 8) {
          hit.samples.push_back(std::make_pair(idx, v));
        }
      }
    }

    hits.push_back(hit);
    if (hits.size() >= 32) {
      break;
    }
    i += 13;
  }

  if (hits.empty()) {
    LogToFile("[CFGTAB-SCAN] no canonical lookup leaf matched");
    return;
  }

  int bestScore = -1;
  size_t bestIndex = 0;
  for (size_t i = 0; i < hits.size(); ++i) {
    const ScanHit &h = hits[i];
    int score = 0;
    if (h.v45 > 1) {
      score += 30;
      if (h.v45 >= 0x5000u && h.v45 <= 0x7000u) {
        score += 90;
      }
    }
    if (h.v46 > 1) {
      score += 30;
      if (h.v46 >= 0x5000u && h.v46 <= 0x7000u) {
        score += 90;
      }
    }
    score += h.objectiveSlcHits * 8;
    if (score > bestScore) {
      bestScore = score;
      bestIndex = i;
    }
  }

  char sbuf[192];
  sprintf_s(sbuf, "[CFGTAB-SCAN] hits=%u bestScore=%d",
            (unsigned int)hits.size(), bestScore);
  LogToFile(sbuf);

  for (size_t i = 0; i < hits.size(); ++i) {
    const ScanHit &h = hits[i];
    std::stringstream ss;
    ss << "[CFGTAB-SCAN] fn=+0x" << std::hex << std::uppercase
       << (unsigned long long)(h.fn - moduleBase) << " table=+0x"
       << (unsigned long long)(h.table - moduleBase) << " v31=0x" << h.v31
       << " v41=0x" << h.v41 << " v44=0x" << h.v44 << " v45=0x" << h.v45
       << " v46=0x" << h.v46 << " v53=0x" << h.v53 << " objHits="
       << std::dec << h.objectiveSlcHits;
    if (!h.samples.empty()) {
      ss << " sample=";
      for (size_t si = 0; si < h.samples.size(); ++si) {
        if (si) {
          ss << ",";
        }
        ss << "0x" << std::hex << std::uppercase << h.samples[si].first
           << "->0x" << h.samples[si].second;
      }
    }
    if (i == bestIndex) {
      ss << " [BEST]";
    }
    LogToFile(ss.str());
  }

  const ScanHit &best = hits[bestIndex];
  if (bestScore >= 120) {
    g_ObjRevAltCfgLookupFn.store(best.fn);
    g_ObjRevAltCfgLookupTable.store(best.table);
    char bbuf[256];
    sprintf_s(
        bbuf,
        "[CFGTAB-SCAN] selected fn=+0x%llX table=+0x%llX v45=0x%X v46=0x%X",
        (unsigned long long)(best.fn - moduleBase),
        (unsigned long long)(best.table - moduleBase), best.v45, best.v46);
    LogToFile(bbuf);
  } else {
    LogToFile("[CFGTAB-SCAN] no authoritative alt lookup selected");
  }
}

static bool ObjRev_DecodeCfgBaseFromCaller(uint32_t queryIdx, uintptr_t retAddr,
                                           uint32_t &outCfgIdx,
                                           uint32_t &outBias) {
  outCfgIdx = 0;
  outBias = 0;
  if (retAddr < 16) {
    return false;
  }

  // Expected caller shape:
  //   lea ecx, [r? + disp32]   ; 8D 8? xx xx xx xx
  //   call cfg_to_slc          ; E8 xx xx xx xx
  const uintptr_t leaAddr = retAddr - 11;
  const uintptr_t callAddr = retAddr - 5;
  if (!ObjRev_IsReadable(leaAddr, 11)) {
    return false;
  }
  const uint8_t op = *(volatile uint8_t *)leaAddr;
  const uint8_t modrm = *(volatile uint8_t *)(leaAddr + 1);
  const uint8_t callOp = *(volatile uint8_t *)callAddr;
  if (op != 0x8D || callOp != 0xE8) {
    return false;
  }
  const uint8_t mod = (uint8_t)((modrm >> 6) & 0x3);
  const uint8_t reg = (uint8_t)((modrm >> 3) & 0x7);
  if (mod != 2 || reg != 1) { // require LEA -> ECX with disp32
    return false;
  }

  int32_t disp = *(volatile int32_t *)(leaAddr + 2);
  const int64_t cfg64 = (int64_t)queryIdx - (int64_t)disp;
  if (cfg64 <= 0 || cfg64 > 0xFFFF) {
    return false;
  }
  outCfgIdx = (uint32_t)cfg64;
  outBias = (uint32_t)disp;
  return true;
}

static int ObjRev_ScoreDirectSlcCandidate(uint32_t slcIdx) {
  if (slcIdx == 0) {
    return -1000;
  }
  int score = 0;
  std::string slcKey;
  if (TextHook_GetSlcKey(slcIdx, slcKey) &&
      ObjRev_LooksLikeLocalizationKey(slcKey.c_str())) {
    score += 110;
  }
  std::string raw = ObjRev_ReadSlcStringRaw(slcIdx);
  if (ObjRev_LooksLikeLocalizationKey(raw.c_str())) {
    score += 140;
  } else if (!raw.empty()) {
    score += 5;
  } else {
    score -= 40;
  }
  return score;
}

static bool ObjRev_IsObjectiveCfgActive(uint32_t cfgIdx, unsigned long now) {
  constexpr unsigned long kObjectiveCfgActiveWindowMs = 450;
  std::lock_guard<std::mutex> lock(g_activeObjectiveCfgMutex);
  auto it = g_activeObjectiveCfgTick.find(cfgIdx);
  if (it == g_activeObjectiveCfgTick.end()) {
    return false;
  }
  return (now - it->second) <= kObjectiveCfgActiveWindowMs;
}

static void ObjRev_MarkObjectiveCfgActive(uint32_t cfgIdx, unsigned long now) {
  if (cfgIdx == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_activeObjectiveCfgMutex);
  g_activeObjectiveCfgTick[cfgIdx] = now;
  if (g_activeObjectiveCfgTick.size() > 4096) {
    for (auto it = g_activeObjectiveCfgTick.begin();
         it != g_activeObjectiveCfgTick.end();) {
      if ((now - it->second) > 8000) {
        it = g_activeObjectiveCfgTick.erase(it);
      } else {
        ++it;
      }
    }
  }
}

static void ObjRev_UpdateActiveObjectiveCfg(
    const std::unordered_set<uint32_t> &cfgSet) {
  const unsigned long now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_activeObjectiveCfgMutex);
  for (uint32_t cfg : cfgSet) {
    if (cfg != 0) {
      g_activeObjectiveCfgTick[cfg] = now;
    }
  }
  if (g_activeObjectiveCfgTick.size() > 4096) {
    for (auto it = g_activeObjectiveCfgTick.begin();
         it != g_activeObjectiveCfgTick.end();) {
      if ((now - it->second) > 8000) {
        it = g_activeObjectiveCfgTick.erase(it);
      } else {
        ++it;
      }
    }
  }
}

static bool ObjRev_ResolveObjectiveBias(uint32_t &outBias) {
  outBias = 0;
  const uint32_t cached = g_ObjRevObjectiveBias.load();
  if (cached != 0) {
    outBias = cached;
    return true;
  }

  // Infer bias from recent direct captures that were seen while objective cfgs
  // were active. This is structural provenance, not key-name heuristics.
  const unsigned long now = GetTickCount();
  {
    std::lock_guard<std::mutex> lock(g_cfgDirectMapMutex);
    std::unordered_map<uint32_t, int> biasScore;
    biasScore.reserve(16);
    for (const auto &kv : g_cfgDirectMap) {
      const CfgDirectMapEntry &e = kv.second;
      if (e.bias == 0 || e.slcIdx == 0) {
        continue;
      }
      int score = 1;
      if ((now - e.tick) <= 2000) {
        score += 20;
      }
      if (e.objectiveTick != 0 && (now - e.objectiveTick) <= 5000) {
        score += 240 + (int)((std::min)(e.objectiveHits, (uint32_t)200));
      }
      score += (int)((std::min)(e.hits, (uint32_t)80));
      biasScore[e.bias] += score;
    }

    uint32_t bestBias = 0;
    int bestScore = 0;
    for (const auto &kv : biasScore) {
      if (kv.second > bestScore) {
        bestScore = kv.second;
        bestBias = kv.first;
      }
    }
    if (bestBias != 0 && bestScore >= 260) {
      g_ObjRevObjectiveBias.store(bestBias);
      g_ObjRevObjectiveBiasTick.store(now);
      outBias = bestBias;
      return true;
    }
  }

  // Signature fallback: locate objective configstring callsite and extract LEA
  // displacement (cfg bias) from instruction stream.
  MODULEINFO mi = GetModuleInfo(nullptr);
  if (mi.lpBaseOfDll != nullptr && mi.SizeOfImage > 0) {
    const char pattern[] =
        "\x8D\x8E\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x8B\xC8\xE8\x00\x00\x00\x00\x4C\x8B\xC3\xBA\x00\x04\x00\x00\x48\x8B\xC8\xE8";
    const char mask[] = "xx????x????xxx????xxxxxxxxxxxx";
    void *hit =
        FindPattern((char *)mi.lpBaseOfDll, (size_t)mi.SizeOfImage, pattern, mask);
    if (hit != nullptr) {
      int32_t disp = *(int32_t *)((uintptr_t)hit + 2);
      if (disp > 0 && disp <= 0xFFFF) {
        const uint32_t resolved = (uint32_t)disp;
        g_ObjRevObjectiveBias.store(resolved);
        g_ObjRevObjectiveBiasTick.store(now);
        outBias = resolved;
        char bbuf[128];
        sprintf_s(bbuf, "[CFG-REV-OBJBIAS] resolved=0x%X src=sig", resolved);
        LogToFile(bbuf);
        return true;
      }
    }
  }

  return false;
}

static void ObjRev_RecordDirectCfgSlc(uint32_t cfgIdx, uint32_t bias,
                                      uint32_t slcIdx, uintptr_t callerOffset) {
  if (RuntimeFlags_ObjectiveStatusEnableOsRevOnly()) {
    return;
  }
  if (cfgIdx == 0 || slcIdx == 0) {
    return;
  }
  const bool isBroadCfgProducerCaller =
      ObjRev_IsBulkCfgProducerCaller(callerOffset);
  if (isBroadCfgProducerCaller) {
    // Stability guard:
    // bulk cfg producer walkers are high-frequency and not authority sources.
    // Skip direct-map enrichment from this path.
    return;
  }
  const bool objectiveProducerCaller =
      (ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset) ||
       ObjRev_IsObjectiveConsumerCandidateCaller(callerOffset));
  if (!objectiveProducerCaller && !ObjRev_IsLocalizedConfigStringIndex(cfgIdx)) {
    return;
  }

  const unsigned long now = GetTickCount();

  // Resolve key identity once (slc->key map preferred, then raw/mapped fallback).
  std::string resolvedKey;
  std::string rawPreview;
  std::string slcKey;
  if (TextHook_GetSlcKey(slcIdx, slcKey) &&
      ObjRev_LooksLikeLocalizationKey(slcKey.c_str())) {
    resolvedKey = slcKey;
  } else {
    rawPreview = ObjRev_ReadSlcStringRaw(slcIdx);
    if (ObjRev_LooksLikeLocalizationKey(rawPreview.c_str())) {
      resolvedKey = rawPreview;
    } else if (!rawPreview.empty()) {
      std::string mappedKey;
      const std::string rawPreviewNorm =
          ObjRev_SanitizeEnglishForKeyResolve(rawPreview);
      if (!rawPreviewNorm.empty() &&
          TextHook_ResolveKeyFromEnglish(rawPreviewNorm, mappedKey) &&
          ObjRev_LooksLikeLocalizationKey(mappedKey.c_str())) {
        resolvedKey = mappedKey;
      }
    }
  }
  const uint32_t resolvedKeyClass = ObjRev_ClassifyConsumerKey(resolvedKey);
  const bool resolvedIsObjectiveList = (resolvedKeyClass == 1);
  const bool resolvedIsStatus = (resolvedKeyClass == 2);
  const bool resolvedIsObjectiveLike = resolvedIsObjectiveList;
  const bool objectiveConsumerCandidateCaller =
      ObjRev_IsObjectiveConsumerCandidateCaller(callerOffset);
  auto isCfgTrustedStatusLane = [&](uint32_t cfg) -> bool {
    return TextHook_IsTrustedObjectiveStatusCfgForKey(cfg,
                                                      "GAME_OBJECTIVESUPDATED",
                                                      5000u) ||
           TextHook_IsTrustedObjectiveStatusCfgForKey(cfg,
                                                      "GAME_OBJECTIVECOMPLETED",
                                                      5000u) ||
           TextHook_IsTrustedObjectiveStatusCfgForKey(cfg, "EXE_GAMESAVED",
                                                      5000u) ||
           TextHook_IsTrustedObjectiveStatusCfgForKey(cfg,
                                                      "CGAME_NOW_SAVING",
                                                      5000u) ||
           TextHook_IsTrustedObjectiveStatusCfgForKey(cfg, "OBJECTIVE_FAILED",
                                                      5000u);
  };

  if (objectiveConsumerCandidateCaller && !resolvedIsObjectiveList &&
      !resolvedIsStatus) {
    return;
  }
  if (resolvedIsObjectiveList && isCfgTrustedStatusLane(cfgIdx)) {
    static std::unordered_map<uint32_t, unsigned long> s_statusCfgGuardLogTick;
    static std::mutex s_statusCfgGuardLogMutex;
    bool shouldLogGuard = false;
    char gbuf[320] = {};
    {
      std::lock_guard<std::mutex> lk(s_statusCfgGuardLogMutex);
      auto itGuard = s_statusCfgGuardLogTick.find(cfgIdx);
      if (itGuard == s_statusCfgGuardLogTick.end() ||
          (now - itGuard->second) > 1500) {
        s_statusCfgGuardLogTick[cfgIdx] = now;
        sprintf_s(
            gbuf,
            "[AUTH-DIRECT-STATUS-CFG-GUARD] cfg=0x%X slc=0x%X caller=+0x%llX key=%s",
            cfgIdx, slcIdx, (unsigned long long)callerOffset,
            resolvedKey.c_str());
        shouldLogGuard = true;
      }
    }
    if (shouldLogGuard) {
      LogToFile(gbuf);
    }
    return;
  }

  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(g_cfgDirectMapMutex);
    auto &e = g_cfgDirectMap[cfgIdx];
    const bool objectiveActiveNow = ObjRev_IsObjectiveCfgActive(cfgIdx, now);
    const int newQuality = ObjRev_ScoreDirectSlcCandidate(slcIdx);
    int oldQuality = e.quality;
    if (e.slcIdx != 0 && oldQuality == 0) {
      oldQuality = ObjRev_ScoreDirectSlcCandidate(e.slcIdx);
    }
    const bool hasRecentObjectiveBinding =
        (e.objectiveTick != 0 && (now - e.objectiveTick) <= 1800);

    // Root fix:
    // While this cfg is on a live objective lane (or very recently bound there),
    // ignore non-objective/status candidates to prevent cfg-domain pollution
    // (e.g. LUA_MENU_* overriding GAME_OBJECTIVESUPDATED on cfg=0x45).
    if ((objectiveActiveNow || hasRecentObjectiveBinding) &&
        !resolvedIsObjectiveLike) {
      return;
    }

    if (e.slcIdx == slcIdx) {
      accepted = true;
      e.hits += 1;
      e.quality = newQuality;
      e.keyClass = resolvedKeyClass;
      if (objectiveActiveNow && resolvedIsObjectiveLike) {
        e.objectiveTick = now;
        e.objectiveHits += 1;
      }
    } else if (hasRecentObjectiveBinding && !objectiveActiveNow &&
               !resolvedIsObjectiveLike) {
      // Prevent non-objective traffic from overriding a fresh objective binding.
      return;
    } else if (newQuality > oldQuality + 15 ||
               ((newQuality >= oldQuality) &&
                (e.hits <= 1 || now - e.tick > 2000))) {
      accepted = true;
      e.slcIdx = slcIdx;
      e.hits = 1;
      e.quality = newQuality;
      e.keyClass = resolvedKeyClass;
      if (objectiveActiveNow && resolvedIsObjectiveLike) {
        e.objectiveTick = now;
        e.objectiveHits += 1;
      } else if (!hasRecentObjectiveBinding) {
        e.objectiveTick = 0;
        e.objectiveHits = 0;
      }
    } else {
      // Keep existing stable observation until the new value persists.
      return;
    }
    e.bias = bias;
    e.callerOffset = callerOffset;
    e.tick = now;

    if (g_cfgDirectMap.size() > 4096) {
      for (auto it = g_cfgDirectMap.begin(); it != g_cfgDirectMap.end();) {
        if ((now - it->second.tick) > 12000) {
          it = g_cfgDirectMap.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  if (!accepted) {
    return;
  }

  // Root-fix hard rule:
  // Never recompute cfg->slc in this direct-authority path.
  // Only accept key identity directly resolved from the captured slcIdx.

  const bool isObjectiveSlcAuthorityCaller =
      ObjRev_IsObjectiveAuthoritySlcCaller(callerOffset);
  const bool isCfgProducerCaller = ObjRev_IsBulkCfgProducerCaller(callerOffset);
  if (isCfgProducerCaller) {
    // Root-fix guardrail:
    // cfg_to_slc producer callers are broad cfg traffic and are not render
    // authority. Keep telemetry map updates above, but never promote key
    // authority from this path.
    static std::unordered_map<uint32_t, unsigned long> s_cfgProducerSkipLogTick;
    static std::mutex s_cfgProducerSkipLogMutex;
    bool shouldEmitCfgProducerSkip = false;
    char sbuf[320] = {};
    {
      std::lock_guard<std::mutex> lk(s_cfgProducerSkipLogMutex);
      auto itSkip = s_cfgProducerSkipLogTick.find(cfgIdx);
      if (itSkip == s_cfgProducerSkipLogTick.end() ||
          (now - itSkip->second) > 2000) {
        s_cfgProducerSkipLogTick[cfgIdx] = now;
        sprintf_s(
            sbuf,
            "[AUTH-DIRECT-PROMOTE-SKIP] cfg=0x%X bias=0x%X slc=0x%X caller=+0x%llX reason=cfg_producer_non_authority",
            cfgIdx, bias, slcIdx, (unsigned long long)callerOffset);
        shouldEmitCfgProducerSkip = true;
      }
    }
    if (shouldEmitCfgProducerSkip) {
      LogToFile(sbuf);
    }
    return;
  }
  const bool acceptResolvedKey =
      (!resolvedKey.empty() &&
       ObjRev_LooksLikeLocalizationKey(resolvedKey.c_str()) &&
       (isObjectiveSlcAuthorityCaller
            ? (TextHook_OSAuth_IsObjectiveCaptureCandidateKey(resolvedKey) ||
               IsObjectiveStatusMessageKeyName(resolvedKey))
            : (ObjRev_IsObjectiveListKeyStrict(resolvedKey) &&
               !IsObjectiveStatusMessageKeyName(resolvedKey))));

  // Keep cfg/slc -> key identity available for HUD live-bridge even when this
  // direct path is not objective-authoritative. Without this, keys that come
  // through direct cfg->slc telemetry only (e.g. some CORNERED console prompts)
  // never reach hint/objhint consumers because canonical key reconstruction
  // fails when result text is empty.
  // Status keys (GAME_OBJECTIVESUPDATED, EXE_GAMESAVED, etc.) are now allowed
  // in the SLC cache. They map by configstring, not HudElem identity, so they
  // don't pollute OSAUTH. This lets the HudElem scan resolve status HudElems
  // directly — no oracle guessing needed.
  if (!resolvedKey.empty() && ObjRev_LooksLikeLocalizationKey(resolvedKey.c_str())) {
    std::lock_guard<std::mutex> lk(g_slcToKeyMutex);
    g_slcToKey[slcIdx] = resolvedKey;
    if (g_slcToKey.size() > 16384) {
      g_slcToKey.clear();
    }
  }

  if (!acceptResolvedKey) {
    static std::unordered_map<uint64_t, unsigned long> s_authDirectSkipTick;
    static std::mutex s_authDirectSkipMutex;
    const uint64_t sig = ((uint64_t)cfgIdx << 32) ^ (uint64_t)slcIdx;
    bool shouldEmitAuthDirectSkip = false;
    char sbuf[384] = {};
    {
      std::lock_guard<std::mutex> lk(s_authDirectSkipMutex);
      auto itSkip = s_authDirectSkipTick.find(sig);
      if (itSkip == s_authDirectSkipTick.end() || (now - itSkip->second) > 1800) {
        s_authDirectSkipTick[sig] = now;
        if (s_authDirectSkipTick.size() > 4096) {
          for (auto it = s_authDirectSkipTick.begin();
               it != s_authDirectSkipTick.end();) {
            if ((now - it->second) > 15000) {
              it = s_authDirectSkipTick.erase(it);
            } else {
              ++it;
            }
          }
        }
        std::string preview = rawPreview;
        if (preview.size() > 72) {
          preview.resize(72);
        }
        sprintf_s(
            sbuf,
                  "[AUTH-DIRECT-SKIP] cfg=0x%X bias=0x%X slc=0x%X caller=+0x%llX auth=%d key=%s raw=\"%s\"",
                  cfgIdx, bias, slcIdx, (unsigned long long)callerOffset,
                  isObjectiveSlcAuthorityCaller ? 1 : 0,
                  resolvedKey.empty() ? "<none>" : resolvedKey.c_str(), preview.c_str());
        shouldEmitAuthDirectSkip = true;
      }
    }
    if (shouldEmitAuthDirectSkip) {
      LogToFile(sbuf);
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lk(g_slcToKeyMutex);
    g_slcToKey[slcIdx] = resolvedKey;
    if (g_slcToKey.size() > 16384) {
      g_slcToKey.clear();
    }
  }

  // Direct cfg->slc telemetry must not create objective authority.
}

static bool ObjRev_GetDirectCfgSlc(uint32_t cfgIdx, uint32_t &outSlcIdx,
                                   uint32_t &outBias,
                                   bool preferObjective) {
  outSlcIdx = 0;
  outBias = 0;
  if (cfgIdx == 0) {
    return false;
  }
  const unsigned long now = GetTickCount();
  const bool cfgActiveNow =
      preferObjective ? ObjRev_IsObjectiveCfgActive(cfgIdx, now) : false;
  std::lock_guard<std::mutex> lock(g_cfgDirectMapMutex);
  auto it = g_cfgDirectMap.find(cfgIdx);
  if (it == g_cfgDirectMap.end()) {
    return false;
  }
  if ((now - it->second.tick) > 5000) {
    return false;
  }
  if (preferObjective) {
    const bool hasRecentObjectiveTick =
        (it->second.objectiveTick != 0 &&
         (now - it->second.objectiveTick) <= 3000);
    if (!hasRecentObjectiveTick && !cfgActiveNow) {
      return false;
    }
    const bool entryObjectiveLikeStrict = (it->second.keyClass == 1);
    if (!entryObjectiveLikeStrict) {
      return false;
    }
    // Guard against stale keyClass pollution (e.g. LUA_MENU_* previously
    // misclassified as objective). Re-validate identity from current slc map.
    if (it->second.keyClass == 1 && it->second.slcIdx > 1) {
      std::string verifyKey;
      if (!(TextHook_GetSlcKey(it->second.slcIdx, verifyKey) &&
            ObjRev_LooksLikeLocalizationKey(verifyKey.c_str()))) {
        verifyKey = ObjRev_ReadSlcStringRaw(it->second.slcIdx);
      }
      if (!verifyKey.empty() && !ObjRev_IsObjectiveListKeyStrict(verifyKey)) {
        return false;
      }
    }
    if (cfgActiveNow) {
      it->second.objectiveTick = now;
      it->second.objectiveHits += 1;
    }
  }
  if (!preferObjective && it->second.quality < 40) {
    return false;
  }
  outSlcIdx = it->second.slcIdx;
  outBias = it->second.bias;
  return (outSlcIdx != 0);
}

static std::string ObjRev_ReadSlcStringRaw(uint32_t slcIdx) {
  if (slcIdx == 0 || g_SLC_StringTableGlobal == 0) {
    return "";
  }
  uintptr_t tableBase = 0;
  if (!ObjRev_ReadU64(g_SLC_StringTableGlobal, tableBase) || tableBase == 0) {
    return "";
  }
  uintptr_t keyPtr = tableBase + 4 + ((uintptr_t)slcIdx << 4);
  if (!ObjRev_IsReadable(keyPtr, 1)) {
    return "";
  }

  char keyBuf[128] = {0};
  int wi = 0;
  for (; wi < 120; ++wi) {
    if (!ObjRev_IsReadable(keyPtr + wi, 1)) {
      return "";
    }
    char c = *(volatile char *)(keyPtr + wi);
    keyBuf[wi] = c;
    if (c == '\0') {
      break;
    }
  }
  keyBuf[127] = '\0';
  return std::string(keyBuf);
}

static bool ObjRev_TryAltLookupTable(uint32_t cfgIdx, uint32_t &outSlcIdx) {
  outSlcIdx = 0;
  if (cfgIdx == 0) {
    return false;
  }
  const uintptr_t table = g_ObjRevAltCfgLookupTable.load();
  if (table == 0) {
    return false;
  }
  const uintptr_t entryAddr = table + (uintptr_t)cfgIdx * 4u;
  if (!IsSafeRead((void *)entryAddr, 4)) {
    return false;
  }
  const uint32_t slcIdx = *(volatile uint32_t *)entryAddr;
  if (slcIdx == 0 || slcIdx == 1) {
    return false;
  }

  // Accept only if resolved text identity is a localization key.
  std::string slcKey;
  if (TextHook_GetSlcKey(slcIdx, slcKey) &&
      ObjRev_LooksLikeLocalizationKey(slcKey.c_str())) {
    outSlcIdx = slcIdx;
    return true;
  }
  const std::string raw = ObjRev_ReadSlcStringRaw(slcIdx);
  if (!raw.empty() && ObjRev_LooksLikeLocalizationKey(raw.c_str())) {
    outSlcIdx = slcIdx;
    return true;
  }
  return false;
}

static bool ObjRev_TryConfigToSlc(uint32_t cfgIdx, uint32_t bias, uint32_t &outSlcIdx) {
  outSlcIdx = 0;
  ObjRev_ConfigIndexToSlcFn fn = ObjRev_GetConfigIndexToSlcFn();
  if (fn == nullptr || cfgIdx == 0) {
    return false;
  }
  uint32_t query = cfgIdx + bias;
  if (query == 0) {
    return false;
  }
  uint32_t slcIdx = 0;
  __try {
    slcIdx = fn(query);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  if (slcIdx == 0) {
    return false;
  }
  outSlcIdx = slcIdx;
  return true;
}

static std::mutex g_ObjRevCfgBiasMutex;
static std::unordered_map<uint64_t, uint32_t> g_ObjRevCfgBiasCache;
static std::unordered_map<uint64_t, unsigned long> g_ObjRevCfgProbeTick;
// PERF: permanent negative cache — cfgIndices that completed the full
// 768-iteration probe and found no valid result.  The configstring table is
// fixed at map load, so a failed probe will never succeed within the same map.
// Never re-probe these; the set is cleared on map change via g_ObjRevCfgBiasCache
// overflow (size > 2048) which coincides with level transitions.
static std::unordered_set<uint64_t> g_ObjRevCfgProbeFailSet;

// Called on game reset (mission restart) to allow re-probing in the new map.
void ObjRev_ClearProbeAndBiasCaches() {
  std::lock_guard<std::mutex> lock(g_ObjRevCfgBiasMutex);
  g_ObjRevCfgBiasCache.clear();
  g_ObjRevCfgProbeTick.clear();
  g_ObjRevCfgProbeFailSet.clear();
}

static uint64_t ObjRev_ConfigBiasKey(uint32_t cfgIdx, bool preferObjective) {
  return ((uint64_t)cfgIdx << 1) | (preferObjective ? 1ull : 0ull);
}

static void ObjRev_RememberConfigBias(uint32_t cfgIdx, uint32_t bias,
                                      bool preferObjective) {
  if (cfgIdx == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_ObjRevCfgBiasMutex);
  g_ObjRevCfgBiasCache[ObjRev_ConfigBiasKey(cfgIdx, preferObjective)] = bias;
  if (g_ObjRevCfgBiasCache.size() > 2048) {
    g_ObjRevCfgBiasCache.clear();
  }
}

static bool ObjRev_GetRememberedConfigBias(uint32_t cfgIdx, uint32_t &outBias,
                                           bool preferObjective) {
  outBias = 0;
  std::lock_guard<std::mutex> lock(g_ObjRevCfgBiasMutex);
  auto it =
      g_ObjRevCfgBiasCache.find(ObjRev_ConfigBiasKey(cfgIdx, preferObjective));
  if (it == g_ObjRevCfgBiasCache.end()) {
    return false;
  }
  outBias = it->second;
  return true;
}

static bool ObjRev_ShouldProbeConfigBiasNow(uint32_t cfgIdx,
                                            bool preferObjective) {
  if (cfgIdx == 0) {
    return false;
  }
  const unsigned long now = GetTickCount();
  const uint64_t key = ObjRev_ConfigBiasKey(cfgIdx, preferObjective);
  std::lock_guard<std::mutex> lock(g_ObjRevCfgBiasMutex);
  // PERF: permanent negative cache — never re-probe cfgIndices that already
  // completed the full 768-iteration scan and found nothing.
  if (g_ObjRevCfgProbeFailSet.count(key)) {
    return false;
  }
  auto it = g_ObjRevCfgProbeTick.find(key);
  if (it != g_ObjRevCfgProbeTick.end() && (now - it->second) < 1200) {
    return false;
  }
  g_ObjRevCfgProbeTick[key] = now;
  if (g_ObjRevCfgProbeTick.size() > 2048) {
    g_ObjRevCfgProbeTick.clear();
  }
  return true;
}

static void ObjRev_RecordProbeFailure(uint32_t cfgIdx, bool preferObjective) {
  const uint64_t key = ObjRev_ConfigBiasKey(cfgIdx, preferObjective);
  std::lock_guard<std::mutex> lock(g_ObjRevCfgBiasMutex);
  g_ObjRevCfgProbeFailSet.insert(key);
  if (g_ObjRevCfgProbeFailSet.size() > 4096) {
    g_ObjRevCfgProbeFailSet.clear();
  }
}

static bool ObjRev_ProbeConfigToSlcBias(uint32_t cfgIdx, uint32_t &outSlcIdx,
                                        uint32_t &outBias,
                                        bool preferObjective) {
  outSlcIdx = 0;
  outBias = 0;
  if (cfgIdx == 0 || !ObjRev_ShouldProbeConfigBiasNow(cfgIdx, preferObjective)) {
    return false;
  }

  struct ProbeBest {
    int score = -1;
    uint32_t bias = 0;
    uint32_t slcIdx = 0;
    std::string key;
    std::string raw;
  } best;

  std::unordered_set<uint32_t> seenSlc;
  seenSlc.reserve(256);

  for (uint32_t bias = 0; bias <= 0x300u; ++bias) {
    uint32_t slcIdx = 0;
    if (!ObjRev_TryConfigToSlc(cfgIdx, bias, slcIdx)) {
      continue;
    }
    if (!seenSlc.insert(slcIdx).second) {
      continue;
    }

    int score = 0;
    std::string pickedKey;
    std::string raw = ObjRev_ReadSlcStringRaw(slcIdx);
    std::string slcKey;
    if (TextHook_GetSlcKey(slcIdx, slcKey) &&
        ObjRev_LooksLikeLocalizationKey(slcKey.c_str())) {
      pickedKey = slcKey;
      score += 340;
      if (IsObjectiveOverlayKeyName(slcKey)) {
        score += 40;
      }
    }

    if (!raw.empty()) {
      score += 8;
      if (ObjRev_LooksLikeLocalizationKey(raw.c_str())) {
        if (pickedKey.empty()) {
          pickedKey = raw;
        }
        score += 320;
        if (IsObjectiveOverlayKeyName(raw)) {
          score += 40;
        }
      } else {
        std::string mappedKey;
        const std::string rawNorm = ObjRev_SanitizeEnglishForKeyResolve(raw);
        if (!rawNorm.empty() &&
            TextHook_ResolveKeyFromEnglish(rawNorm, mappedKey) &&
            !mappedKey.empty()) {
          if (pickedKey.empty()) {
            pickedKey = mappedKey;
          }
          score += 230;
          if (IsObjectiveOverlayKeyName(mappedKey)) {
            score += 40;
          }
        }
      }
    }

    if (preferObjective) {
      if (pickedKey.empty()) {
        score -= 120;
      } else if (ObjRev_IsObjectiveListKeyStrict(pickedKey)) {
        score += 280;
      } else if (IsObjectiveStatusMessageKeyName(pickedKey)) {
        // Status keys are valid — they indicate the HudElem currently shows
        // a status message. Don't penalize.
        score += 160;
      } else {
        HudKeyType kt = ClassifyHudKey(pickedKey);
        if (kt == HUD_KEY_HINT || kt == HUD_KEY_OBJ_HUD_HINT) {
          score -= 360;
        } else {
          score -= 240;
        }
      }
    }

    if (score > best.score) {
      best.score = score;
      best.bias = bias;
      best.slcIdx = slcIdx;
      best.key = pickedKey;
      best.raw = raw;
    }
  }

  // Require a resolved key-level match to accept probe result.
  const int minScore = preferObjective ? 360 : 220;
  const bool probeKeyIsUnsafeGenericPrompt =
      (!preferObjective &&
       (IsAmbiguousGenericPlatformHoldKey(best.key) ||
        IsHudRuntimeTailJoinPromptKey(ToUpperAscii(TrimSpaces(best.key)))));
  const bool keyFitsPreferObjective =
      (ObjRev_IsObjectiveListKeyStrict(best.key) ||
       IsObjectiveStatusMessageKeyName(best.key));
  const bool keyAccepted =
      (!best.key.empty() &&
       (!preferObjective || keyFitsPreferObjective) &&
       !probeKeyIsUnsafeGenericPrompt);
  if (best.score >= minScore && best.slcIdx != 0 && keyAccepted) {
    ObjRev_RememberConfigBias(cfgIdx, best.bias, preferObjective);
    outSlcIdx = best.slcIdx;
    outBias = best.bias;

    {
      std::lock_guard<std::mutex> lk(g_slcToKeyMutex);
      g_slcToKey[best.slcIdx] = best.key;
      if (g_slcToKey.size() > 16384) {
        g_slcToKey.clear();
      }
    }

    char pbuf[384];
    sprintf_s(pbuf,
              "[CFG-REV-PROBE] cfg=0x%X pref=%s bias=0x%X slc=0x%X score=%d key=%s raw=\"%.72s\"",
              cfgIdx, preferObjective ? "obj" : "any", best.bias, best.slcIdx,
              best.score, best.key.c_str(),
              best.raw.c_str());
    LogToFile(pbuf);
    return true;
  }

  static std::unordered_map<uint32_t, unsigned long> s_probeMissLogTick;
  const unsigned long now = GetTickCount();
  const uint32_t missKey = cfgIdx ^ (preferObjective ? 0x80000000u : 0u);
  auto itMiss = s_probeMissLogTick.find(missKey);
  if (itMiss == s_probeMissLogTick.end() || (now - itMiss->second) > 1800) {
    s_probeMissLogTick[missKey] = now;
    char mbuf[192];
    sprintf_s(mbuf, "[CFG-REV-PROBE-MISS] cfg=0x%X pref=%s", cfgIdx,
              preferObjective ? "obj" : "any");
    LogToFile(mbuf);
  }
  // PERF: record failure so we don't re-probe this cfgIndex for 120 seconds.
  ObjRev_RecordProbeFailure(cfgIdx, preferObjective);
  return false;
}

static bool ObjRev_TryConfigToSlcAny(uint32_t cfgIdx, uint32_t &outSlcIdx,
                                     uint32_t &outBias,
                                     bool preferObjective = false,
                                     bool directOnly = false) {
  outSlcIdx = 0;
  outBias = 0;
  if (ObjRev_GetDirectCfgSlc(cfgIdx, outSlcIdx, outBias, preferObjective)) {
    return true;
  }
  if (preferObjective && cfgIdx != 0) {
    uint32_t tableSlc = 0;
    if (ObjRev_TryAltLookupTable(cfgIdx, tableSlc)) {
      outSlcIdx = tableSlc;
      outBias = 0;
      ObjRev_RecordDirectCfgSlc(cfgIdx, 0, tableSlc, g_ObjRevAltCfgLookupFn.load());
      static std::unordered_map<uint32_t, unsigned long> s_altHitLogTick;
      const unsigned long now = GetTickCount();
      auto it = s_altHitLogTick.find(cfgIdx);
      if (it == s_altHitLogTick.end() || (now - it->second) > 1500) {
        s_altHitLogTick[cfgIdx] = now;
        char lbuf[256];
        sprintf_s(lbuf,
                  "[CFGTAB-HIT] cfg=0x%X slc=0x%X fn=+0x%llX",
                  cfgIdx, tableSlc,
                  (unsigned long long)(g_ObjRevAltCfgLookupFn.load() -
                                       (uintptr_t)GetModuleHandleA(NULL)));
        LogToFile(lbuf);
      }
      return true;
    }
  }
  if (directOnly) {
    if (preferObjective && cfgIdx != 0) {
      uint32_t objectiveBias = 0;
      if (ObjRev_ResolveObjectiveBias(objectiveBias) && objectiveBias != 0) {
        uint32_t slcIdx = 0;
        if (ObjRev_TryConfigToSlc(cfgIdx, objectiveBias, slcIdx)) {
          outSlcIdx = slcIdx;
          outBias = objectiveBias;
          ObjRev_RememberConfigBias(cfgIdx, objectiveBias, true);
          ObjRev_RecordDirectCfgSlc(cfgIdx, objectiveBias, slcIdx, 0);
          return true;
        }
      }
    }
    return false;
  }
  uint32_t cachedBias = 0;
  if (ObjRev_GetRememberedConfigBias(cfgIdx, cachedBias, preferObjective)) {
    uint32_t slcIdx = 0;
    if (ObjRev_TryConfigToSlc(cfgIdx, cachedBias, slcIdx)) {
      outSlcIdx = slcIdx;
      outBias = cachedBias;
      return true;
    }
  }

  constexpr uint32_t kBiases[] = {0xACu, 0x8Cu};
  for (uint32_t bias : kBiases) {
    uint32_t slcIdx = 0;
    if (ObjRev_TryConfigToSlc(cfgIdx, bias, slcIdx)) {
      ObjRev_RememberConfigBias(cfgIdx, bias, preferObjective);
      outSlcIdx = slcIdx;
      outBias = bias;
      return true;
    }
  }

  if (ObjRev_ProbeConfigToSlcBias(cfgIdx, outSlcIdx, outBias,
                                  preferObjective)) {
    return true;
  }
  return false;
}

static std::string ObjRev_ReadConfigStringRaw(uint32_t cfgIdx) {
  if (cfgIdx == 0) {
    return "";
  }

  // Authoritative path: cfgIndex -> (cfg_to_slc with lane bias) -> SLC string.
  // +0xAC and +0x8C are observed in HUD callsites for cfg-indexed string reads.
  uint32_t slcIdx = 0;
  uint32_t bias = 0;
  if (ObjRev_TryConfigToSlcAny(cfgIdx, slcIdx, bias)) {
    std::string raw = ObjRev_ReadSlcStringRaw(slcIdx);
    // Credits keys must never enter objective/HUD pipeline.
    // Skip CREDITS_ entries from CFG-REV — not useful for objective resolution.
    if (!raw.empty() && raw.rfind("CREDITS_", 0) == 0) {
      return raw; // Return string but skip logging / further processing
    }
    if (!raw.empty()) {
      static std::unordered_map<uint64_t, unsigned long> s_cfgResolveLogTick;
      uint64_t sig = ((uint64_t)cfgIdx << 32) | (uint64_t)bias;
      unsigned long now = GetTickCount();
      auto it = s_cfgResolveLogTick.find(sig);
      if (it == s_cfgResolveLogTick.end() || (now - it->second) > 1500) {
        s_cfgResolveLogTick[sig] = now;
        char lbuf[320];
        sprintf_s(lbuf,
                  "[CFG-REV] cfg=0x%X bias=0x%X slc=0x%X raw=\"%.72s\"",
                  cfgIdx, bias, slcIdx, raw.c_str());
        LogToFile(lbuf);
      }
      return raw;
    }
  }

  // Last fallback only: treat cfg as direct string id (legacy behavior).
  // This path is non-authoritative and exists to avoid hard regressions while
  // resolver function discovery stabilizes.
  return ObjRev_ReadSlcStringRaw(cfgIdx);
}

// Direct configstring resolver (no g_slcToKey cache dependency).
// Uses cfgIndex->stringId engine resolver first, then maps English->key if needed.
static std::string ObjRev_ResolveConfigKey(uint32_t cfgIdx,
                                           bool preferObjective = false,
                                           bool directOnly = false) {
  uint32_t slcIdx = 0;
  uint32_t bias = 0;
  std::string raw;
  auto LogResolvedKey = [&](const char *source, const std::string &key) {
    if (!kVerboseRuntimeLogs || key.empty()) {
      return;
    }
    static std::unordered_map<uint64_t, unsigned long> s_cfgKeyLogTick;
    uint64_t sig = ((uint64_t)cfgIdx << 32) | (uint64_t)slcIdx;
    unsigned long now = GetTickCount();
    auto it = s_cfgKeyLogTick.find(sig);
    if (it == s_cfgKeyLogTick.end() || (now - it->second) > 1500) {
      s_cfgKeyLogTick[sig] = now;
      char lbuf[384];
      sprintf_s(lbuf,
                "[CFG-REV-KEY] cfg=0x%X pref=%s bias=0x%X slc=0x%X src=%s key=%s",
                cfgIdx, preferObjective ? "obj" : "any", bias, slcIdx,
                source ? source : "unknown",
                key.c_str());
      LogToFile(lbuf);
    }
  };

  const bool requireObjectiveKey = preferObjective;
  auto IsPreferredObjectiveKey = [&](const std::string &candidate) -> bool {
    if (candidate.empty()) {
      return false;
    }
    if (!requireObjectiveKey) {
      return true;
    }
    if (ObjRev_IsObjectiveListKeyStrict(candidate)) {
      return true;
    }
    // Accept status keys directly — the SLC cache maps by configstring,
    // not by HudElem identity. When the game displays "Objectives Updated"
    // on a HudElem, this lets the scan resolve the key with correct metadata
    // (fxBirthTime, alpha, position) from the actual HudElem.
    if (IsObjectiveStatusMessageKeyName(candidate)) {
      return true;
    }
    return false;
  };

  if (ObjRev_TryConfigToSlcAny(cfgIdx, slcIdx, bias, preferObjective,
                               directOnly)) {
    std::string slcKey;
    if (TextHook_GetSlcKey(slcIdx, slcKey) &&
        ObjRev_LooksLikeLocalizationKey(slcKey.c_str()) &&
        IsPreferredObjectiveKey(slcKey)) {
      // Credits keys: skip objective/HUD pipeline entirely.
      if (slcKey.rfind("CREDITS_", 0) == 0) {
        return "";
      }
      LogResolvedKey("slc_cache", slcKey);
      return slcKey;
    }
    raw = ObjRev_ReadSlcStringRaw(slcIdx);
    // Early exit for credits keys resolved from raw SLC.
    if (!raw.empty() && raw.rfind("CREDITS_", 0) == 0) {
      return "";
    }
  } else if (directOnly) {
    if (kVerboseRuntimeLogs) {
      static std::unordered_map<uint32_t, unsigned long> s_cfgDirectMissLogTick;
      unsigned long now = GetTickCount();
      auto it = s_cfgDirectMissLogTick.find(cfgIdx);
      if (it == s_cfgDirectMissLogTick.end() || (now - it->second) > 1200) {
        s_cfgDirectMissLogTick[cfgIdx] = now;
        char dbuf[192];
        sprintf_s(dbuf, "[CFG-REV-DIRECT-MISS] cfg=0x%X pref=%s", cfgIdx,
                  preferObjective ? "obj" : "any");
        LogToFile(dbuf);
      }
    }
    return "";
  }

  if (!directOnly && raw.empty()) {
    raw = ObjRev_ReadConfigStringRaw(cfgIdx);
  }
  if (raw.empty()) {
    return "";
  }
  // Credits keys must not enter objective/HUD pipeline from any path.
  if (raw.rfind("CREDITS_", 0) == 0) {
    return "";
  }

  if (ObjRev_LooksLikeLocalizationKey(raw.c_str()) &&
      IsPreferredObjectiveKey(raw)) {
    LogResolvedKey("raw_key", raw);
    return raw;
  }

  // Early-exit: skip expensive resolution for obvious non-translation strings
  // (asset names, numbers, single chars, VFX paths).
  if (ObjRev_IsObviouslyNotTranslatable(raw)) {
    return "";
  }

  std::string mappedKey;
  const std::string rawNorm = ObjRev_SanitizeEnglishForKeyResolve(raw);
  if (!rawNorm.empty() &&
      TextHook_ResolveKeyFromEnglish(rawNorm, mappedKey) &&
      !mappedKey.empty() && IsPreferredObjectiveKey(mappedKey)) {
    LogResolvedKey("english_map", mappedKey);
    return mappedKey;
  }

  // If the default bias path produced no key, force a one-shot bias probe.
  // This is critical for objective cfg indexes where +0xAC/+0x8C can resolve
  // a string id that is not the actual objective text key.
  uint32_t probeSlc = 0;
  uint32_t probeBias = 0;
  if (!directOnly && !preferObjective &&
      ObjRev_ProbeConfigToSlcBias(cfgIdx, probeSlc, probeBias,
                                  preferObjective)) {
    slcIdx = probeSlc;
    bias = probeBias;
    std::string probeKey;
    if (TextHook_GetSlcKey(slcIdx, probeKey) &&
        ObjRev_LooksLikeLocalizationKey(probeKey.c_str()) &&
        IsPreferredObjectiveKey(probeKey)) {
      LogResolvedKey("probe_cache", probeKey);
      return probeKey;
    }
    std::string probeRaw = ObjRev_ReadSlcStringRaw(slcIdx);
    if (ObjRev_LooksLikeLocalizationKey(probeRaw.c_str()) &&
        IsPreferredObjectiveKey(probeRaw)) {
      LogResolvedKey("probe_raw_key", probeRaw);
      return probeRaw;
    }
    std::string probeMapped;
    const std::string probeNorm =
        ObjRev_SanitizeEnglishForKeyResolve(probeRaw);
    if (!probeNorm.empty() &&
        TextHook_ResolveKeyFromEnglish(probeNorm, probeMapped) &&
        !probeMapped.empty() && IsPreferredObjectiveKey(probeMapped)) {
      LogResolvedKey("probe_english_map", probeMapped);
      return probeMapped;
    }
    raw = probeRaw;
  }

  if (kVerboseRuntimeLogs) {
    static std::unordered_map<uint32_t, unsigned long> s_cfgNoKeyLogTick;
    unsigned long nowNoKey = GetTickCount();
    auto itNoKey = s_cfgNoKeyLogTick.find(cfgIdx);
    if (itNoKey == s_cfgNoKeyLogTick.end() || (nowNoKey - itNoKey->second) > 1200) {
      s_cfgNoKeyLogTick[cfgIdx] = nowNoKey;
      char nbuf[384];
      sprintf_s(nbuf,
                "[CFG-REV-NOKEY] cfg=0x%X pref=%s bias=0x%X slc=0x%X raw=\"%.72s\"",
                cfgIdx, preferObjective ? "obj" : "any", bias, slcIdx,
                raw.c_str());
      LogToFile(nbuf);
    }
  }
  return "";
}

static bool ObjRev_IsPlausibleConfigIndex(uint32_t cfgIdx) {
  // string_t/configstring id in this build is a compact index space.
  // Pointer-like values (e.g. 0x43xxxxxx) are structural noise, not keys.
  return (cfgIdx > 0 && cfgIdx <= 0xFFFFu);
}

static uint64_t ObjRev_MakeAuthoritativeInstanceKey(uint32_t cfgIndex,
                                                    int fxBirth,
                                                    int elemIndex) {
  return ((uint64_t)cfgIndex << 32) ^
         ((uint64_t)(uint32_t)fxBirth << 16) ^
         (uint64_t)(uint32_t)(elemIndex & 0xFFFF);
}

static uint64_t ObjRev_MakeAuthoritativeCfgBirthKey(uint32_t cfgIndex,
                                                    int fxBirth) {
  return ((uint64_t)cfgIndex << 32) | (uint64_t)(uint32_t)fxBirth;
}

static int ObjRev_ResolveHudElemArrayIndexFromLayout(uintptr_t elemPtr,
                                                     uintptr_t baseAddr,
                                                     int stride,
                                                     int count) {
  if (elemPtr == 0 || baseAddr == 0 || stride <= 0 || count <= 0) {
    return -1;
  }
  if (elemPtr < baseAddr) {
    return -1;
  }
  uintptr_t delta = elemPtr - baseAddr;
  if ((delta % (uintptr_t)stride) != 0) {
    return -1;
  }
  int idx = (int)(delta / (uintptr_t)stride);
  if (idx < 0 || idx >= count) {
    return -1;
  }
  return idx;
}

static int ObjRev_ResolveHudElemArrayIndex(uintptr_t elemPtr) {
  if (elemPtr <= 0x10000 || elemPtr >= 0x7FFFFFFFFFFF) {
    return -1;
  }

  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  const uintptr_t fixedBase = s_moduleBase + IW6Offsets::HudElem_Array_SP;
  const int fixedStride = IW6Offsets::HudElem_Array_Stride_SP;
  const int fixedCount = 1024;
  int idx = ObjRev_ResolveHudElemArrayIndexFromLayout(
      elemPtr, fixedBase, fixedStride, fixedCount);
  if (idx >= 0) {
    return idx;
  }

  std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
  if ((g_hudElemArray.valid || g_hudElemArray.tentative) &&
      g_hudElemArray.baseAddr != 0 && g_hudElemArray.stride > 0 &&
      g_hudElemArray.count > 0) {
    idx = ObjRev_ResolveHudElemArrayIndexFromLayout(
        elemPtr, g_hudElemArray.baseAddr, g_hudElemArray.stride,
        g_hudElemArray.count);
    if (idx >= 0) {
      return idx;
    }
  }
  return -1;
}

static bool ObjRev_IsReadable(uintptr_t p, size_t bytes) {
  return (p > 0x10000 && p < 0x7FFFFFFFFFFF &&
          IsBadReadPtr((void *)p, bytes) == 0);
}

// Check if a memory address looks like a valid hudelem_s with reasonable fields
static bool ObjRev_LooksLikeHudElem(uintptr_t addr) {
  if (!ObjRev_IsReadable(addr, 0xA8)) return false;
  uint32_t type = 0;
  if (!ObjRev_ReadU32(addr + 0x00, type)) return false;
  // type should be 0 (FREE) through ~13 (max known type)
  if (type > 20) return false;
  if (type == 0) return true; // FREE is valid (empty slot)
  // For active elements, check x/y are plausible
  float x = 0, y = 0;
  if (!ObjRev_ReadF32(addr + 0x04, x)) return false;
  if (!ObjRev_ReadF32(addr + 0x08, y)) return false;
  return (x > -10000.0f && x < 10000.0f && y > -10000.0f && y < 10000.0f);
}

void TextHook_RunHudElemArrayScan() {
  static unsigned long s_lastScan = 0;
  unsigned long now = GetTickCount();
  if ((now - s_lastScan) < 2000) return; // Scan at most every 2s
  s_lastScan = now;

  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  const uintptr_t fixedBase = s_moduleBase + IW6Offsets::HudElem_Array_SP;
  const int fixedStride = IW6Offsets::HudElem_Array_Stride_SP;

  // The reversed fixed SP HudElem array is the current low-cost fast-path
  // candidate. When it is readable we prefer it for polling, but this does not
  // rule out other producer/consumer paths for timer localization work.
  if (ObjRev_IsReadable(fixedBase, 0xA8) && fixedStride > 0) {
    std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
    g_hudElemArray.baseAddr = fixedBase;
    g_hudElemArray.stride = fixedStride;
    g_hudElemArray.count = 1024;
    g_hudElemArray.tentative = true;
    g_hudElemArray.valid = true;
    return;
  }

  uintptr_t knownPtr = g_introHudElemPtr;
  if (knownPtr == 0) return; // No intro HudElem captured yet

  // Phase 1D: Server time offset
  {
    int serverTime = g_introHudElemServerTime;
    unsigned long captureTick = g_introHudElemTick;
    if (serverTime > 0 && captureTick > 0) {
      long offset = (long)captureTick - (long)serverTime;
      static long s_lastOffset = 0;
      if (offset != s_lastOffset) {
        char buf[256];
        sprintf_s(buf,
                  "[OBJ-REVERSE] serverTime=%d tick=%lu offset=%ld "
                  "(delta from last=%ld)",
                  serverTime, captureTick, offset, offset - s_lastOffset);
        LogToFile(buf);
        s_lastOffset = offset;
      }
    }
  }

  // If we already have a tentative/confirmed base, use it instead of the
  // (possibly stale) intro pointer for scanning.
  uintptr_t scanBase = 0;
  int scanStride = 0;
  {
    std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
    if (g_hudElemArray.tentative || g_hudElemArray.valid) {
      scanBase = g_hudElemArray.baseAddr;
      scanStride = g_hudElemArray.stride;
    }
  }

  if (scanBase != 0 && scanStride > 0) {
    // --- FULL ARRAY SCAN from known base ---
    // IW6 HudElem array is typically 1024 elements. Scan all of them.
    const int kMaxElems = 1024;
    int activeCount = 0, textCount = 0, objectiveCount = 0;
    int textLogged = 0;
    int slcFailCount = 0;

    // Build a reverse map from SLC index ??key name (from the SLC hook's
    // observations). This lets us identify objective keys even when
    // SL_ConvertToString returns English text instead of the key name.
    // The SLC hook stores slcIndex ??key in g_SLC_SlcToKey (see SLC hook).
    // For now, use position-based detection as well.

    for (int i = 0; i < kMaxElems; ++i) {
      uintptr_t elem = scanBase + (uintptr_t)i * scanStride;
      if (!ObjRev_IsReadable(elem, 0xA8)) break; // past valid memory
      uint32_t t = 0;
      if (!ObjRev_ReadU32(elem, t)) continue;
      if (t == 0) continue; // FREE
      if (t > 20) continue; // unexpected type — skip, don't abort scan
      activeCount++;
      if (t == 1) { // TEXT
        textCount++;
        float ex = 0, ey = 0, efs = 0;
        uint32_t ecolor = 0;
        int eFxBirth = 0, eFxLetter = 0, eFlags = 0;
        ObjRev_ReadF32(elem + 0x04, ex);
        ObjRev_ReadF32(elem + 0x08, ey);
        ObjRev_ReadF32(elem + 0x14, efs);
        ObjRev_ReadU32(elem + 0x30, ecolor);
        ObjRev_ReadI32(elem + 0x90, eFxBirth);
        ObjRev_ReadI32(elem + 0x94, eFxLetter);
        ObjRev_ReadI32(elem + 0xA4, eFlags);
        float alpha = (float)((ecolor >> 24) & 0xFF) / 255.0f;

        uint32_t textSlc = 0;
        ObjRev_ReadU32(elem + 0x84, textSlc);

        // Skip elements tracked by the intro system (prevents crash from
        // processing intro HudElems as objectives — intro title at y=-82
        // was passing objective score filter and causing OBJ-UNRESOLVED spam).
        // Do not skip objective-position elements (the intro detour captures
        // all HudElem renders, including objectives).
        {
          bool posLooksObj = (ex > 5.5f && ex < 6.5f && ey >= 9.0f && ey <= 91.0f &&
                              efs > 1.2f && efs < 1.3f) ||
                             (ex < 0.5f && ey < -60.0f && efs > 1.9f);
          if (textSlc > 0 && !posLooksObj) {
            std::lock_guard<std::mutex> introLk(g_nativeIntroMutex);
            if (g_nativeIntroMap.count(textSlc) > 0) continue;
          }
        }

        // Resolve configstring index via Phase 2A mapping
        std::string resolvedStr = ObjRev_ResolveConfigKey(textSlc, false, true);
        const char *resolved = resolvedStr.c_str();

        // Position-based objective detection:
        // GSC objectives use x=6, y=10+n*20, fs=1.25, flags=0x5
        bool posMatchObj = (ex > 5.5f && ex < 6.5f &&
                            ey >= 9.0f && ey <= 91.0f &&
                            efs > 1.2f && efs < 1.3f);
        // Also check for "Objectives Updated" style (x=0, y=-68, fs=2.0)
        bool posMatchObjUpdate = (ex < 0.5f && ey < -60.0f && efs > 1.9f);

        bool isObj = posMatchObj || posMatchObjUpdate;
        if (isObj) objectiveCount++;

        // Log ALL TEXT elements (first 30 per scan)
        if (textLogged < 30) {
          textLogged++;
          int eFadeStart = 0, eFadeTime = 0, eTime = 0;
          uint32_t efromColor = 0;
          ObjRev_ReadU32(elem + 0x34, efromColor);
          ObjRev_ReadI32(elem + 0x38, eFadeStart);
          ObjRev_ReadI32(elem + 0x3C, eFadeTime);
          ObjRev_ReadI32(elem + 0x78, eTime);

          char ebuf[512];
          sprintf_s(
              ebuf,
              "[OBJ-REVERSE] ELEM[%d] %s slc=0x%X text=\"%.60s\" "
              "x=%.1f y=%.1f fs=%.2f a=%.2f color=0x%08X "
              "fadeS=%d fadeT=%d fxB=%d fxL=%d time=%d flags=0x%X",
              i, isObj ? "OBJ" : "hud", textSlc, resolved,
              ex, ey, efs, alpha, ecolor, eFadeStart, eFadeTime,
              eFxBirth, eFxLetter, eTime, eFlags);
          LogToFile(ebuf);
        }

        if (resolved[0] == '\0' || resolved[0] == '(') {
          slcFailCount++;
        }
      }
    }

    char abuf[384];
    sprintf_s(abuf,
              "[OBJ-REVERSE] FULLSCAN stride=0x%X base=module+0x%llX "
              "active=%d text=%d obj=%d slcFail=%d",
              scanStride, (unsigned long long)(scanBase - s_moduleBase),
              activeCount, textCount, objectiveCount, slcFailCount);
    LogToFile(abuf);

    if (objectiveCount > 0) {
      std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
      if (!g_hudElemArray.valid) {
        g_hudElemArray.valid = true;
        g_hudElemArray.count = kMaxElems;
        char sbuf[256];
        sprintf_s(sbuf,
                  "[OBJ-REVERSE] === ARRAY CONFIRMED === stride=0x%X "
                  "module+0x%llX objKeys=%d",
                  scanStride, (unsigned long long)(scanBase - s_moduleBase),
                  objectiveCount);
        LogToFile(sbuf);
      }
    }
    return; // Full scan done, skip discovery scan
  }

  // --- DISCOVERY: find array base from intro pointer ---

  // Phase 1A: Log the captured pointer (diagnostic only, every 5s)
  {
    uint32_t type = 0, slc = 0;
    float x = 0, y = 0, fs = 0;
    ObjRev_ReadU32(knownPtr + 0x00, type);
    ObjRev_ReadF32(knownPtr + 0x04, x);
    ObjRev_ReadF32(knownPtr + 0x08, y);
    ObjRev_ReadF32(knownPtr + 0x14, fs);
    ObjRev_ReadU32(knownPtr + 0x84, slc);

    std::string slcResolved = ObjRev_ResolveConfigKey(slc, false, true);
    const char *slcStr = slcResolved.c_str();

    static unsigned long s_lastPtrLog = 0;
    if ((now - s_lastPtrLog) > 5000) {
      s_lastPtrLog = now;
      char buf[384];
      sprintf_s(buf,
                "[OBJ-REVERSE] INTRO ptr=0x%llX (module+0x%llX) type=%u "
                "x=%.1f y=%.1f fs=%.2f slc=0x%X text=\"%s\"",
                (unsigned long long)knownPtr,
                (unsigned long long)(knownPtr - s_moduleBase), type, x, y, fs,
                slc, slcStr);
      LogToFile(buf);
    }
  }

  // Phase 1B: Array boundary scan
  // Try stride 0xB8 (game_hudelem_s) first, then 0xA8 (hudelem_s)
  const int strides[] = {0xB8, 0xA8};
  for (int si = 0; si < 2; ++si) {
    int stride = strides[si];

    // Scan backward to find array start
    int backCount = 0;
    uintptr_t scanPtr = knownPtr;
    for (int i = 0; i < 2048; ++i) {
      uintptr_t prev = scanPtr - stride;
      if (!ObjRev_LooksLikeHudElem(prev)) break;
      scanPtr = prev;
      backCount++;
    }
    uintptr_t arrayBase = scanPtr;

    // Scan forward: IW6 has up to 1024 elements.
    // Use a high threshold (1024 consecutive FREE) to ensure we scan
    // the full array even if the intro element is near the start.
    int fwdCount = 0;
    scanPtr = arrayBase + (uintptr_t)(backCount + 1) * stride;
    int consecutiveFree = 0;
    for (int i = 0; i < 2048; ++i) {
      if (!ObjRev_IsReadable(scanPtr, 0xA8)) break;
      uint32_t t = 0;
      if (!ObjRev_ReadU32(scanPtr, t)) break;
      if (t > 20) { fwdCount++; scanPtr = arrayBase + (uintptr_t)(backCount + 1 + fwdCount) * stride; continue; }
      if (t == 0) {
        consecutiveFree++;
        if (consecutiveFree > 1024) {
          fwdCount -= 1023;
          break;
        }
      } else {
        consecutiveFree = 0;
      }
      fwdCount++;
      scanPtr = arrayBase + (uintptr_t)(backCount + 1 + fwdCount) * stride;
    }

    int totalCount = backCount + 1 + fwdCount;

    // Count active, text, and objective elements; log ALL text elements
    int activeCount = 0, textCount = 0, objectiveCount = 0;
    int textLogged = 0;
    for (int i = 0; i < totalCount && i < 2048; ++i) {
      uintptr_t elem = arrayBase + (uintptr_t)i * stride;
      uint32_t t = 0;
      if (!ObjRev_ReadU32(elem, t)) continue;
      if (t == 0) continue;
      activeCount++;
      if (t == 1) {
        textCount++;
        uint32_t textSlc = 0;
        if (ObjRev_ReadU32(elem + 0x84, textSlc) && textSlc != 0) {
          std::string resolvedStr = ObjRev_ResolveConfigKey(textSlc, false, true);
          const char *resolved = resolvedStr.c_str();
          if (resolved[0] != '\0') {
            std::string keyStr(resolved);
            bool isObj = IsObjectiveOverlayKeyName(keyStr);
            if (isObj) objectiveCount++;

            // Log ALL TEXT elements (first 20 per scan)
            if (textLogged < 20) {
              textLogged++;
              float ex = 0, ey = 0, efs = 0;
              uint32_t ecolor = 0;
              int eTime = 0;
              ObjRev_ReadF32(elem + 0x04, ex);
              ObjRev_ReadF32(elem + 0x08, ey);
              ObjRev_ReadF32(elem + 0x14, efs);
              ObjRev_ReadU32(elem + 0x30, ecolor);
              ObjRev_ReadI32(elem + 0x78, eTime);
              float alpha = (float)((ecolor >> 24) & 0xFF) / 255.0f;

              char ebuf[384];
              sprintf_s(ebuf,
                        "[OBJ-REVERSE] DISC-ELEM[%d] %s key=\"%s\" x=%.1f "
                        "y=%.1f fs=%.2f a=%.2f time=%d",
                        i, isObj ? "OBJ" : "hud", keyStr.c_str(), ex, ey,
                        efs, alpha, eTime);
              LogToFile(ebuf);
            }
          }
        }
      }
    }

    bool plausible = (totalCount >= 64 && totalCount <= 2048 &&
                      activeCount >= 1 && textCount >= 1);

    char abuf[384];
    sprintf_s(abuf,
              "[OBJ-REVERSE] ARRAY stride=0x%X base=0x%llX "
              "(module+0x%llX) count=%d active=%d text=%d obj=%d "
              "plausible=%d",
              stride, (unsigned long long)arrayBase,
              (unsigned long long)(arrayBase - s_moduleBase), totalCount,
              activeCount, textCount, objectiveCount, plausible ? 1 : 0);
    LogToFile(abuf);

    // Store base when plausible (even without objectives yet).
    // Once stored as tentative, full-array scan will run on next cycle.
    if (plausible) {
      std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
      if (!g_hudElemArray.tentative && !g_hudElemArray.valid) {
        g_hudElemArray.baseAddr = arrayBase;
        g_hudElemArray.stride = stride;
        g_hudElemArray.count = totalCount;
        g_hudElemArray.tentative = true;

        char sbuf[256];
        sprintf_s(sbuf,
                  "[OBJ-REVERSE] === TENTATIVE BASE === stride=0x%X "
                  "module+0x%llX count=%d (will full-scan next cycle)",
                  stride, (unsigned long long)(arrayBase - s_moduleBase),
                  totalCount);
        LogToFile(sbuf);
      }
      if (objectiveCount > 0) {
        g_hudElemArray.valid = true;
        char sbuf[256];
        sprintf_s(sbuf,
                  "[OBJ-REVERSE] === ARRAY CONFIRMED === stride=0x%X "
                  "module+0x%llX count=%d objKeys=%d",
                  stride, (unsigned long long)(arrayBase - s_moduleBase),
                  totalCount, objectiveCount);
        LogToFile(sbuf);
      }
      break; // Don't try other strides
    }
  }
}

HudElemArrayInfo TextHook_GetHudElemArrayInfo() {
  HudElemArrayInfo info{};
  std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
  if (!g_hudElemArray.valid && !g_hudElemArray.tentative) return info;
  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  info.baseAddr = g_hudElemArray.baseAddr;
  info.moduleOffset = g_hudElemArray.baseAddr - s_moduleBase;
  info.stride = g_hudElemArray.stride;
  info.count = g_hudElemArray.count;
  info.valid = g_hudElemArray.valid;
  return info;
}

// =============================================================================
// Targeted suppression for countdown timer HudElems.
// Called from D3D11 Present when a TimeScript Korean overlay is active.
// Zeroes the color alpha (+0x30) to make the entire element invisible (label +
// timer number).  Korean label is rendered by our D3D11 overlay.  Timer number
// is also rendered by our overlay once SP game time is known.
//
// Also probes candidate addresses to discover SP game time so we can compute
// the countdown timer value for Korean rendering.
// =============================================================================

// SP game time discovery state.
static struct {
  uintptr_t addr = 0;       // Confirmed game time address (0 = not yet found)
  bool      confirmed = false;
} g_spGameTime;

// Wide-scan game time discovery: two-pass delta-matching scan of all
// writable pages in the module image.  Pass 1 collects candidate int32
// addresses whose value, when subtracted from the HudElem time field,
// yields a plausible countdown remainder.  Pass 2 (>=2 s later) re-reads
// each candidate and confirms by checking that the value advanced at
// approximately 1x wall-clock rate (real game time should track real time).
struct WideScanCand { uintptr_t addr; int val; };
static struct {
  bool pass1Done    = false;
  bool pass2Done    = false;
  DWORD pass1Tick   = 0;
  int timeFieldAt1  = 0;
  WideScanCand cands[100000];
  int candCount     = 0;
} g_wideScan;

// Pass 1 helper: scans writable pages in [scanStart, scanEnd) for
// plausible game time values.  In its own function so __try/__except
// doesn't conflict with C++ dtors in the caller.
static int WideScan_CollectCandidates(
    uintptr_t scanStart, uintptr_t scanEnd, int hudTime,
    WideScanCand *out, int maxOut,
    uintptr_t exclStart = 0, uintptr_t exclEnd = 0) {
  int n = 0;
  uintptr_t cur = scanStart;
  __try {
    while (cur < scanEnd && n < maxOut) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery((void *)cur, &mbi, sizeof(mbi)) == 0) {
        cur += 0x10000;
        continue;
      }
      uintptr_t regEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
      if (regEnd > scanEnd) regEnd = scanEnd;
      const DWORD p = mbi.Protect & 0xFF;
      bool w = (mbi.State == MEM_COMMIT) &&
               !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
               (p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                p == PAGE_EXECUTE_READWRITE ||
                p == PAGE_EXECUTE_WRITECOPY);
      if (!w) { cur = regEnd; continue; }
      uintptr_t aStart = (uintptr_t)mbi.BaseAddress;
      if (aStart < scanStart) aStart = scanStart;
      for (uintptr_t a = aStart; a + 4 <= regEnd && n < maxOut; a += 4) {
        // Skip exclusion zone (e.g. entity array, HudElem array).
        if (exclStart != 0 && a >= exclStart && a < exclEnd) {
          a = exclEnd - 4;  // loop increment brings to exclEnd
          continue;
        }
        int v = *(volatile int *)a;
        int rem = hudTime - v;
        if (v > 30000 && rem >= 1000 && rem <= 150000) {
          out[n].addr = a;
          out[n].val  = v;
          ++n;
        }
      }
      cur = regEnd;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { /* stop on fault */ }
  return n;
}

// Read SP game time.  Returns 0 if not yet discovered.
static int ReadSPGameTime() {
  // Stock August SP: 0x231081 reads this clock into renderCtx+0x238;
  // CG_DrawHudElem passes it to 0x231E80, which subtracts it from elem+0x78.
  // Prefer that exact native countdown clock over plausible-value guesses.
  static const uintptr_t moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  int nativeTime = 0;
  if (GameBuild::RuntimeReady() &&
      ObjRev_ReadI32(moduleBase + IW6Offsets::ClientGameTime_SP, nativeTime) &&
      nativeTime > 0 && nativeTime < 100000000) {
    return nativeTime;
  }
  if (!g_spGameTime.confirmed || g_spGameTime.addr == 0) {
    return 0;
  }
  int val = 0;
  if (ObjRev_ReadI32(g_spGameTime.addr, val) && val > 0 && val < 10000000) {
    return val;
  }
  return 0;
}

void TextHook_SuppressActiveCountdownLabel() {
  // English suppression: CLOCKWORK_POWERDOWN label uses SEH " " return;
  // all timescripts use CG_DrawHudElem x=9999 persist suppress.
  // This function reads the HudElem time field (+0x78) and probes for SP
  // game time.

  TimeScriptRenderSnapshot snap;
  if (!TextHook_GetTimeScriptRenderSnapshot(snap)) {
    return;
  }

  uint32_t targetLabel = 0;
  if (snap.key == "CLOCKWORK_POWERDOWN") {
    targetLabel = 0x23;
  } else if (snap.key == "CLOCKWORK_EXFIL") {
    targetLabel = 0x23;
  } else if (snap.key == "SATFARM_TIME_IMACT") {
    targetLabel = 0x3C;
  }
  if (targetLabel == 0) {
    return;
  }
  const bool allowSatfarmFallback3B = (snap.key == "SATFARM_TIME_IMACT");

  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  const uintptr_t base = s_moduleBase + IW6Offsets::HudElem_Array_SP;
  const int stride = IW6Offsets::HudElem_Array_Stride_SP;
  const int maxElems = 2048;  // server (0-1023) + client (1024-2047) HudElems

  static uintptr_t s_trackedAddr = 0;
  static DWORD     s_trackedTick = 0;

  // Fast path: re-check tracked element.
  bool found = false;
  if (s_trackedAddr != 0 && (GetTickCount() - s_trackedTick) < 2000) {
    uint32_t currentLabel = 0;
    if (ObjRev_ReadU32(s_trackedAddr + 0x40, currentLabel) &&
        (currentLabel == targetLabel ||
         (allowSatfarmFallback3B && currentLabel == 0x3B))) {
      int timeField = 0;
      if (ObjRev_ReadI32(s_trackedAddr + 0x78, timeField) && timeField > 0) {
        g_countdownTimeField = timeField;
      }
      s_trackedTick = GetTickCount();
      found = true;
    }
  }

  if (!found) {
    // Full scan: find element with matching label at +0x40.
    if (!ObjRev_IsReadable(base, (uintptr_t)stride)) {
      return;
    }
    for (int i = 0; i < maxElems; ++i) {
      uintptr_t elem = base + (uintptr_t)i * (uintptr_t)stride;
      uint32_t label = 0;
      if (!ObjRev_ReadU32(elem + 0x40, label)) {
        continue;
      }
      if (label == targetLabel || (allowSatfarmFallback3B && label == 0x3B)) {
        s_trackedAddr = elem;
        s_trackedTick = GetTickCount();
        int timeField = 0;
        if (ObjRev_ReadI32(elem + 0x78, timeField) && timeField > 0) {
          g_countdownTimeField = timeField;
        }
        found = true;
        break;
      }
    }
  }

  if (!found) {
    return;
  }
  // --- SP Game Time Discovery ---
  // Probe candidate addresses to find the current game time.
  // The HudElem time field is the absolute end time (ms).  The game time
  // should be close to (timeField - displayed_remaining_seconds * 1000).
  // We probe addresses and check if (timeField - candidate) is in a
  // plausible countdown range (0..300 seconds).
  if (ReadSPGameTime() > 0) {
    return;  // Already found
  }
  if (g_countdownTimeField <= 0) {
    return;
  }

  // Candidate offsets to probe (relative to module base).
  // Include MP offsets (might work in SP), nearby SP globals, and common
  // engine time storage patterns.
  static const uintptr_t kCandidateOffsets[] = {
      IW6Offsets::MPGlobals::gameTime,      // 0x43F4B6C
      IW6Offsets::MPGlobals::serverTime,    // 0x647B280
      // playerState_s.commandTime at start of cg_s (MP cgArray)
      IW6Offsets::MPGlobals::cgArray,       // 0x176EC00
      // Some common SP offsets to try (near known SP structures)
      IW6Offsets::Profile::Rva_176EC00 + 0x3388 + 0x3C7C,         // cg->snap->serverTime (if MP struct)
      // Try offsets near HudElem array
      IW6Offsets::Profile::Rva_147ED00,  IW6Offsets::Profile::Rva_147ED04,  IW6Offsets::Profile::Rva_147ED08,
      // Try some known IW6 SP patterns
      IW6Offsets::Profile::Rva_1478000,  IW6Offsets::Profile::Rva_1478004,  IW6Offsets::Profile::Rva_1478008,
      IW6Offsets::Profile::Rva_15B0000,  IW6Offsets::Profile::Rva_15B0004,  IW6Offsets::Profile::Rva_15B0008,
      // Game time is often near level globals
      IW6Offsets::Profile::Rva_3C91500,  IW6Offsets::Profile::Rva_3C91504,  IW6Offsets::Profile::Rva_3C91508,
      IW6Offsets::Profile::Rva_3C914F8,  IW6Offsets::Profile::Rva_3C914FC,
      // cg_t area for SP (might be near MP cgArray but shifted)
      IW6Offsets::Profile::Rva_1550000,  IW6Offsets::Profile::Rva_1550004,  IW6Offsets::Profile::Rva_1560000,
      // Additional common engine time locations
      IW6Offsets::Profile::Rva_43F4B60,  IW6Offsets::Profile::Rva_43F4B64,  IW6Offsets::Profile::Rva_43F4B68,  IW6Offsets::Profile::Rva_43F4B70,
  };

  static DWORD s_lastProbeLog = 0;
  const DWORD now = GetTickCount();
  if ((now - s_lastProbeLog) < 2000) {
    return;  // Don't probe too frequently
  }
  s_lastProbeLog = now;

  char probeBuf[1024];
  int pos = sprintf_s(probeBuf, "[GAMETIME-PROBE] hudTime=%d candidates:",
                      g_countdownTimeField);

  for (size_t ci = 0; ci < sizeof(kCandidateOffsets) / sizeof(kCandidateOffsets[0]); ++ci) {
    uintptr_t addr = s_moduleBase + kCandidateOffsets[ci];
    int val = 0;
    if (ObjRev_ReadI32(addr, val) && val > 1000 && val < 10000000) {
      int remaining = g_countdownTimeField - val;
      // Plausible countdown range: 0 to 300 seconds (0-300000 ms)
      if (remaining >= 0 && remaining <= 300000) {
        pos += sprintf_s(probeBuf + pos, sizeof(probeBuf) - pos,
                         " +0x%llX=%d(rem=%.1fs)",
                         (unsigned long long)kCandidateOffsets[ci],
                         val, remaining / 1000.0f);
        // If remaining is plausible for a countdown (1s..120s), mark as confirmed
        if (remaining >= 1000 && remaining <= 120000) {
          g_spGameTime.addr = addr;
          g_spGameTime.confirmed = true;
          pos += sprintf_s(probeBuf + pos, sizeof(probeBuf) - pos, " **CONFIRMED**");
        }
      }
    }
  }
  LogToFile(probeBuf);

  // --- Phase 2: Module-wide scan (two-pass delta matching) ---
  // Fixed-address probe above failed — scan all writable pages in the
  // module image.  Pass 1 collects candidate int32 addresses; pass 2
  // (>=2 s later) confirms by checking that the value advanced at ~1x
  // wall-clock rate (true game time tracks real time in normal gameplay).
  if (!g_spGameTime.confirmed) {
    // Reset if a different countdown timeField appears.
    if (g_countdownTimeField != g_wideScan.timeFieldAt1 &&
        g_wideScan.timeFieldAt1 != 0) {
      g_wideScan.pass1Done = false;
      g_wideScan.pass2Done = false;
      g_wideScan.candCount = 0;
    }

    if (g_wideScan.pass2Done) {
      // Already completed for this countdown.
    } else if (!g_wideScan.pass1Done) {
      // Scan writable pages for game time candidates, excluding known
      // dynamic arrays (entities, HudElems) that contain per-object
      // timers and produce false positives.
      //
      // Exclusion zone: entity array at g_entities (0x3C91600), ~2 MB.
      const uintptr_t entExclStart = s_moduleBase + IW6Offsets::SPGlobals::g_entities;
      const uintptr_t entExclEnd   = entExclStart + 0x200000;  // ~2 MB
      int n = 0;
      // Region 1: BEFORE entity array (level_locals_t lives here)
      n = WideScan_CollectCandidates(
          s_moduleBase + IW6Offsets::Profile::Rva_3C80000, entExclStart,
          g_countdownTimeField, g_wideScan.cands, 100000);
      // Region 2: AFTER entity array
      if (n < 100000) {
        n += WideScan_CollectCandidates(
            entExclEnd, s_moduleBase + IW6Offsets::Profile::Rva_3F00000,
            g_countdownTimeField, g_wideScan.cands + n, 100000 - n);
      }
      // Region 3: near HudElem_Array_SP, excluding HudElem array itself
      if (n < 100000) {
        const uintptr_t hudExclStart = s_moduleBase + IW6Offsets::HudElem_Array_SP;
        const uintptr_t hudExclEnd   = hudExclStart + 2048 * IW6Offsets::HudElem_Array_Stride_SP;
        n += WideScan_CollectCandidates(
            s_moduleBase + IW6Offsets::Profile::Rva_1400000, s_moduleBase + IW6Offsets::Profile::Rva_1580000,
            g_countdownTimeField, g_wideScan.cands + n, 100000 - n,
            hudExclStart, hudExclEnd);
      }
      // Region 4: full module (excluding entity array)
      if (n < 100000) {
        n += WideScan_CollectCandidates(
            s_moduleBase, s_moduleBase + IW6Offsets::Profile::SearchImageSpan,
            g_countdownTimeField, g_wideScan.cands + n, 100000 - n,
            entExclStart, entExclEnd);
      }
      g_wideScan.candCount = n;
      g_wideScan.pass1Done    = true;
      g_wideScan.pass1Tick    = now;
      g_wideScan.timeFieldAt1 = g_countdownTimeField;
      char wbuf[160];
      sprintf_s(wbuf,
          "[GAMETIME-WIDESCAN] pass1: %d candidates (hudTime=%d)",
          g_wideScan.candCount, g_countdownTimeField);
      LogToFile(wbuf);
    } else if ((now - g_wideScan.pass1Tick) >= 2000) {
      // Pass 2: re-read candidates, confirm by delta.
      const DWORD wallDelta = now - g_wideScan.pass1Tick;
      int bestIdx = -1, bestDelta = 0;
      for (int i = 0; i < g_wideScan.candCount; ++i) {
        int nv = 0;
        if (!ObjRev_ReadI32(g_wideScan.cands[i].addr, nv)) continue;
        int delta = nv - g_wideScan.cands[i].val;
        int rem   = g_countdownTimeField - nv;
        if (delta > (int)(wallDelta / 3) &&
            delta < (int)(wallDelta * 2) &&
            rem >= 0 && rem <= 200000) {
          if (bestIdx < 0 ||
              abs(delta - (int)wallDelta) <
                  abs(bestDelta - (int)wallDelta)) {
            bestIdx   = i;
            bestDelta = delta;
          }
        }
      }
      if (bestIdx >= 0) {
        g_spGameTime.addr      = g_wideScan.cands[bestIdx].addr;
        g_spGameTime.confirmed = true;
        int nv = 0;
        ObjRev_ReadI32(g_spGameTime.addr, nv);
        char wbuf[256];
        sprintf_s(wbuf,
            "[GAMETIME-WIDESCAN] **CONFIRMED** offset=+0x%llX val=%d "
            "delta=%d wall=%u rem=%.1fs",
            (unsigned long long)(g_spGameTime.addr - s_moduleBase),
            nv, bestDelta, wallDelta,
            (g_countdownTimeField - nv) / 1000.0f);
        LogToFile(wbuf);
      } else {
        char wbuf[128];
        sprintf_s(wbuf,
            "[GAMETIME-WIDESCAN] pass2: no match (%d cands, wall=%u)",
            g_wideScan.candCount, wallDelta);
        LogToFile(wbuf);
      }
      g_wideScan.pass2Done = true;
      g_wideScan.candCount = 0;
    }
  }
}

int TextHook_ReadSPGameTime() {
  return ReadSPGameTime();
}

int TextHook_GetCountdownTimeField() {
  return g_countdownTimeField;
}

void TextHook_ResetGameTimeDiscovery() {
  g_spGameTime.addr = 0;
  g_spGameTime.confirmed = false;
  g_wideScan.pass1Done = false;
  g_wideScan.pass2Done = false;
  g_wideScan.candCount = 0;
  g_wideScan.timeFieldAt1 = 0;
  LogToFile("[GAMETIME-RESET] game time address invalidated, will re-scan");
}

// =============================================================================
// Phase 2B: Per-frame HudElem reading for objectives
// Called from D3D11Hook Present every frame (not every 2s like scan)
// =============================================================================
void TextHook_ReadLiveHudElems() {

  // Credits guard: skip the entire 2048-element HudElem scan during credits.
  // This is the main per-frame CPU cost — each iteration does multiple memory
  // probes, key resolution, and objective/countdown classification.
  {
    extern std::atomic<bool> g_creditsGuardActive;
    if (g_creditsGuardActive.load(std::memory_order_relaxed)) {
      return;
    }
  }

  static uintptr_t s_moduleBase = (uintptr_t)GetModuleHandleA(NULL);
  const uintptr_t fixedBase = s_moduleBase + IW6Offsets::HudElem_Array_SP;
  const int fixedStride = IW6Offsets::HudElem_Array_Stride_SP;
  uintptr_t readBase = fixedBase;
  int readStride = fixedStride;
  int readCount = 2048;  // server (0-1023) + client (1024-2047) HudElems
  bool fallbackUsed = false;

  if (!ObjRev_IsReadable(readBase, 0xA8)) {
    std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
    if ((g_hudElemArray.valid || g_hudElemArray.tentative) &&
        g_hudElemArray.baseAddr != 0 && g_hudElemArray.stride > 0 &&
        ObjRev_IsReadable(g_hudElemArray.baseAddr, 0xA8)) {
      readBase = g_hudElemArray.baseAddr;
      readStride = g_hudElemArray.stride;
      if (g_hudElemArray.count > 0) {
        readCount = g_hudElemArray.count;
      }
      fallbackUsed = true;
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_hudElemArrayMutex);
    g_hudElemArray.baseAddr = readBase;
    g_hudElemArray.stride = readStride;
    g_hudElemArray.count = readCount;
    g_hudElemArray.tentative = true;
    g_hudElemArray.valid = true;
  }

  if (!ObjRev_IsReadable(readBase, 0xA8)) {
    if (!g_TimeScriptHooked) {
      TimeScript_PollFallbackDiagnostics();
    }
    {
      std::lock_guard<std::mutex> lock(g_liveHudScriptCountdownMutex);
      g_liveHudScriptCountdowns.clear();
    }
    return;
  }
  if (fallbackUsed) {
    static DWORD s_lastFallbackLog = 0;
    DWORD now = GetTickCount();
    if ((now - s_lastFallbackLog) > 2000) {
      s_lastFallbackLog = now;
      char fbuf[192];
      sprintf_s(fbuf,
                "[OBJ-REVERSE] fixed HudElem base unreadable -> fallback base=0x%llX stride=0x%X count=%d",
                (unsigned long long)readBase, readStride, readCount);
      LogToFile(fbuf);
    }
  }

  int globalServerTime = 0;
  {
    const uintptr_t stAddr = s_moduleBase + IW6Offsets::MPGlobals::serverTime;
    const uintptr_t gtAddr = s_moduleBase + IW6Offsets::MPGlobals::gameTime;
    int candidate = 0;
    if (ObjRev_IsReadable(stAddr, sizeof(int)) &&
        ObjRev_ReadI32(stAddr, candidate) && candidate > 0 &&
        candidate < 10000000) {
      globalServerTime = candidate;
    } else if (ObjRev_IsReadable(gtAddr, sizeof(int)) &&
               ObjRev_ReadI32(gtAddr, candidate) && candidate > 0 &&
               candidate < 10000000) {
      globalServerTime = candidate;
    }
  }

  int kMaxElems = readCount;
  if (kMaxElems <= 0 || kMaxElems > 2048) {
    kMaxElems = 1024;
  }
  if (kMaxElems > 1) {
    const uintptr_t tailElem =
        readBase + (uintptr_t)(kMaxElems - 1) * (uintptr_t)readStride;
    if (!ObjRev_IsReadable(tailElem, sizeof(uint32_t))) {
      // One-shot range sanity check; detailed boundary handling remains fail-open.
      kMaxElems = (std::max)(1, kMaxElems / 2);
    }
  }
  std::vector<LiveHudElem> newElems;
  std::vector<LiveHudScriptCountdown> liveCountdowns;
  std::vector<NativeObjectiveItem> nativeItems;
  std::unordered_map<std::string, LiveHudScriptCountdown> bestCountdownByKey;
  std::unordered_set<uintptr_t> activeCountdownPtrs;
  std::unordered_set<uint32_t> activeObjectiveCfgSet;
  bool hasObjectiveLaneActivity = false;
  activeObjectiveCfgSet.reserve(64);
  newElems.reserve(8);
  liveCountdowns.reserve(4);
  nativeItems.reserve(8);
  bestCountdownByKey.reserve(4);
  activeCountdownPtrs.reserve(4);
  struct CountdownSuppressionState {
    uint8_t colorAlpha = 0;
    uint8_t fromColorAlpha = 0;
    bool captured = false;
    unsigned long lastSeenTick = 0;
  };
  static std::unordered_map<uintptr_t, CountdownSuppressionState>
      s_countdownSuppressionByPtr;
  constexpr bool kRequireAuthoritativeBind = true;
  constexpr bool kEnableRuntimeObjectiveAuthorityFallbacks = false;
  static_assert(!kEnableRuntimeObjectiveAuthorityFallbacks,
                "Objective authority fallback paths must remain disabled.");

  static std::unordered_map<uint32_t, std::string> s_cfgToLastKey;
  static std::unordered_map<uint32_t, unsigned long> s_cfgNoKeyLogTick;
  std::string recentStatusProbeKey;
  uint32_t recentStatusProbeSlc = 0;
  bool recentStatusProbeHasParams = false;
  unsigned int recentStatusProbeCaller = 0;
  bool hasRecentStatusProbe = TextHook_GetRecentObjectiveStatusProbeFromConsumers(
      350, recentStatusProbeKey, &recentStatusProbeSlc,
      &recentStatusProbeHasParams, &recentStatusProbeCaller);
  if (!hasRecentStatusProbe) {
    hasRecentStatusProbe = TextHook_GetRecentObjectiveStatusProbe(
        350, 0, recentStatusProbeKey, &recentStatusProbeSlc,
        &recentStatusProbeHasParams);
  }
  // Strict window used for authority guard decisions.
  // We keep this tighter than telemetry window to avoid stale probe bleed.
  std::string recentStatusProbeKeyStrict;
  uint32_t recentStatusProbeSlcStrict = 0;
  bool recentStatusProbeHasParamsStrict = false;
  const bool hasRecentStatusProbeStrict =
      TextHook_GetRecentObjectiveStatusProbeFromConsumers(
          140, recentStatusProbeKeyStrict, &recentStatusProbeSlcStrict,
          &recentStatusProbeHasParamsStrict, nullptr);
  const bool frameStatusProbeStrictActive =
      (hasRecentStatusProbeStrict && recentStatusProbeHasParamsStrict &&
       IsObjectiveStatusMessageKeyName(recentStatusProbeKeyStrict));
  if (hasRecentStatusProbe &&
      IsObjectiveStatusMessageKeyName(recentStatusProbeKey)) {
    // Reverse-only telemetry:
    // Do NOT inject status events from probe directly.
    // Status render authority must come from consumer lane edge capture.
    static std::unordered_map<std::string, unsigned long> s_statusProbeSeenLogTick;
    const unsigned long nowProbeSeen = GetTickCount();
    std::string probeSig = recentStatusProbeKey + "|" +
                           std::to_string((unsigned int)recentStatusProbeSlc);
    auto itProbeSeen = s_statusProbeSeenLogTick.find(probeSig);
    if (itProbeSeen == s_statusProbeSeenLogTick.end() ||
        (nowProbeSeen - itProbeSeen->second) > 500) {
      s_statusProbeSeenLogTick[probeSig] = nowProbeSeen;
      char pbuf[320];
      sprintf_s(pbuf,
                "[STATUS-PROBE-SEEN] key=%s caller=+0x%X slc=0x%X rp=%d",
                recentStatusProbeKey.c_str(), recentStatusProbeCaller,
                (unsigned int)recentStatusProbeSlc,
                recentStatusProbeHasParams ? 1 : 0);
      LogToFile(pbuf);
    }
  }
  std::string recentStatusCompletedKey;
  uint32_t recentStatusCompletedSlc = 0;
  bool recentStatusCompletedHasParams = false;
  unsigned int recentStatusCompletedCaller = 0;
  const bool hasRecentStatusCompletedProbe =
      TextHook_GetRecentObjectiveStatusProbeByKeyFromConsumers(
          700, "GAME_OBJECTIVECOMPLETED", recentStatusCompletedKey,
          &recentStatusCompletedSlc, &recentStatusCompletedHasParams,
          &recentStatusCompletedCaller);
  if (hasRecentStatusCompletedProbe &&
      IsObjectiveStatusMessageKeyName(recentStatusCompletedKey)) {
    static unsigned long s_statusCompletedProbeHitLogTick = 0;
    const unsigned long nowComp = GetTickCount();
    if ((nowComp - s_statusCompletedProbeHitLogTick) > 500) {
      s_statusCompletedProbeHitLogTick = nowComp;
      char cbuf[320];
      sprintf_s(cbuf,
                "[STATUS-COMPLETE-PROBE-HIT] key=%s caller=+0x%X slc=0x%X rp=%d",
                recentStatusCompletedKey.c_str(),
                recentStatusCompletedCaller, recentStatusCompletedSlc,
                recentStatusCompletedHasParams ? 1 : 0);
      LogToFile(cbuf);
    }
  }

  for (int i = 0; i < kMaxElems; ++i) {
    uintptr_t elem = readBase + (uintptr_t)i * (uintptr_t)readStride;

    uint32_t t = 0;
    if (!ObjRev_ReadU32(elem, t)) {
      // Fail-open: stop at first unreadable lane boundary.
      break;
    }
    if (t == 0) {
      continue;
    }
    if (t > 20) {
      // IW6 HudElem array is fixed-size (2048 slots).  A type > 20 is not
      // "past the array end" — it is an unexpected element type (material,
      // waypoint, or engine-internal).  Skip it instead of aborting the
      // entire scan, which was causing the objective system to go blind
      // whenever such an element appeared at an index below the objectives.
      static DWORD s_lastBadTypeLog = 0;
      DWORD nowBt = GetTickCount();
      if ((nowBt - s_lastBadTypeLog) > 3000) {
        s_lastBadTypeLog = nowBt;
        char btbuf[192];
        sprintf_s(btbuf,
                  "[OBJ-SCAN] skipping elem[%d] type=%u (>20) at ptr=0x%llX",
                  i, t, (unsigned long long)elem);
        LogToFile(btbuf);
      }
      continue;
    }
    const bool isTextElem = (t == 1);
    // When type=0 and we're actively suppressing this element (zeroed type),
    // treat it as a timer element so the scanner still processes it.
    const bool isSuppressedTimer =
        (t == 0 && g_TimeScriptPersistSuppress.active &&
         g_TimeScriptPersistSuppress.elemPtr == elem &&
         g_TimeScriptPersistSuppress.originalType != 0);
    // SATFARM: after map transition, the timer type changes from
    // TIMER_DOWN(5) to TEXT(1) or TIMER_UP(4).  Detect by label
    // (0x3B/0x3C are high configstring indices unique to SATFARM).
    bool isSatfarmLabelTimer = false;
    if (!IsHudElemTimerType(t) && !isSuppressedTimer) {
      uint32_t earlyLabel = 0;
      if (ObjRev_ReadU32(elem + 0x40, earlyLabel)) {
        isSatfarmLabelTimer = (earlyLabel == 0x3B || earlyLabel == 0x3C);
      }
    }
    const bool isTimerElem =
        IsHudElemTimerType(t) || isSuppressedTimer || isSatfarmLabelTimer;
    if (!isTextElem && !isTimerElem) {
      continue;
    }

    float ex = 0.0f, ey = 0.0f, efs = 0.0f;
    int eFlags = 0;
    if (!ObjRev_ReadF32(elem + 0x04, ex) || !ObjRev_ReadF32(elem + 0x08, ey) ||
        !ObjRev_ReadF32(elem + 0x14, efs) || !ObjRev_ReadI32(elem + 0xA4, eFlags)) {
      continue;
    }

    // If this element is suppressed (x=9999 in memory), prefer the fresh
    // CG_DrawHudElem capture for this elemPtr. Only fall back to the
    // suppress-map x cache when memory is still offscreen and no fresh
    // draw-time capture exists.
    // x=9999 enforcement is handled by the CG_DrawHudElem hook.
    {
      std::lock_guard<std::mutex> supLk(g_objSuppressMutex);
      auto itSup = g_objSuppressedElems.find(elem);
      if (itSup != g_objSuppressedElems.end()) {
        const bool memoryLooksSuppressed =
            (!std::isfinite(ex) || ex >= 9000.0f || ex <= -9000.0f);
        if (memoryLooksSuppressed) {
          CgDrawHudElemCapture cap{};
          if (TextHook_GetCgDrawElemPtrCapture(elem, cap, 250) &&
              std::isfinite(cap.x) && std::isfinite(cap.y) &&
              std::isfinite(cap.fontScale) && cap.x < 9000.0f &&
              cap.x > -9000.0f) {
            ex = cap.x;
            ey = cap.y;
            efs = cap.fontScale;
          } else {
            float origX = 0.0f;
            memcpy(&origX, &itSup->second.currentXBits, sizeof(float));
            if (std::isfinite(origX) && origX < 9000.0f) {
              ex = origX;
            }
          }
        }
      }
    }

    const bool isHintLane = (ey <= -60.0f && efs >= 1.8f);
    // Objective update/save notification lane is vertically separated from
    // list lanes (often around y=-68 with larger scale and flags=0x7).
    // Capture it explicitly so authoritative key join can proceed there too.
    const bool isObjectiveNotificationLane =
        (ey <= -40.0f && ey >= -95.0f && efs >= 1.65f && efs <= 2.35f &&
         (eFlags == 0x7 || eFlags == 0x5));
    // Negative-Y with small font is usually intro/cinematic text, not gameplay
    // objective lanes. Keep notification lane exception above.
    const bool looksIntroLikeNegativeSmallLane =
        (ey <= -20.0f && efs < 1.65f);
    const bool looksCenterPromptLane =
        (ex >= 1.0f && ex <= 5.5f && ey >= 82.0f && ey <= 125.0f &&
         efs >= 1.10f && efs <= 1.40f);
    int objLaneScore = 0;
    if (ex >= -15.0f && ex <= 20.0f) {
      objLaneScore += 46;
    } else if (ex >= -30.0f && ex <= 35.0f) {
      objLaneScore += 18;
    }
    if (ey >= -10.0f && ey <= 105.0f) {
      objLaneScore += 40;
    } else if (ey >= -24.0f && ey <= 130.0f) {
      objLaneScore += 14;
    }
    if (efs >= 1.10f && efs <= 1.42f) {
      objLaneScore += 34;
    } else if (efs >= 0.95f && efs <= 1.62f) {
      objLaneScore += 12;
    }
    const bool hasObjectiveLaneFlags = (eFlags == 0x5 || eFlags == 0x7);
    if (hasObjectiveLaneFlags) {
      objLaneScore += 34;
    } else if (eFlags != 0) {
      objLaneScore += 10;
    }
    if (looksCenterPromptLane) {
      objLaneScore -= 74;
    }
    if (isHintLane) {
      objLaneScore -= 96;
    }
    if (efs < 0.75f || !std::isfinite(efs)) {
      objLaneScore -= 84;
    }
    // Penalize large fontScale — objectives use fs≈1.25; prompt-sized
    // HudElems (fs>1.62, e.g. FLOOD_LAUNCHER_MELEE fs=2.0) that happen
    // to share objective-like x/y/flags should not pass the OBJ gate.
    if (efs > 1.62f) {
      objLaneScore -= 60;
    }
    if (ey < -130.0f || ey > 150.0f) {
      objLaneScore -= 72;
    }
    // List lanes in this build are left-anchored around x≈6. Non-left lanes
    // (e.g., x≈64 prompts) can accidentally satisfy score-only checks.
    const bool hasObjectiveListX = (ex >= -15.0f && ex <= 20.0f);
    const bool isObjLane =
        (((objLaneScore >= 76) && hasObjectiveListX && hasObjectiveLaneFlags &&
          !looksIntroLikeNegativeSmallLane && !looksCenterPromptLane)) ||
        isObjectiveNotificationLane;
    const bool isHintBridgeLane = isHintLane || looksCenterPromptLane;
    const bool keepForLiveBridge = isObjLane || isHintBridgeLane;

    {
      uintptr_t textPtr = 0;
      float timerValue = 0.0f;
      uint32_t horzAlign = 0;
      uint32_t vertAlign = 0;
      uint32_t countdownColor = 0;
      uint32_t countdownFromColor = 0;
      uint32_t countdownLabelSlc = 0;
      uint32_t countdownTextSlc = 0;
      // hudelem_s struct offsets (authoritative, from iw6-mod-main structs.hpp):
      //   +0x28 = alignOrg, +0x2C = alignScreen
      //   +0x40 = label (configstring/SLC), +0x84 = text (SLC)
      //   +0x78 = time (int ms), +0x7C = duration (int ms), +0x80 = value (float)
      const bool haveTextPtr = false;
      const bool haveAlign =
          ObjRev_ReadU32(elem + 0x28, horzAlign) &&
          ObjRev_ReadU32(elem + 0x2C, vertAlign);
      const bool haveColor =
          ObjRev_ReadU32(elem + 0x30, countdownColor);
      const bool haveFromColor =
          ObjRev_ReadU32(elem + 0x34, countdownFromColor);
      ObjRev_ReadU32(elem + 0x40, countdownLabelSlc);
      ObjRev_ReadU32(elem + 0x84, countdownTextSlc);
      ObjRev_ReadF32(elem + 0x80, timerValue);

      std::string rawCountdownText;
      if (haveTextPtr && textPtr > 0x10000 && textPtr < 0x7FFFFFFFFFFF) {
        TryReadCStringPreview((const char *)textPtr, 160, rawCountdownText);
      }

      // --- Direct label-SLC detection for TimeScript-managed countdown timers ---
      // For timer-type elements, resolve label SLC directly (doesn't require
      // timerValue > 0.05 which fails when value field is unused by engine).
      //
      // Strategy for English suppression (2026-03-14 refactor):
      //   Do NOT suppress alpha here.  Let the engine render normally so that
      //   R_AddCmdDrawText is called with the label ("Powerdown in ") and
      //   timer number ("1:02.6") as two separate draw calls.  The split
      //   capture in Detour_R_AddCmdDrawText intercepts both, suppresses the
      //   English draws, and queues a combined Korean overlay via
      //   QueueHudScriptCountdownOverlay.  This eliminates flicker (no
      //   HudElem memory writes racing with the engine) and provides the
      //   exact timer value (from the engine's own rendering, not from
      //   unreliable game-time memory scanning).

      if (isTimerElem && haveAlign && haveColor) {
        std::string directTimerKey;
        bool detectedByLabel =
            (TryResolveHudScriptCountdownKeyFromCfg(countdownLabelSlc,
                                                     directTimerKey) &&
             IsTimeScriptManagedCountdownKeyName(directTimerKey));

        if (detectedByLabel) {
          // No alpha suppression — engine renders normally, R_AddCmdDrawText
          // hook handles English suppression + Korean overlay.
          // Still store the snapshot for diagnostic/fallback purposes.
          int timeField = 0;
          int durationField = 0;
          ObjRev_ReadI32(elem + 0x78, timeField);
          ObjRev_ReadI32(elem + 0x7C, durationField);
          TimeScript_StoreSnapshotFromHudElemTimer(
              directTimerKey, ex, ey, efs, horzAlign, vertAlign,
              countdownColor, timeField, durationField,
              timerValue, countdownLabelSlc, countdownTextSlc, elem);
          continue;
        }
      }

      if (haveAlign && haveColor) {
        HudScriptCountdownMatch countdownMatch{};
        if (TryResolveHudScriptCountdownFromHudElem(
                t, countdownLabelSlc, countdownTextSlc, textPtr, timerValue,
                rawCountdownText, countdownMatch)) {
          if (IsTimeScriptManagedCountdownKeyName(countdownMatch.key)) {
            continue;
          }

          LiveHudScriptCountdown countdown{};
          countdown.key = countdownMatch.key;
          countdown.rawText = rawCountdownText;
          countdown.englishText = countdownMatch.englishPrefix;
          if (!countdown.englishText.empty() &&
              !std::isspace((unsigned char)countdown.englishText.back())) {
            countdown.englishText.push_back(' ');
          }
          countdown.englishText += countdownMatch.tail;
          countdown.renderText = countdownMatch.renderText;
          countdown.rawPtr = elem;
          countdown.textPtr = textPtr;
          countdown.x = ex;
          countdown.y = ey;
          countdown.fontScale = efs;
          countdown.timerValue = timerValue;
          countdown.horzAlign = horzAlign;
          countdown.vertAlign = vertAlign;
          countdown.colorPacked = countdownColor;
          countdown.alpha =
              (float)((countdownColor >> 24) & 0xFFu) / 255.0f;
          countdown.active = true;

          auto CountdownScore = [](const LiveHudScriptCountdown &cand) {
            float score = cand.alpha * 100.0f;
            if (cand.horzAlign == 2) {
              score += 40.0f;
            }
            if (cand.vertAlign == 0) {
              score += 10.0f;
            }
            if (cand.x >= 20.0f) {
              score += 8.0f;
            }
            if (cand.x <= -100.0f && cand.horzAlign == 2) {
              score += 16.0f;
            }
            if (cand.y >= -90.0f && cand.y <= 120.0f) {
              score += 8.0f;
            }
            if (cand.timerValue > 0.05f) {
              score += 12.0f;
            }
            return score;
          };

          auto itBest = bestCountdownByKey.find(countdown.key);
          if (itBest == bestCountdownByKey.end() ||
              CountdownScore(countdown) > CountdownScore(itBest->second)) {
            bestCountdownByKey[countdown.key] = countdown;
          }

          activeCountdownPtrs.insert(elem);
          // No alpha suppression — R_AddCmdDrawText hook handles English
          // suppression + Korean overlay at draw-call level (no flicker).

          static std::unordered_map<std::string, unsigned long>
              s_liveCountdownLogTick;
          const unsigned long nowCountdown = GetTickCount();
          const std::string logSig =
              countdown.key + "|" +
              std::to_string((unsigned long long)elem);
          auto itLog = s_liveCountdownLogTick.find(logSig);
          if (itLog == s_liveCountdownLogTick.end() ||
              (nowCountdown - itLog->second) > 800) {
            s_liveCountdownLogTick[logSig] = nowCountdown;
            char cbuf[640];
            sprintf_s(
                cbuf,
                "[HUD-TIMER-LIVE] type=%u key=%s ptr=0x%llX textPtr=0x%llX label=0x%X text=0x%X x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f a=%.2f raw=\"%.96s\"",
                t, countdown.key.c_str(),
                (unsigned long long)countdown.rawPtr,
                (unsigned long long)countdown.textPtr,
                (unsigned int)countdownLabelSlc,
                (unsigned int)countdownTextSlc, countdown.x, countdown.y,
                countdown.horzAlign, countdown.vertAlign,
                countdown.fontScale, countdown.timerValue, countdown.alpha,
                countdown.rawText.c_str());
            LogToFile(cbuf);
          }
        } else if (isTimerElem && timerValue > 0.05f && timerValue < 600.0f) {
          static std::unordered_map<uintptr_t, unsigned long>
              s_timerElemCandLogTick;
          const unsigned long nowCand = GetTickCount();
          auto itCand = s_timerElemCandLogTick.find(elem);
          if (itCand == s_timerElemCandLogTick.end() ||
              (nowCand - itCand->second) > 900) {
            s_timerElemCandLogTick[elem] = nowCand;
            std::string labelKey;
            std::string textKey;
            TryResolveHudScriptCountdownKeyFromCfg(countdownLabelSlc, labelKey);
            TryResolveHudScriptCountdownKeyFromCfg(countdownTextSlc, textKey);
            char cbuf[640];
            sprintf_s(
                cbuf,
                "[HUD-TIMER-CAND] type=%u ptr=0x%llX textPtr=0x%llX label=0x%X(%s) text=0x%X(%s) x=%.1f y=%.1f align=%u/%u fs=%.2f timer=%.3f raw=\"%.96s\"",
                t, (unsigned long long)elem, (unsigned long long)textPtr,
                (unsigned int)countdownLabelSlc,
                labelKey.empty() ? "-" : labelKey.c_str(),
                (unsigned int)countdownTextSlc,
                textKey.empty() ? "-" : textKey.c_str(), ex, ey, horzAlign,
                vertAlign, efs, timerValue, rawCountdownText.c_str());
            LogToFile(cbuf);
          }
        }
      }
    }
    if (!isTextElem) {
      continue;
    }

    if (!keepForLiveBridge) {
#if GHOSTSKOR_RUNTIME_DIAG
      static std::unordered_map<int, unsigned long> s_laneRejectLogTick;
      if (looksCenterPromptLane || (objLaneScore >= 40 && objLaneScore < 76)) {
        const int rejectKey = i;
        const unsigned long nowTick = GetTickCount();
        auto it = s_laneRejectLogTick.find(rejectKey);
        if (it == s_laneRejectLogTick.end() || (nowTick - it->second) > 1500) {
          s_laneRejectLogTick[rejectKey] = nowTick;
          char rbuf[192];
          sprintf_s(rbuf,
                    "[OBJ-NATIVE-SKIP-LANE] idx=%d x=%.1f y=%.1f fs=%.2f flags=0x%X score=%d notif=%d",
                    i, ex, ey, efs, eFlags, objLaneScore,
                    isObjectiveNotificationLane ? 1 : 0);
          LogToFile(rbuf);
        }
      }
#endif
      continue;
    }

    uint32_t ecolor = 0, efromColor = 0;
    int eFadeStart = 0, eFadeTime = 0, eServerTime = 0;
    int eFxBirth = 0, eFxLetter = 0;
    uint32_t labelSlc = 0, textSlc = 0;
    if (!ObjRev_ReadU32(elem + 0x40, labelSlc) ||  // +0x40 = label (was +0x80 = value)
        !ObjRev_ReadU32(elem + 0x30, ecolor) ||
        !ObjRev_ReadU32(elem + 0x34, efromColor) ||
        !ObjRev_ReadI32(elem + 0x38, eFadeStart) ||
        !ObjRev_ReadI32(elem + 0x3C, eFadeTime) ||
        !ObjRev_ReadI32(elem + 0x78, eServerTime) ||
        !ObjRev_ReadI32(elem + 0x90, eFxBirth) ||
        !ObjRev_ReadI32(elem + 0x94, eFxLetter) ||
        !ObjRev_ReadU32(elem + 0x84, textSlc)) {
      continue;
    }

    // Skip elements tracked by the intro system (prevents crash from
    // processing intro HudElems as objectives — intro title at y=-82
    // was passing objective score filter with score 114 and causing
    // OBJ-UNRESOLVED spam + potential crashes in key resolution).
    // BUT: do NOT skip objective-lane elements — the intro detour
    // captures ALL HudElem text renders (including objectives), so
    // g_nativeIntroMap would incorrectly block objectives after the
    // first frame. Objective lanes are safe to process here.
    if (textSlc > 0 && !isObjLane) {
      std::lock_guard<std::mutex> introLk(g_nativeIntroMutex);
      if (g_nativeIntroMap.count(textSlc) > 0) continue;
    }

    uint32_t resolvedCfg = 0;
    uint32_t bridgeSlcIndex = 0;
    if (ObjRev_IsPlausibleConfigIndex(textSlc)) {
      resolvedCfg = textSlc;
    } else if (ObjRev_IsPlausibleConfigIndex(labelSlc)) {
      resolvedCfg = labelSlc;
    }
    if (resolvedCfg != 0) {
      // Deterministic bridge: prefer direct cfg->slc provenance over raw cfg id.
      // Without this, hint lanes can carry cfg indices (e.g. 0x1F) while SLC
      // events/key maps are indexed by resolved slc (e.g. 0x5ACA), causing key
      // join misses and forcing fallback param probes.
      uint32_t directSlc = 0;
      uint32_t directBias = 0;
      bool haveDirectSlc =
          (ObjRev_GetDirectCfgSlc(resolvedCfg, directSlc, directBias, isObjLane) &&
           directSlc > 1);
      if (!haveDirectSlc && isObjLane) {
        // Objective-active gating can be cold on the first status frame.
        // Allow non-objective direct slc only for status-key corroboration.
        uint32_t anyDirectSlc = 0;
        uint32_t anyDirectBias = 0;
        if (ObjRev_GetDirectCfgSlc(resolvedCfg, anyDirectSlc, anyDirectBias, false) &&
            anyDirectSlc > 1) {
          std::string anyDirectKey;
          if (TextHook_GetSlcKey(anyDirectSlc, anyDirectKey) &&
              ObjRev_LooksLikeLocalizationKey(anyDirectKey.c_str()) &&
              IsObjectiveStatusMessageKeyName(anyDirectKey)) {
            directSlc = anyDirectSlc;
            directBias = anyDirectBias;
            haveDirectSlc = true;
          }
        }
      }
      if (haveDirectSlc) {
        bridgeSlcIndex = directSlc;
        if (!isObjLane && directSlc != resolvedCfg) {
          static std::unordered_map<uint64_t, unsigned long> s_hintCfgBridgeLogTick;
          const unsigned long nowCfgBridge = GetTickCount();
          const uint64_t sig = ((uint64_t)resolvedCfg << 32) | (uint64_t)directSlc;
          auto itLog = s_hintCfgBridgeLogTick.find(sig);
          if (itLog == s_hintCfgBridgeLogTick.end() ||
              (nowCfgBridge - itLog->second) > 1500) {
            s_hintCfgBridgeLogTick[sig] = nowCfgBridge;
            char bbuf[256];
            sprintf_s(
                bbuf,
                "[HUD-CFGSLC-BRIDGE] cfg=0x%X slc=0x%X bias=0x%X idx=%d x=%.1f y=%.1f fs=%.2f",
                resolvedCfg, directSlc, directBias, i, ex, ey, efs);
            LogToFile(bbuf);
          }
        }
      } else {
        bridgeSlcIndex = resolvedCfg;
      }
    }
    if (bridgeSlcIndex == 0) {
      if (textSlc > 1 && textSlc < 0x40000u) {
        bridgeSlcIndex = textSlc;
      } else if (labelSlc > 1 && labelSlc < 0x40000u) {
        bridgeSlcIndex = labelSlc;
      }
    }
    if (bridgeSlcIndex == 0) {
      if (isObjLane) {
        static std::unordered_map<uint32_t, unsigned long> s_cfgInvalidLogTick;
        unsigned long nowTick = GetTickCount();
        uint32_t rawCfg = (textSlc != 0) ? textSlc : labelSlc;
        auto itInvalid = s_cfgInvalidLogTick.find(rawCfg);
        if (itInvalid == s_cfgInvalidLogTick.end() ||
            (nowTick - itInvalid->second) > 1200) {
          s_cfgInvalidLogTick[rawCfg] = nowTick;
          char sbuf[320];
          sprintf_s(sbuf,
                    "[OBJ-NATIVE-SKIP-CFG] cfgLabel=0x%X cfgText=0x%X idx=%d "
                    "x=%.1f y=%.1f fs=%.2f flags=0x%X",
                    labelSlc, textSlc, i, ex, ey, efs, eFlags);
          LogToFile(sbuf);
        }
      }
      continue;
    }
    if (isObjLane && resolvedCfg == 0) {
      static std::unordered_map<uint32_t, unsigned long> s_cfgInvalidLogTick;
      unsigned long nowTick = GetTickCount();
      uint32_t rawCfg = (textSlc != 0) ? textSlc : labelSlc;
      auto itInvalid = s_cfgInvalidLogTick.find(rawCfg);
      if (itInvalid == s_cfgInvalidLogTick.end() ||
          (nowTick - itInvalid->second) > 1200) {
        s_cfgInvalidLogTick[rawCfg] = nowTick;
        char sbuf[320];
        sprintf_s(sbuf,
                  "[OBJ-NATIVE-SKIP-CFG] cfgLabel=0x%X cfgText=0x%X idx=%d "
                  "x=%.1f y=%.1f fs=%.2f flags=0x%X",
                  labelSlc, textSlc, i, ex, ey, efs, eFlags);
        LogToFile(sbuf);
      }
      continue;
    }

    int effectiveServerTime = eServerTime;
    if (effectiveServerTime <= 0 && globalServerTime > 0) {
      effectiveServerTime = globalServerTime;
    }

    std::string resolvedKey;
    const char *authSource = "none";
    bool hasAuthoritativeBind = false;
    const int authElemIndexResolved = ObjRev_ResolveHudElemArrayIndex(elem);
    const int authElemIndex = (authElemIndexResolved >= 0) ? authElemIndexResolved
                                                            : i;
    std::string cfgLiveKeyEarlyCheck; // Saved for reuse by status guard below
    bool cfgLiveKeyEarlyChecked = false;
    ObjConsumerResolveEvent recentConsumeEv{};
    bool hasRecentConsumeEv = false;
    unsigned long objInstanceFirstSeenTick = 0;
    bool hasObjInstanceFirstSeenTick = false;
    if (isObjLane && resolvedCfg != 0) {
      const unsigned long nowObjLane = GetTickCount();
      ObjRev_MarkObjectiveCfgActive(resolvedCfg, nowObjLane);
      activeObjectiveCfgSet.insert(resolvedCfg);
      if (eFxBirth > 0) {
        static std::unordered_map<uint64_t, unsigned long>
            s_objLaneInstanceFirstSeenTick;
        const uint64_t instanceSig =
            ((uint64_t)resolvedCfg << 32) ^ (uint64_t)(uint32_t)eFxBirth;
        auto itFirst = s_objLaneInstanceFirstSeenTick.find(instanceSig);
        if (itFirst == s_objLaneInstanceFirstSeenTick.end()) {
          s_objLaneInstanceFirstSeenTick[instanceSig] = nowObjLane;
          objInstanceFirstSeenTick = nowObjLane;
        } else {
          objInstanceFirstSeenTick = itFirst->second;
        }
        hasObjInstanceFirstSeenTick = (objInstanceFirstSeenTick != 0);
        if (s_objLaneInstanceFirstSeenTick.size() > 4096) {
          for (auto it = s_objLaneInstanceFirstSeenTick.begin();
               it != s_objLaneInstanceFirstSeenTick.end();) {
            if ((nowObjLane - it->second) > 15000) {
              it = s_objLaneInstanceFirstSeenTick.erase(it);
            } else {
              ++it;
            }
          }
        }
      }

      // Reverse telemetry join: capture recent cfg->slc consumer resolve events
      // for this live objective lane. This is diagnostic-only and does not
      // affect render authority.
      {
        ObjConsumerResolveEvent capEv{};
        if (ObjRev_GetRecentObjectiveConsumerResolve(resolvedCfg, 900, capEv)) {
          recentConsumeEv = capEv;
          hasRecentConsumeEv = true;
          static std::unordered_map<uint64_t, unsigned long>
              s_consumeJoinLogTick;
          const unsigned long nowJoin = GetTickCount();
          const uint64_t sig =
              ((uint64_t)resolvedCfg << 32) ^
              ((uint64_t)(uint16_t)authElemIndex << 16) ^
              (uint64_t)(capEv.slcIdx & 0xFFFFu);
          auto itJoin = s_consumeJoinLogTick.find(sig);
          if (itJoin == s_consumeJoinLogTick.end() ||
              (nowJoin - itJoin->second) > 1200) {
            s_consumeJoinLogTick[sig] = nowJoin;
            const DWORD ageMs =
                (nowJoin >= capEv.tick) ? (nowJoin - capEv.tick) : 0;
            char jbuf[512];
            sprintf_s(
                jbuf,
                "[OBJ-CONSUME-JOIN] cfg=0x%X idx=%d fxB=%d q=0x%X slc=0x%X caller=+0x%X age=%lu type=%u ord=%u kcls=%u key=%s",
                resolvedCfg, authElemIndex, eFxBirth, capEv.queryIdx,
                capEv.slcIdx,
                capEv.cfgCallerOffset, (unsigned long)ageMs, capEv.typeValue,
                capEv.ordinal, capEv.keyClass,
                capEv.keyName.empty() ? "(none)" : capEv.keyName.c_str());
            LogToFile(jbuf);
          }
        }
      }

      hasAuthoritativeBind = ObjRev_GetAuthoritativeObjectiveKey(
          resolvedCfg, eFxBirth, authElemIndex, resolvedKey, &authSource);

      // Native producer pending join:
      // when +0x34E25E fires before lane instance becomes bindable, keep a
      // short-lived pending event and finalize exact (cfg,fxBirth,elem)
      // authority when the lane appears.
      if (!hasAuthoritativeBind && isObjLane && resolvedCfg != 0 &&
          eFxBirth > 0) {
        uint32_t pendingSlc = 0;
        if (ObjRev_ConsumePendingObjectiveAuthorityForLane(
                resolvedCfg, eFxBirth, authElemIndex, elem, ey, &pendingSlc) &&
            ObjRev_GetAuthoritativeObjectiveKey(resolvedCfg, eFxBirth,
                                                authElemIndex,
                                                resolvedKey, &authSource)) {
          hasAuthoritativeBind = true;
          if (bridgeSlcIndex <= 1 && pendingSlc > 1) {
            bridgeSlcIndex = pendingSlc;
          }
        }
      }

      // Consumer resolve is telemetry-only in root-fix mode.
      // Do not mutate objective authority from consumer samples.

      // Cross-validate authority cache against live SLC content.
      // The engine can reuse the same (cfg, fxBirth, elemIdx) for a different
      // objective text.  When that happens, the authority cache is stale and
      // must be invalidated so the correct key is used.
      if (hasAuthoritativeBind && !resolvedKey.empty()) {
        std::string cfgLiveCheck = ObjRev_ResolveConfigKey(resolvedCfg, true, true);
        // When pref=true/direct=true fails (e.g. objectiveTick stale, keyClass
        // mismatch after HudElem repurposed for status text), retry with full
        // bias search but ONLY accept status keys.  Accepting objective keys
        // here would invalidate correct authority when the SLC cache is stale
        // (race between SLC hook update and render-thread read).
        if (cfgLiveCheck.empty()) {
          std::string fallbackKey = ObjRev_ResolveConfigKey(resolvedCfg, false, false);
          if (!fallbackKey.empty() && IsObjectiveStatusMessageKeyName(fallbackKey)) {
            cfgLiveCheck = fallbackKey;
          }
        }
        cfgLiveKeyEarlyCheck = cfgLiveCheck;
        cfgLiveKeyEarlyChecked = true;
        if (!cfgLiveCheck.empty() &&
            ObjRev_LooksLikeLocalizationKey(cfgLiveCheck.c_str()) &&
            cfgLiveCheck != resolvedKey) {
          // Live SLC disagrees with cached authority.  Check if the live key
          // is a known objective or status key before accepting it.
          const bool liveIsObjective = ObjRev_IsObjectiveListKeyStrict(cfgLiveCheck);
          const bool liveIsStatus = IsObjectiveStatusMessageKeyName(cfgLiveCheck);
          const bool liveIsOverlay = IsObjectiveOverlayKeyName(cfgLiveCheck);
          if (liveIsObjective || liveIsStatus || liveIsOverlay) {
            const bool liveStatusTrusted =
                (liveIsStatus &&
                 TextHook_IsTrustedObjectiveStatusCfgForKey(resolvedCfg,
                                                            cfgLiveCheck));
            const bool shouldInvalidateAuthority =
                (liveIsObjective || liveIsOverlay);
            const char *driftAction = shouldInvalidateAuthority
                                          ? "invalidated"
                                          : (liveStatusTrusted
                                                 ? "kept_status_trusted"
                                                 : "kept_untrusted_status");
            static std::unordered_map<uint64_t, unsigned long> s_authLiveDriftLogTick;
            const unsigned long nowDrift = GetTickCount();
            const uint64_t driftSig =
                ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^ (uint64_t)i;
            auto itDrift = s_authLiveDriftLogTick.find(driftSig);
            if (itDrift == s_authLiveDriftLogTick.end() ||
                (nowDrift - itDrift->second) > 1200) {
              s_authLiveDriftLogTick[driftSig] = nowDrift;
              char dbuf[448];
              sprintf_s(dbuf,
                        "[AUTH-LIVE-DRIFT] cfg=0x%X idx=%d fxB=%d cached=%s live=%s "
                        "src=%s -> %s",
                        resolvedCfg, i, eFxBirth, resolvedKey.c_str(),
                        cfgLiveCheck.c_str(), authSource, driftAction);
              LogToFile(dbuf);
            }
            if (shouldInvalidateAuthority) {
              // Exact-join mode: invalidate stale capture and drop this lane.
              ObjRev_InvalidateAuthoritativeInstance(resolvedCfg, eFxBirth,
                                                     authElemIndex, "live_drift");
              hasAuthoritativeBind = false;
              resolvedKey.clear();
              authSource = liveIsObjective
                               ? "live_drift_obj"
                               : (liveIsStatus ? "live_drift_status"
                                               : "live_drift_overlay");
            } else if (liveIsStatus) {
              // The game has temporarily repurposed this HudElem for a status
              // message. Use the status key FOR THIS FRAME so Korean matches
              // the English text. Don't invalidate authority — the objective
              // key will return when the status message finishes.
              resolvedKey = cfgLiveCheck;
              authSource = "live_status_override";
            }
          }
        }
      }

      // Deterministic live query:
      // cfgText -> (ConfigString_IndexToSlc with fixed objective bias) -> key.
      // Accept only objective-list keys already observed on +0x34E25E.
      if (kEnableRuntimeObjectiveAuthorityFallbacks && !hasAuthoritativeBind &&
          Original_ConfigString_IndexToSlc) {
        constexpr uint32_t kObjectiveCfgBias = 0x90D;
        const uint32_t queryIdx = resolvedCfg + kObjectiveCfgBias;
        uint32_t liveSlc = 0;
        if (ObjRev_TryConfigToSlc(resolvedCfg, kObjectiveCfgBias, liveSlc) &&
            liveSlc > 1) {
          std::string liveKey;
          if (TextHook_GetSlcKey(liveSlc, liveKey) &&
              ObjRev_LooksLikeLocalizationKey(liveKey.c_str()) &&
              ObjRev_IsObjectiveListKeyStrict(liveKey)) {
            bool observedByObjectiveCaller = false;
            {
              std::lock_guard<std::mutex> lk(g_objSlcObsMutex);
              for (const auto &obs : g_objSlcObservations) {
                if (!obs.key.empty() && obs.key == liveKey) {
                  observedByObjectiveCaller = true;
                  break;
                }
              }
            }
            if (observedByObjectiveCaller) {
              resolvedKey = liveKey;
              hasAuthoritativeBind = true;
              authSource = "live_cfg_query";
              cfgLiveKeyEarlyCheck = liveKey;
              cfgLiveKeyEarlyChecked = true;
              // Disabled in exact-join mode: runtime live query must not
              // synthesize authoritative producer events.
            } else {
              static std::unordered_map<uint64_t, unsigned long>
                  s_liveQueryMissObsLogTick;
              const unsigned long nowMissObs = GetTickCount();
              const uint64_t missSig =
                  ((uint64_t)resolvedCfg << 32) | (uint64_t)liveSlc;
              auto itMiss = s_liveQueryMissObsLogTick.find(missSig);
              if (itMiss == s_liveQueryMissObsLogTick.end() ||
                  (nowMissObs - itMiss->second) > 1500) {
                s_liveQueryMissObsLogTick[missSig] = nowMissObs;
                char mbuf[384];
                sprintf_s(
                    mbuf,
                    "[AUTH-LIVE-QUERY-MISS-OBS] cfg=0x%X query=0x%X slc=0x%X "
                    "key=%s idx=%d fxB=%d",
                    resolvedCfg, queryIdx, liveSlc, liveKey.c_str(), i,
                    eFxBirth);
                LogToFile(mbuf);
              }
            }
          }

          if (hasAuthoritativeBind) {
            static std::unordered_map<uint64_t, unsigned long> s_liveQueryLogTick;
            const unsigned long nowLive = GetTickCount();
            const uint64_t sig =
                ((uint64_t)resolvedCfg << 32) ^ (uint64_t)liveSlc;
            auto itLive = s_liveQueryLogTick.find(sig);
            if (itLive == s_liveQueryLogTick.end() ||
                (nowLive - itLive->second) > 1500) {
              s_liveQueryLogTick[sig] = nowLive;
              char lbuf[384];
              sprintf_s(
                  lbuf,
                  "[AUTH-LIVE-QUERY] cfg=0x%X query=0x%X slc=0x%X key=%s "
                  "idx=%d fxB=%d",
                  resolvedCfg, queryIdx, liveSlc, resolvedKey.c_str(), i,
                  eFxBirth);
              LogToFile(lbuf);
            }
          }
        }
      }

      // Guard obs_ordinal: when the live SLC content for this cfg is a
      // status key, the engine has repurposed this HudElem slot for a status
      // message.  Allowing obs_ordinal to fire would re-pollute the authority
      // cache with the previous objective key every frame, creating an
      // infinite invalidate→re-assign cycle.
      bool liveSlcIsStatus = false;
      const char *liveStatusGuardSource = "none";
      std::string liveStatusUntrustedKey;
      const char *liveStatusUntrustedSource = "none";
      if (!hasAuthoritativeBind) {
        const int laneSlotRaw = (int)lroundf((ey - 10.0f) / 20.0f);
        const bool isTopStatusLane = (laneSlotRaw == 0);
        bool topLaneProbeCorroborated = false;
        if (frameStatusProbeStrictActive && isTopStatusLane) {
          // Probe-only status adoption is too broad and can cross-contaminate
          // adjacent status keys (e.g., UPDATED vs COMPLETED) on the same cfg.
          // Require corroboration from at least one cfg-local signal.
          if (!cfgLiveKeyEarlyCheck.empty() &&
              IsObjectiveStatusMessageKeyName(cfgLiveKeyEarlyCheck) &&
              cfgLiveKeyEarlyCheck == recentStatusProbeKeyStrict) {
            topLaneProbeCorroborated = true;
          }
          if (!topLaneProbeCorroborated && bridgeSlcIndex > 1) {
            std::string bridgeProbeKey;
            if (TextHook_GetSlcKey(bridgeSlcIndex, bridgeProbeKey) &&
                IsObjectiveStatusMessageKeyName(bridgeProbeKey) &&
                bridgeProbeKey == recentStatusProbeKeyStrict) {
              topLaneProbeCorroborated = true;
            }
          }
        }
        if (frameStatusProbeStrictActive && isTopStatusLane &&
            topLaneProbeCorroborated) {
          liveSlcIsStatus = true;
          liveStatusGuardSource = "probe_seen_toplane";
          if (cfgLiveKeyEarlyCheck.empty()) {
            cfgLiveKeyEarlyCheck = recentStatusProbeKeyStrict;
          }
          cfgLiveKeyEarlyChecked = true;
          static std::unordered_map<uint64_t, unsigned long>
              s_obsProbeStatusGuardLogTick;
          const unsigned long nowGuard = GetTickCount();
          const uint64_t guardSig =
              ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^
              (uint64_t)i;
          auto itGuard = s_obsProbeStatusGuardLogTick.find(guardSig);
          if (itGuard == s_obsProbeStatusGuardLogTick.end() ||
              (nowGuard - itGuard->second) > 1200) {
            s_obsProbeStatusGuardLogTick[guardSig] = nowGuard;
            char gbuf[448];
            sprintf_s(
                gbuf,
                "[AUTH-OBS-PROBE-GUARD] cfg=0x%X idx=%d fxB=%d lane=%d key=%s rp=1",
                resolvedCfg, i, eFxBirth, laneSlotRaw,
                recentStatusProbeKeyStrict.c_str());
            LogToFile(gbuf);
          }
        } else if (frameStatusProbeStrictActive && isTopStatusLane &&
                   !topLaneProbeCorroborated) {
          static std::unordered_map<uint64_t, unsigned long>
              s_obsProbeStatusSkipLogTick;
          const unsigned long nowGuard = GetTickCount();
          const uint64_t guardSig =
              ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^
              (uint64_t)i;
          auto itGuard = s_obsProbeStatusSkipLogTick.find(guardSig);
          if (itGuard == s_obsProbeStatusSkipLogTick.end() ||
              (nowGuard - itGuard->second) > 1200) {
            s_obsProbeStatusSkipLogTick[guardSig] = nowGuard;
            char gbuf[448];
            sprintf_s(
                gbuf,
                "[AUTH-OBS-PROBE-SKIP] cfg=0x%X idx=%d fxB=%d lane=%d key=%s reason=no_corroboration",
                resolvedCfg, i, eFxBirth, laneSlotRaw,
                recentStatusProbeKeyStrict.c_str());
            LogToFile(gbuf);
          }
        }

        auto ResolveStatusProbeMatchSlc = [&](const std::string &statusKey)
            -> uint32_t {
          if (statusKey.empty() || !IsObjectiveStatusMessageKeyName(statusKey)) {
            return 0;
          }
          uint32_t directCfgSlc = 0;
          uint32_t directCfgBias = 0;
          if (ObjRev_GetDirectCfgSlc(resolvedCfg, directCfgSlc, directCfgBias, false) &&
              directCfgSlc > 1) {
            std::string directCfgKey;
            if (TextHook_GetSlcKey(directCfgSlc, directCfgKey) &&
                ObjRev_LooksLikeLocalizationKey(directCfgKey.c_str()) &&
                directCfgKey == statusKey) {
              return directCfgSlc;
            }
          }
          if (bridgeSlcIndex > 1) {
            std::string bridgeKey;
            if (TextHook_GetSlcKey(bridgeSlcIndex, bridgeKey) &&
                ObjRev_LooksLikeLocalizationKey(bridgeKey.c_str()) &&
                bridgeKey == statusKey) {
              return bridgeSlcIndex;
            }
          }
          return 0;
        };

        auto TryAcceptTrustedLiveStatusKey =
            [&](const std::string &candidateKey,
                const char *candidateSource) -> bool {
          if (candidateKey.empty() ||
              !ObjRev_LooksLikeLocalizationKey(candidateKey.c_str()) ||
              !IsObjectiveStatusMessageKeyName(candidateKey)) {
            return false;
          }
          if (TextHook_IsTrustedObjectiveStatusCfgForKey(resolvedCfg,
                                                         candidateKey)) {
            liveSlcIsStatus = true;
            liveStatusGuardSource = candidateSource;
            return true;
          }
          // Bootstrap trust from confirmed status-consumer probes
          // when key+slc are corroborated in the same short window.
          std::string probeKeyByName;
          uint32_t probeSlcByName = 0;
          bool probeHasParamsByName = false;
          unsigned int probeCallerByName = 0;
          const uint32_t probeMatchSlc =
              ResolveStatusProbeMatchSlc(candidateKey);
          if (TextHook_GetRecentObjectiveStatusProbeByKeyFromConsumers(
                  260, candidateKey.c_str(), probeKeyByName, &probeSlcByName,
                  &probeHasParamsByName, &probeCallerByName) &&
              probeHasParamsByName &&
              probeKeyByName == candidateKey &&
              probeSlcByName > 1 &&
              probeMatchSlc > 1 &&
              probeSlcByName == probeMatchSlc) {
            const unsigned long nowTrust = GetTickCount();
            TextHook_RecordObjectiveStatusCfgTrust(candidateKey, resolvedCfg,
                                                   nowTrust,
                                                   "probe_cfg_match");
            liveSlcIsStatus = true;
            liveStatusGuardSource = "probe_cfg_match";
            return true;
          }
          liveStatusUntrustedKey = candidateKey;
          liveStatusUntrustedSource = candidateSource;
          return false;
        };

        // Primary check: direct cfg->slc status key (fast path).
        if (!cfgLiveKeyEarlyChecked) {
          cfgLiveKeyEarlyCheck = ObjRev_ResolveConfigKey(resolvedCfg, true, true);
          if (cfgLiveKeyEarlyCheck.empty()) {
            const std::string cfgLiveAnyDirect =
                ObjRev_ResolveConfigKey(resolvedCfg, false, false);
            if (!cfgLiveAnyDirect.empty() &&
                ObjRev_LooksLikeLocalizationKey(cfgLiveAnyDirect.c_str()) &&
                IsObjectiveStatusMessageKeyName(cfgLiveAnyDirect)) {
              cfgLiveKeyEarlyCheck = cfgLiveAnyDirect;
            }
          }
          cfgLiveKeyEarlyChecked = true;
        }
        if (!liveSlcIsStatus && !cfgLiveKeyEarlyCheck.empty()) {
          TryAcceptTrustedLiveStatusKey(cfgLiveKeyEarlyCheck, "cfg_live_direct");
        }

        // Secondary check: bridge slc->key map can already know status while
        // direct cfg->slc is still cold at mission start.
        if (!liveSlcIsStatus && bridgeSlcIndex > 1) {
          std::string bridgeGuardKey;
          if (TextHook_GetSlcKey(bridgeSlcIndex, bridgeGuardKey) &&
              ObjRev_LooksLikeLocalizationKey(bridgeGuardKey.c_str()) &&
              IsObjectiveStatusMessageKeyName(bridgeGuardKey)) {
            TryAcceptTrustedLiveStatusKey(bridgeGuardKey, "bridge_slc");
            if (cfgLiveKeyEarlyCheck.empty()) {
              cfgLiveKeyEarlyCheck = bridgeGuardKey;
            }
          }
        }

        // Non-direct cfg fallback is intentionally disabled here.
        // Bias-probe resolver can cross-contaminate objective/status cfg lanes.

        if (liveSlcIsStatus) {
          static std::unordered_map<uint64_t, unsigned long>
              s_obsOrdinalStatusGuardLogTick;
          const unsigned long nowGuard = GetTickCount();
          const uint64_t guardSig =
              ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^ (uint64_t)i;
          auto itGuard = s_obsOrdinalStatusGuardLogTick.find(guardSig);
          if (itGuard == s_obsOrdinalStatusGuardLogTick.end() ||
              (nowGuard - itGuard->second) > 1200) {
            s_obsOrdinalStatusGuardLogTick[guardSig] = nowGuard;
            char gbuf[448];
            sprintf_s(gbuf,
                      "[AUTH-OBS-STATUS-GUARD] cfg=0x%X idx=%d fxB=%d src=%s key=%s",
                      resolvedCfg, i, eFxBirth, liveStatusGuardSource,
                      cfgLiveKeyEarlyCheck.empty() ? "(none)"
                                                   : cfgLiveKeyEarlyCheck.c_str());
            LogToFile(gbuf);
          }
        } else if (!liveStatusUntrustedKey.empty()) {
          static std::unordered_map<uint64_t, unsigned long>
              s_obsOrdinalStatusUntrustedLogTick;
          const unsigned long nowGuard = GetTickCount();
          const uint64_t guardSig =
              ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^
              (uint64_t)i;
          auto itGuard = s_obsOrdinalStatusUntrustedLogTick.find(guardSig);
          if (itGuard == s_obsOrdinalStatusUntrustedLogTick.end() ||
              (nowGuard - itGuard->second) > 1200) {
            s_obsOrdinalStatusUntrustedLogTick[guardSig] = nowGuard;
            char gbuf[448];
            sprintf_s(
                gbuf,
                "[AUTH-OBS-STATUS-UNTRUSTED] cfg=0x%X idx=%d fxB=%d src=%s key=%s",
                resolvedCfg, i, eFxBirth, liveStatusUntrustedSource,
                liveStatusUntrustedKey.c_str());
            LogToFile(gbuf);
          }
        }
      }

      // Top-left rebuild rule:
      // objective authority must come from exact native join only.
      // Ordinal observations may enrich debugging/telemetry, but they must not
      // assign keys to live HudElem instances.

      if (!hasAuthoritativeBind && !kRequireAuthoritativeBind) {
        // Optional compatibility fallback (disabled in root-fix mode).
        resolvedKey = ObjRev_ResolveConfigKey(resolvedCfg, true, true);
      }

      // Direct key resolution from live SLC cache.
      // When no authority bind exists yet, resolve directly from the SLC cache.
      // The cache has the current configstring→key mapping, giving us the EXACT
      // key the game is displaying right now.  This eliminates the multi-frame
      // delay of the OSAUTH authority pipeline (which can leave Korean invisible
      // until alpha ≥ 0.7).
      // Accepts: status keys (always) and objective-list keys (in obj lane only).
      // Once OSAUTH captures the authority, it takes over on subsequent frames.
      if (!hasAuthoritativeBind && resolvedKey.empty() && resolvedCfg != 0) {
        std::string directKey =
            ObjRev_ResolveConfigKey(resolvedCfg, false, false);
        if (!directKey.empty() &&
            ObjRev_LooksLikeLocalizationKey(directKey.c_str())) {
          const bool isStatus = IsObjectiveStatusMessageKeyName(directKey);
          // For objectives, skip the strict observation requirement
          // (ObjRev_IsObjectiveListKeyStrict) — we're resolving BEFORE the
          // authority system observes the key.  The isObjLane gate (score≥100,
          // x≈6 y≈10-70 fs≈1.25 flags=0x5) plus SLC cache provenance is
          // sufficient.  Once OSAUTH captures, it takes over next frame.
          const bool isObjective =
              isObjLane && !isStatus &&
              directKey != "CGAME_MISSIONOBJECTIVES" &&
              TextHook_OSRev_IsNativeObjectiveCandidateKey(directKey);
          if (isStatus || isObjective) {
            resolvedKey = directKey;
            authSource = isStatus ? "live_status_slc_direct"
                                  : "live_obj_slc_direct";
          }
        }
      }

      // Targeted configstring→key resolution using SLC hook observations.
      //
      // The SLC hook from caller +0x34E25E captures (slcIdx, key) when the
      // engine resolves each objective HudElem text configstring.  We use
      // these observations in two strategies:
      //
      // Strategy A (direct SLC base offset):
      //   Assumes SLC indices are contiguous: slcIdx = cfgIdx + constant.
      //   Compute candidateBase = obs.slcIdx - cfgIdx.  Fast, O(1) per frame
      //   once discovered.  Verified by cross-checking other active cfgIdx.
      //
      // Strategy B (targeted ConfigString_IndexToSlc search):
      //   Search fn(cfgIdx + bias) for bias in [0, 0xA000], comparing the
      //   result against known SLC observations.  Works even when SLC indices
      //   are not contiguous.  Runs at most once (throttled 5s).
      // Root-fix rule:
      // Do NOT recompute objective SLC index in render path.
      // (No direct-base/bias search/target fn search here.)
      if (kEnableRuntimeObjectiveAuthorityFallbacks && !hasAuthoritativeBind &&
          resolvedCfg != 0) {

        // ---- Fast path: previously discovered fn()-based bias ----
        uint32_t objBias = g_ObjRevObjectiveBias.load();
        if (objBias != 0) {
          uint32_t probeSlcIdx = 0;
          if (ObjRev_TryConfigToSlc(resolvedCfg, objBias, probeSlcIdx) &&
              probeSlcIdx > 1) {
            std::string probeKey;
            if (TextHook_GetSlcKey(probeSlcIdx, probeKey) &&
                ObjRev_LooksLikeLocalizationKey(probeKey.c_str()) &&
                IsObjectiveOverlayKeyName(probeKey)) {
              resolvedKey = probeKey;
              authSource = "bias_cached";
              hasAuthoritativeBind = true;
            }
            if (!hasAuthoritativeBind) {
              std::string rawStr = ObjRev_ReadSlcStringRaw(probeSlcIdx);
              if (!rawStr.empty()) {
                if (ObjRev_LooksLikeLocalizationKey(rawStr.c_str()) &&
                    IsObjectiveOverlayKeyName(rawStr)) {
                  resolvedKey = rawStr;
                  authSource = "bias_cached_raw";
                  hasAuthoritativeBind = true;
                } else {
                  std::string mappedKey;
                  const std::string rawNorm =
                      ObjRev_SanitizeEnglishForKeyResolve(rawStr);
                  if (!rawNorm.empty() &&
                      TextHook_ResolveKeyFromEnglish(rawNorm, mappedKey) &&
                      ObjRev_LooksLikeLocalizationKey(mappedKey.c_str()) &&
                      IsObjectiveOverlayKeyName(mappedKey)) {
                    resolvedKey = mappedKey;
                    authSource = "bias_cached_eng";
                    hasAuthoritativeBind = true;
                  }
                }
              }
            }
            if (hasAuthoritativeBind) {
              ObjRev_RecordAuthoritativeObjectiveCapture(
                  resolvedKey, resolvedCfg, 0, eFxBirth, elem, 0, false);
            }
          }
        }

        // ---- Strategy A: Direct SLC base offset (linear assumption) ----
        // slcIdx = cfgIdx + directBase.  Typical when SLC indices are
        // allocated in the same order as HudElem text configstrings.
        if (!hasAuthoritativeBind) {
          uint32_t directBase = g_objSlcDirectBase.load();

          // Discover base from SLC hook observations.
          if (directBase == 0 && eFxBirth > 0) {
            std::lock_guard<std::mutex> lk(g_objSlcObsMutex);
            for (const auto &obs : g_objSlcObservations) {
              if (!IsObjectiveOverlayKeyName(obs.key)) continue;
              if (obs.slcIdx <= resolvedCfg) continue;
              uint32_t candidateBase = obs.slcIdx - resolvedCfg;
              if (candidateBase < 0x100 || candidateBase > 0xFFFF) continue;

              // Verify: SLC at (candidateBase + resolvedCfg) must be
              // valid objective content.
              std::string rawCheck =
                  ObjRev_ReadSlcStringRaw(candidateBase + resolvedCfg);
              if (rawCheck.empty()) continue;
              bool valid = false;
              std::string rawCheckKey;
              if (ObjRev_LooksLikeLocalizationKey(rawCheck.c_str()) &&
                  IsObjectiveOverlayKeyName(rawCheck)) {
                valid = true;
                rawCheckKey = rawCheck;
              } else {
                std::string mappedKey;
                const std::string rawCheckNorm =
                    ObjRev_SanitizeEnglishForKeyResolve(rawCheck);
                if (!rawCheckNorm.empty() &&
                    TextHook_ResolveKeyFromEnglish(rawCheckNorm, mappedKey) &&
                    ObjRev_LooksLikeLocalizationKey(mappedKey.c_str()) &&
                    IsObjectiveOverlayKeyName(mappedKey)) {
                  valid = true;
                  rawCheckKey = mappedKey;
                }
              }
              if (!valid) continue;

              // Cross-verify with another active objective cfgIdx.
              // A single observation cannot determine linear base uniquely.
              bool crossOk = false;
              // Deterministic single-line accept:
              // If candidate (cfg + base) resolves to the exact observed key,
              // this base is directly evidenced by the same engine event.
              if (!rawCheckKey.empty() && rawCheckKey == obs.key) {
                crossOk = true;
              }
              for (uint32_t otherCfg : activeObjectiveCfgSet) {
                if (otherCfg == resolvedCfg) continue;
                std::string otherRaw =
                    ObjRev_ReadSlcStringRaw(candidateBase + otherCfg);
                if (!otherRaw.empty()) {
                  std::string otherKey;
                  bool otherValid =
                      (ObjRev_LooksLikeLocalizationKey(otherRaw.c_str()) &&
                       IsObjectiveOverlayKeyName(otherRaw)) ||
                      (TextHook_ResolveKeyFromEnglish(
                           ObjRev_SanitizeEnglishForKeyResolve(otherRaw), otherKey) &&
                       !otherKey.empty() &&
                       IsObjectiveOverlayKeyName(otherKey));
                  if (otherValid) {
                    crossOk = true;
                    break;
                  }
                }
              }
              if (crossOk) {
                g_objSlcDirectBase.store(candidateBase);
                directBase = candidateBase;
                char dbuf[320];
                sprintf_s(dbuf,
                          "[OBJ-SLC-DIRECT-BASE] base=0x%X obsSlc=0x%X "
                          "cfg=0x%X key=%s",
                          candidateBase, obs.slcIdx, resolvedCfg,
                          obs.key.c_str());
                LogToFile(dbuf);
                break;
              }
            }
          }

          // Use direct base to resolve key.
          if (directBase != 0) {
            uint32_t slcIdx = resolvedCfg + directBase;
            // Check g_slcToKey first (populated by SLC hook).
            std::string probeKey;
            if (TextHook_GetSlcKey(slcIdx, probeKey) &&
                ObjRev_LooksLikeLocalizationKey(probeKey.c_str()) &&
                IsObjectiveOverlayKeyName(probeKey)) {
              resolvedKey = probeKey;
              authSource = "direct_base_slc";
              hasAuthoritativeBind = true;
            }
            if (!hasAuthoritativeBind) {
              std::string rawStr = ObjRev_ReadSlcStringRaw(slcIdx);
              if (!rawStr.empty()) {
                if (ObjRev_LooksLikeLocalizationKey(rawStr.c_str()) &&
                    IsObjectiveOverlayKeyName(rawStr)) {
                  resolvedKey = rawStr;
                  authSource = "direct_base_raw";
                  hasAuthoritativeBind = true;
                } else {
                  std::string mappedKey;
                  const std::string rawNorm =
                      ObjRev_SanitizeEnglishForKeyResolve(rawStr);
                  if (!rawNorm.empty() &&
                      TextHook_ResolveKeyFromEnglish(rawNorm, mappedKey) &&
                      ObjRev_LooksLikeLocalizationKey(mappedKey.c_str()) &&
                      IsObjectiveOverlayKeyName(mappedKey)) {
                    resolvedKey = mappedKey;
                    authSource = "direct_base_eng";
                    hasAuthoritativeBind = true;
                  }
                }
              }
            }
            if (hasAuthoritativeBind) {
              ObjRev_RecordAuthoritativeObjectiveCapture(
                  resolvedKey, resolvedCfg, 0, eFxBirth, elem, 0, false);
              static std::unordered_map<uint32_t, unsigned long>
                  s_directBaseLogTick;
              unsigned long nowDb = GetTickCount();
              auto itDb = s_directBaseLogTick.find(resolvedCfg);
              if (itDb == s_directBaseLogTick.end() ||
                  (nowDb - itDb->second) > 2000) {
                s_directBaseLogTick[resolvedCfg] = nowDb;
                char abuf[384];
                sprintf_s(abuf,
                          "[AUTH-DIRECT-BASE] cfg=0x%X base=0x%X slc=0x%X "
                          "key=%s idx=%d fxB=%d src=%s",
                          resolvedCfg, directBase, slcIdx,
                          resolvedKey.c_str(), i, eFxBirth, authSource);
                LogToFile(abuf);
              }
            } else {
              // Diagnostic: direct base read failed for this cfgIdx.
              static std::unordered_map<uint32_t, unsigned long>
                  s_directBaseMissLogTick;
              unsigned long nowMiss = GetTickCount();
              auto itMiss = s_directBaseMissLogTick.find(resolvedCfg);
              if (itMiss == s_directBaseMissLogTick.end() ||
                  (nowMiss - itMiss->second) > 3000) {
                s_directBaseMissLogTick[resolvedCfg] = nowMiss;
                std::string rawDiag = ObjRev_ReadSlcStringRaw(slcIdx);
                char mbuf[384];
                sprintf_s(mbuf,
                          "[AUTH-DIRECT-BASE-MISS] cfg=0x%X base=0x%X "
                          "slc=0x%X raw=\"%.60s\" idx=%d x=%.1f y=%.1f",
                          resolvedCfg, directBase, slcIdx,
                          rawDiag.empty() ? "(empty)" : rawDiag.c_str(),
                          i, ex, ey);
                LogToFile(mbuf);
              }
              // Direct-base self-heal: if objective-lane items (fxBirth>0)
              // repeatedly miss with the same base, clear and rediscover.
              if (eFxBirth > 0) {
                static uint32_t s_lastDirectBaseMissBase = 0;
                static unsigned long s_lastDirectBaseMissTick = 0;
                static int s_directBaseMissStreak = 0;
                if (s_lastDirectBaseMissBase != directBase ||
                    (nowMiss - s_lastDirectBaseMissTick) > 2500) {
                  s_directBaseMissStreak = 0;
                }
                s_lastDirectBaseMissBase = directBase;
                s_lastDirectBaseMissTick = nowMiss;
                s_directBaseMissStreak += 1;
                if (s_directBaseMissStreak >= 3) {
                  g_objSlcDirectBase.store(0);
                  s_directBaseMissStreak = 0;
                  char rbuf[256];
                  sprintf_s(rbuf,
                            "[OBJ-SLC-DIRECT-BASE-RESET] oldBase=0x%X "
                            "cfg=0x%X idx=%d fxB=%d",
                            directBase, resolvedCfg, i, eFxBirth);
                  LogToFile(rbuf);
                }
              }
            }
          }
        }

        // Disabled dead-end: Strategy A2 observation-order matching.
        if (kEnableRuntimeObjectiveAuthorityFallbacks && !hasAuthoritativeBind) {
          static std::unordered_map<uint32_t, std::string> s_cfgBoundKey;
          static unsigned long s_cfgBoundResetTick = 0;
          uint32_t cfgDirectSlc = 0;
          uint32_t cfgDirectBias = 0;
          const bool haveCfgDirectSlc =
              ObjRev_GetDirectCfgSlc(resolvedCfg, cfgDirectSlc, cfgDirectBias, true);
          unsigned long curResetTick = ObjectiveRuntime::GetRuntimeResetTick();
          if (curResetTick > s_cfgBoundResetTick) {
            s_cfgBoundKey.clear();
            s_cfgBoundResetTick = curResetTick;
          }
          // Strict mode: observation bind requires matching cfg->slc evidence.
          if (haveCfgDirectSlc) {
            std::lock_guard<std::mutex> lk(g_objSlcObsMutex);
            for (const auto &obs : g_objSlcObservations) {
              // Skip observations already bound to another cfgIdx.
              bool alreadyBound = false;
              for (const auto &bnd : s_cfgBoundKey) {
                if (bnd.first != resolvedCfg && bnd.second == obs.key) {
                  alreadyBound = true;
                  break;
                }
              }
              if (alreadyBound) continue;
              // Check if this key is a valid objective key.
              if (!IsObjectiveOverlayKeyName(obs.key)) continue;
              // Require exact slc match to avoid cross-instance contamination.
              if (obs.slcIdx != cfgDirectSlc) continue;
              // Bind it.
              resolvedKey = obs.key;
              authSource = "obs_key_match";
              hasAuthoritativeBind = true;
              s_cfgBoundKey[resolvedCfg] = obs.key;
              ObjRev_RecordAuthoritativeObjectiveCapture(
                  resolvedKey, resolvedCfg, 0, eFxBirth, elem, 0, false);
              {
                std::lock_guard<std::mutex> slkLk(g_slcToKeyMutex);
                g_slcToKey[obs.slcIdx] = obs.key;
              }
              char obuf[384];
              sprintf_s(obuf,
                        "[AUTH-OBS-KEY] cfg=0x%X slc=0x%X key=%s idx=%d "
                        "fxB=%d x=%.1f y=%.1f bias=0x%X",
                        resolvedCfg, obs.slcIdx, obs.key.c_str(), i,
                        eFxBirth, ex, ey, cfgDirectBias);
              LogToFile(obuf);
              break;
            }
          }
        }

        // Disabled dead-end: wide bias search for cfg->slc in render path.
        if (kEnableRuntimeObjectiveAuthorityFallbacks && !hasAuthoritativeBind &&
            objBias == 0) {
          static unsigned long s_lastTargetSearch = 0;
          unsigned long nowTarget = GetTickCount();
          if ((nowTarget - s_lastTargetSearch) > 5000) {
            s_lastTargetSearch = nowTarget;

            // Collect target slcIdx values from SLC hook observations.
            std::vector<uint32_t> targetSlcValues;
            {
              std::lock_guard<std::mutex> lk(g_objSlcObsMutex);
              for (const auto &obs : g_objSlcObservations)
                targetSlcValues.push_back(obs.slcIdx);
            }

            if (!targetSlcValues.empty()) {
              uint32_t foundBias = 0;
              uint32_t foundSlcIdx = 0;
              // Scan biases 0 to 0x10000 (65536), calling
              // ConfigString_IndexToSlc via ObjRev_TryConfigToSlc (SEH-safe).
              for (uint32_t bias = 0; bias <= 0x10000u; ++bias) {
                uint32_t probeSlcIdx = 0;
                if (!ObjRev_TryConfigToSlc(resolvedCfg, bias, probeSlcIdx))
                  continue;
                if (probeSlcIdx <= 1) continue;
                for (uint32_t target : targetSlcValues) {
                  if (probeSlcIdx == target) {
                    foundBias = bias;
                    foundSlcIdx = probeSlcIdx;
                    break;
                  }
                }
                if (foundBias != 0) break;
              }

              if (foundBias != 0) {
                g_ObjRevObjectiveBias.store(foundBias);
                g_ObjRevObjectiveBiasTick.store(nowTarget);
                // Resolve key for this frame.
                std::string rawStr = ObjRev_ReadSlcStringRaw(foundSlcIdx);
                if (!rawStr.empty()) {
                  if (ObjRev_LooksLikeLocalizationKey(rawStr.c_str()) &&
                      IsObjectiveOverlayKeyName(rawStr)) {
                    resolvedKey = rawStr;
                    authSource = "target_fn_raw";
                    hasAuthoritativeBind = true;
                  } else {
                    std::string mappedKey;
                    const std::string rawNorm =
                        ObjRev_SanitizeEnglishForKeyResolve(rawStr);
                    if (!rawNorm.empty() &&
                        TextHook_ResolveKeyFromEnglish(rawNorm, mappedKey) &&
                        ObjRev_LooksLikeLocalizationKey(mappedKey.c_str()) &&
                        IsObjectiveOverlayKeyName(mappedKey)) {
                      resolvedKey = mappedKey;
                      authSource = "target_fn_eng";
                      hasAuthoritativeBind = true;
                    }
                  }
                }
                if (hasAuthoritativeBind) {
                  ObjRev_RecordAuthoritativeObjectiveCapture(
                      resolvedKey, resolvedCfg, 0, eFxBirth, elem, 0, false);
                }
                char pbuf[384];
                sprintf_s(pbuf,
                          "[AUTH-TARGET-FN] cfg=0x%X bias=0x%X slc=0x%X "
                          "key=%s idx=%d fxB=%d",
                          resolvedCfg, foundBias, foundSlcIdx,
                          hasAuthoritativeBind ? resolvedKey.c_str()
                                              : "(unresolved)",
                          i, eFxBirth);
                LogToFile(pbuf);
              } else {
                char mbuf[256];
                sprintf_s(mbuf,
                          "[AUTH-TARGET-FN-MISS] cfg=0x%X idx=%d x=%.1f "
                          "y=%.1f fxB=%d targets=%d range=0x10000",
                          resolvedCfg, i, ex, ey, eFxBirth,
                          (int)targetSlcValues.size());
                LogToFile(mbuf);
              }
            } else {
              static unsigned long s_noObsLog = 0;
              if ((nowTarget - s_noObsLog) > 10000) {
                s_noObsLog = nowTarget;
                char nbuf[192];
                sprintf_s(nbuf,
                          "[AUTH-TARGET-NO-OBS] cfg=0x%X idx=%d — waiting for "
                          "SLC hook observations",
                          resolvedCfg, i);
                LogToFile(nbuf);
              }
            }
          }
        }
      }
    }
    std::string statusLaneKey;
    std::string statusKeyConsume;
    std::string statusKeyResolved;
    std::string statusKeyBridge;
    std::string statusKeyCfg;
    std::string statusKeyUntrusted;
    // Consumer resolve is not status authority.
    if (isObjLane && statusKeyConsume.empty() && frameStatusProbeStrictActive) {
      const int laneSlotRaw = (int)lroundf((ey - 10.0f) / 20.0f);
      if (laneSlotRaw == 0) {
        bool probeCorroborated = false;
        if (!cfgLiveKeyEarlyCheck.empty() &&
            IsObjectiveStatusMessageKeyName(cfgLiveKeyEarlyCheck) &&
            cfgLiveKeyEarlyCheck == recentStatusProbeKeyStrict) {
          probeCorroborated = true;
        }
        if (!probeCorroborated && !statusKeyCfg.empty() &&
            statusKeyCfg == recentStatusProbeKeyStrict) {
          probeCorroborated = true;
        }
        if (probeCorroborated) {
          statusKeyConsume = recentStatusProbeKeyStrict;
          static std::unordered_map<uint64_t, unsigned long>
              s_statusProbeConsumeAdoptLogTick;
          const unsigned long nowAdopt = GetTickCount();
          const uint64_t sig =
              ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^
              (uint64_t)i;
          auto itLog = s_statusProbeConsumeAdoptLogTick.find(sig);
          if (itLog == s_statusProbeConsumeAdoptLogTick.end() ||
              (nowAdopt - itLog->second) > 1200) {
            s_statusProbeConsumeAdoptLogTick[sig] = nowAdopt;
            char cbuf[448];
            sprintf_s(
                cbuf,
                "[STATUS-PROBE-TOP-ADOPT] cfg=0x%X idx=%d fxB=%d lane=%d key=%s",
                resolvedCfg, i, eFxBirth, laneSlotRaw,
                statusKeyConsume.c_str());
            LogToFile(cbuf);
          }
        }
      }
    }
    if (isObjLane && !resolvedKey.empty() &&
        IsObjectiveStatusMessageKeyName(resolvedKey) && resolvedCfg != 0) {
      if (TextHook_IsTrustedObjectiveStatusCfgForKey(resolvedCfg, resolvedKey)) {
        statusKeyResolved = resolvedKey;
      } else {
        statusKeyUntrusted = resolvedKey;
      }
    }
    if (isObjLane && bridgeSlcIndex > 1 && resolvedCfg != 0) {
      std::string bridgeStatusKey;
      if (TextHook_GetSlcKey(bridgeSlcIndex, bridgeStatusKey) &&
          ObjRev_LooksLikeLocalizationKey(bridgeStatusKey.c_str()) &&
          IsObjectiveStatusMessageKeyName(bridgeStatusKey)) {
        if (TextHook_IsTrustedObjectiveStatusCfgForKey(resolvedCfg,
                                                       bridgeStatusKey)) {
          statusKeyBridge = bridgeStatusKey;
        } else if (statusKeyUntrusted.empty()) {
          statusKeyUntrusted = bridgeStatusKey;
        }
      }
    }

    // Guardrail: when cfg currently resolves to a status/update key, do not
    // reuse stale objective-list authority for that lane.
    if (isObjLane && resolvedCfg != 0) {
      // Reuse the live SLC result from the cross-validation above if available,
      // otherwise resolve it now.
      std::string cfgLiveKey = cfgLiveKeyEarlyChecked
          ? cfgLiveKeyEarlyCheck
          : ObjRev_ResolveConfigKey(resolvedCfg, true, true);
      if (cfgLiveKey.empty()) {
        const std::string cfgLiveAnyDirect =
            ObjRev_ResolveConfigKey(resolvedCfg, false, true);
        if (!cfgLiveAnyDirect.empty() &&
            ObjRev_LooksLikeLocalizationKey(cfgLiveAnyDirect.c_str()) &&
            IsObjectiveStatusMessageKeyName(cfgLiveAnyDirect)) {
          cfgLiveKey = cfgLiveAnyDirect;
        }
      }
      // Keep status guard on direct cfg->slc only. Non-direct fallback can
      // re-open probe contamination between adjacent cfg lanes.
      const bool cfgLiveStatusKey =
          (!cfgLiveKey.empty() &&
           ObjRev_LooksLikeLocalizationKey(cfgLiveKey.c_str()) &&
           IsObjectiveUpdateKeyName(cfgLiveKey));
      const bool cfgLiveStatusConsumeConfirmed =
          (cfgLiveStatusKey && !statusKeyConsume.empty() &&
           statusKeyConsume == cfgLiveKey);
      const bool cfgLiveStatusTrusted =
          (cfgLiveStatusKey &&
           TextHook_IsTrustedObjectiveStatusCfgForKey(resolvedCfg, cfgLiveKey));
      bool cfgLiveStatusProbeConfirmed = false;
      if (cfgLiveStatusKey) {
        uint32_t cfgProbeMatchSlc = 0;
        uint32_t cfgDirectSlc = 0;
        uint32_t cfgDirectBias = 0;
        if (ObjRev_GetDirectCfgSlc(resolvedCfg, cfgDirectSlc, cfgDirectBias, false) &&
            cfgDirectSlc > 1) {
          std::string directStatusKey;
          if (TextHook_GetSlcKey(cfgDirectSlc, directStatusKey) &&
              ObjRev_LooksLikeLocalizationKey(directStatusKey.c_str()) &&
              directStatusKey == cfgLiveKey) {
            cfgProbeMatchSlc = cfgDirectSlc;
          }
        }
        if (cfgProbeMatchSlc == 0 && bridgeSlcIndex > 1) {
          std::string bridgeStatusKey;
          if (TextHook_GetSlcKey(bridgeSlcIndex, bridgeStatusKey) &&
              ObjRev_LooksLikeLocalizationKey(bridgeStatusKey.c_str()) &&
              bridgeStatusKey == cfgLiveKey) {
            cfgProbeMatchSlc = bridgeSlcIndex;
          }
        }
        std::string probeKeyByName;
        uint32_t probeSlcByName = 0;
        bool probeHasParamsByName = false;
        unsigned int probeCallerByName = 0;
        if (TextHook_GetRecentObjectiveStatusProbeByKeyFromConsumers(
                260, cfgLiveKey.c_str(), probeKeyByName, &probeSlcByName,
                &probeHasParamsByName, &probeCallerByName) &&
            probeHasParamsByName &&
            probeKeyByName == cfgLiveKey &&
            probeSlcByName > 1 &&
            cfgProbeMatchSlc > 1 &&
            probeSlcByName == cfgProbeMatchSlc) {
          cfgLiveStatusProbeConfirmed = true;
          TextHook_RecordObjectiveStatusCfgTrust(cfgLiveKey, resolvedCfg,
                                                 GetTickCount(),
                                                 "cfg_live_probe_match");
        }
      }
      if (cfgLiveStatusKey &&
          (cfgLiveStatusTrusted || cfgLiveStatusConsumeConfirmed ||
           cfgLiveStatusProbeConfirmed)) {
        if (IsObjectiveStatusMessageKeyName(cfgLiveKey)) {
          statusKeyCfg = cfgLiveKey;
        }
        if (!resolvedKey.empty() && ObjRev_IsObjectiveListKeyStrict(resolvedKey)) {
          static std::unordered_map<uint32_t, unsigned long>
              s_authStatusGuardLogTick;
          const unsigned long nowGuard = GetTickCount();
          auto itGuard = s_authStatusGuardLogTick.find(resolvedCfg);
          if (itGuard == s_authStatusGuardLogTick.end() ||
              (nowGuard - itGuard->second) > 1200) {
            s_authStatusGuardLogTick[resolvedCfg] = nowGuard;
            char gbuf[384];
            sprintf_s(gbuf,
                      "[AUTH-STATUS-CFG-GUARD] cfg=0x%X idx=%d fxB=%d authKey=%s cfgKey=%s",
                      resolvedCfg, i, eFxBirth, resolvedKey.c_str(),
                      cfgLiveKey.c_str());
            LogToFile(gbuf);
          }
        }
        // Status channel is independent from objective authority.
        // Do not clear objective bind here.
      } else if (cfgLiveStatusKey && !cfgLiveStatusTrusted) {
        static std::unordered_map<uint64_t, unsigned long>
            s_authStatusUntrustedLogTick;
        const unsigned long nowGuard = GetTickCount();
        const uint64_t guardSig =
            ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^
            (uint64_t)i;
        auto itGuard = s_authStatusUntrustedLogTick.find(guardSig);
        if (itGuard == s_authStatusUntrustedLogTick.end() ||
            (nowGuard - itGuard->second) > 1200) {
          s_authStatusUntrustedLogTick[guardSig] = nowGuard;
          char gbuf[384];
          sprintf_s(
              gbuf,
              "[AUTH-STATUS-CFG-UNTRUSTED] cfg=0x%X idx=%d fxB=%d cfgKey=%s",
              resolvedCfg, i, eFxBirth, cfgLiveKey.c_str());
          LogToFile(gbuf);
        }
      }
    }
    const int laneSlotStrict = (int)lroundf((ey - 10.0f) / 20.0f);
    const bool strictToplaneStatusProbeActive =
        (isObjLane && laneSlotStrict == 0 && frameStatusProbeStrictActive &&
         IsObjectiveStatusMessageKeyName(recentStatusProbeKeyStrict));
    const bool strictToplaneStatusProbeCorroborated =
        (strictToplaneStatusProbeActive &&
         ((!statusKeyResolved.empty() &&
           statusKeyResolved == recentStatusProbeKeyStrict) ||
          (!statusKeyConsume.empty() &&
           statusKeyConsume == recentStatusProbeKeyStrict) ||
          (!statusKeyBridge.empty() &&
           statusKeyBridge == recentStatusProbeKeyStrict) ||
          (!statusKeyCfg.empty() && statusKeyCfg == recentStatusProbeKeyStrict) ||
          (!cfgLiveKeyEarlyCheck.empty() &&
           IsObjectiveStatusMessageKeyName(cfgLiveKeyEarlyCheck) &&
           cfgLiveKeyEarlyCheck == recentStatusProbeKeyStrict &&
           TextHook_IsTrustedObjectiveStatusCfgForKey(resolvedCfg,
                                                      cfgLiveKeyEarlyCheck))));
    const bool lockObjectiveListAuthority =
        (isObjLane && laneSlotStrict >= 1 && hasAuthoritativeBind &&
         !resolvedKey.empty() &&
         ObjRev_LooksLikeLocalizationKey(resolvedKey.c_str()) &&
         ObjRev_IsObjectiveListKeyStrict(resolvedKey) &&
         !IsObjectiveStatusMessageKeyName(resolvedKey));

    // Legacy live-lane rescue is retired.
    // Objective/status ownership must come from owner-resolved OSAUTH captures,
    // not from per-frame cfg/probe/observation reconstruction inside the
    // HudElem poller.

    if (!lockObjectiveListAuthority) {
      if (!statusKeyResolved.empty()) {
        statusLaneKey = statusKeyResolved;
      } else if (!statusKeyConsume.empty()) {
        statusLaneKey = statusKeyConsume;
      } else if (!statusKeyBridge.empty()) {
        statusLaneKey = statusKeyBridge;
      } else if (!statusKeyCfg.empty()) {
        statusLaneKey = statusKeyCfg;
      } else if (!statusKeyUntrusted.empty()) {
        static std::unordered_map<uint64_t, unsigned long>
            s_statusLaneUntrustedLogTick;
        const unsigned long nowStatus = GetTickCount();
        const uint64_t statusSig =
            ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^
            (uint64_t)i;
        auto itStatus = s_statusLaneUntrustedLogTick.find(statusSig);
        if (itStatus == s_statusLaneUntrustedLogTick.end() ||
            (nowStatus - itStatus->second) > 1200) {
          s_statusLaneUntrustedLogTick[statusSig] = nowStatus;
          char sbuf[320];
          sprintf_s(sbuf,
                    "[STATUS-LANE-UNTRUSTED] cfg=0x%X idx=%d fxB=%d key=%s",
                    resolvedCfg, i, eFxBirth, statusKeyUntrusted.c_str());
          LogToFile(sbuf);
        }
      }
    } else if (!statusKeyResolved.empty() || !statusKeyConsume.empty() ||
               !statusKeyBridge.empty() || !statusKeyCfg.empty() ||
               !statusKeyUntrusted.empty()) {
      static std::unordered_map<uint64_t, unsigned long>
          s_statusLaneSuppressedLogTick;
      const unsigned long nowStatus = GetTickCount();
      const uint64_t statusSig =
          ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^
          (uint64_t)i;
      auto itStatus = s_statusLaneSuppressedLogTick.find(statusSig);
      if (itStatus == s_statusLaneSuppressedLogTick.end() ||
          (nowStatus - itStatus->second) > 1200) {
        s_statusLaneSuppressedLogTick[statusSig] = nowStatus;
        char sbuf[448];
        sprintf_s(sbuf,
                  "[STATUS-LANE-SUPPRESS-AUTHOBJ] cfg=0x%X idx=%d fxB=%d auth=%s resolved=%s consume=%s bridge=%s cfg=%s untrusted=%s",
                  resolvedCfg, i, eFxBirth, resolvedKey.c_str(),
                  statusKeyResolved.empty() ? "(none)" : statusKeyResolved.c_str(),
                  statusKeyConsume.empty() ? "(none)" : statusKeyConsume.c_str(),
                  statusKeyBridge.empty() ? "(none)" : statusKeyBridge.c_str(),
                  statusKeyCfg.empty() ? "(none)" : statusKeyCfg.c_str(),
                  statusKeyUntrusted.empty() ? "(none)" : statusKeyUntrusted.c_str());
        LogToFile(sbuf);
      }
    }

    const bool laneLooksStatus =
        (!statusKeyResolved.empty() || !statusKeyBridge.empty() ||
         !statusKeyCfg.empty() || !statusLaneKey.empty());
    bool probeMatchesStatusLane = false;
    if (isObjLane && laneLooksStatus && hasRecentStatusProbe &&
        IsObjectiveStatusMessageKeyName(recentStatusProbeKey)) {
      if ((!statusKeyResolved.empty() &&
           statusKeyResolved == recentStatusProbeKey) ||
          (!statusKeyBridge.empty() &&
           statusKeyBridge == recentStatusProbeKey) ||
          (!statusKeyCfg.empty() && statusKeyCfg == recentStatusProbeKey) ||
          (bridgeSlcIndex > 1 && recentStatusProbeSlc > 1 &&
           bridgeSlcIndex == recentStatusProbeSlc)) {
        probeMatchesStatusLane = true;
      }
    }
    if (probeMatchesStatusLane) {
      const std::string preOverrideKey = statusLaneKey;
      statusLaneKey = recentStatusProbeKey;
      if (preOverrideKey != statusLaneKey) {
        static std::unordered_map<std::string, unsigned long>
            s_statusEdgeKeyLogTick;
        const unsigned long nowKey = GetTickCount();
        std::string logSig = std::to_string((unsigned int)resolvedCfg) + "|" +
                             std::to_string(i) + "|" +
                             std::to_string((unsigned int)eFxBirth);
        auto itLog = s_statusEdgeKeyLogTick.find(logSig);
        if (itLog == s_statusEdgeKeyLogTick.end() ||
            (nowKey - itLog->second) > 600) {
          s_statusEdgeKeyLogTick[logSig] = nowKey;
          char kbuf[448];
          sprintf_s(
              kbuf,
              "[STATUS-EDGE-KEY] cfg=0x%X idx=%d fxB=%d chosen=%s pre=%s resolved=%s bridge=%s cfg=%s probe=%s probeCaller=+0x%X probeSlc=0x%X rp=%d",
              resolvedCfg, i, eFxBirth, statusLaneKey.c_str(),
              preOverrideKey.empty() ? "(none)" : preOverrideKey.c_str(),
              statusKeyResolved.empty() ? "(none)" : statusKeyResolved.c_str(),
              statusKeyBridge.empty() ? "(none)" : statusKeyBridge.c_str(),
              statusKeyCfg.empty() ? "(none)" : statusKeyCfg.c_str(),
              recentStatusProbeKey.c_str(), recentStatusProbeCaller,
              (unsigned int)recentStatusProbeSlc,
              recentStatusProbeHasParams ? 1 : 0);
          LogToFile(kbuf);
        }
      }
    }

    if (strictToplaneStatusProbeCorroborated && !resolvedKey.empty() &&
        ObjRev_IsObjectiveListKeyStrict(resolvedKey) &&
        !IsObjectiveStatusMessageKeyName(resolvedKey) &&
        statusLaneKey.empty()) {
      static std::unordered_map<uint64_t, unsigned long>
          s_strictTopProbeSuppressLogTick;
      const unsigned long nowStrict = GetTickCount();
      const uint64_t strictSig =
          ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^
          (uint64_t)i;
      auto itStrict = s_strictTopProbeSuppressLogTick.find(strictSig);
      if (itStrict == s_strictTopProbeSuppressLogTick.end() ||
          (nowStrict - itStrict->second) > 1200) {
        s_strictTopProbeSuppressLogTick[strictSig] = nowStrict;
        char sbuf[448];
        sprintf_s(
            sbuf,
            "[AUTH-STRICT-PROBE-TOPLANE-SUPPRESS] cfg=0x%X idx=%d fxB=%d lane=%d stale=%s probeKey=%s",
            resolvedCfg, i, eFxBirth, laneSlotStrict, resolvedKey.c_str(),
            recentStatusProbeKeyStrict.c_str());
        LogToFile(sbuf);
      }
      // Status channel is independent from objective authority.
      // Keep objective bind untouched.
      statusLaneKey = recentStatusProbeKeyStrict;
    }

    // Probe-based status override: when a confirmed status-consumer probe
    // detects a status key (EXE_GAMESAVED, etc.) but the authority cache still
    // holds an objective list key, the authority is stale.  Invalidate it so
    // the status rendering pipeline can take over.
    //
    // This fires when:
    //  (a) cfgLiveKey is empty (direct miss — original condition), OR
    //  (b) cfgLiveKey itself is a status key (AUTH-LIVE-DRIFT cleared
    //      authority, but obs_ordinal re-assigned a stale objective key)
    if (isObjLane && !statusLaneKey.empty() &&
        IsObjectiveStatusMessageKeyName(statusLaneKey) &&
        !resolvedKey.empty() && ObjRev_IsObjectiveListKeyStrict(resolvedKey) &&
        (!cfgLiveKeyEarlyChecked || cfgLiveKeyEarlyCheck.empty() ||
         (IsObjectiveStatusMessageKeyName(cfgLiveKeyEarlyCheck) &&
          TextHook_IsTrustedObjectiveStatusCfgForKey(resolvedCfg,
                                                     cfgLiveKeyEarlyCheck)))) {
      static std::unordered_map<uint64_t, unsigned long> s_probeOverrideLogTick;
      const unsigned long nowPO = GetTickCount();
      const uint64_t poSig =
          ((uint64_t)resolvedCfg << 32) ^ ((uint64_t)eFxBirth << 8) ^ (uint64_t)i;
      auto itPO = s_probeOverrideLogTick.find(poSig);
      if (itPO == s_probeOverrideLogTick.end() ||
          (nowPO - itPO->second) > 1200) {
        s_probeOverrideLogTick[poSig] = nowPO;
        char pobuf[384];
        sprintf_s(pobuf,
                  "[AUTH-PROBE-OVERRIDE] cfg=0x%X idx=%d fxB=%d staleAuth=%s "
                  "probeStatus=%s -> clear authority",
                  resolvedCfg, i, eFxBirth, resolvedKey.c_str(),
                  statusLaneKey.c_str());
        LogToFile(pobuf);
      }
      // Status channel is independent from objective authority.
      // Keep objective bind untouched.
    }

    bool validKey =
        (!resolvedKey.empty() && ObjRev_LooksLikeLocalizationKey(resolvedKey.c_str()));
    const float alpha = (float)((ecolor >> 24) & 0xFF) / 255.0f;

    if (isObjLane) {
      if (validKey) {
        auto itLast = s_cfgToLastKey.find(resolvedCfg);
        if (itLast == s_cfgToLastKey.end() || itLast->second != resolvedKey) {
          s_cfgToLastKey[resolvedCfg] = resolvedKey;
          char kbuf[320];
          sprintf_s(kbuf,
                    "[OBJ-NATIVE-KEYMAP] cfg=0x%X(label=0x%X text=0x%X) key=%s "
                    "idx=%d x=%.1f y=%.1f",
                    resolvedCfg, labelSlc, textSlc, resolvedKey.c_str(), i, ex, ey);
          LogToFile(kbuf);
        }
        static std::unordered_map<std::string, unsigned long> s_authJoinLogTick;
        std::string joinSig = std::to_string((unsigned int)resolvedCfg) + "|" +
                              std::to_string(authElemIndex) + "|" +
                              std::to_string(eFxBirth);
        auto itJoin = s_authJoinLogTick.find(joinSig);
        if (itJoin == s_authJoinLogTick.end() ||
            (GetTickCount() - itJoin->second) > 1200) {
          s_authJoinLogTick[joinSig] = GetTickCount();
          char abuf[384];
          sprintf_s(abuf,
                    "[AUTH-JOIN-OK] cfg=0x%X idx=%d fxB=%d src=%s key=%s",
                    resolvedCfg, authElemIndex, eFxBirth, authSource,
                    resolvedKey.c_str());
          LogToFile(abuf);
        }
        // Propagate native fontScale+alpha to HUDHINT synth pipeline
        // (objective-lane keys like FLOOD_STEALTH_ATTACK, INVULERABLE_*).
        if (efs > 0.1f && efs < 10.0f) {
          std::lock_guard<std::mutex> lk(g_NativeCfgFontScaleMutex);
          g_NativeCfgFontScaleByKey[resolvedKey] = efs;
        }
        {
          // Always propagate the current alpha so hidden HudElems
          // clear the latch left from a previous visible state.
          std::lock_guard<std::mutex> lk(g_NativeCfgAlphaMutex);
          g_NativeCfgAlphaByKey[resolvedKey] = alpha;
        }
        // Propagate fade parameters for native pulsing reproduction.
        {
          uint32_t eFColor = 0;
          int eFStart = 0, eFTime = 0;
          ObjRev_ReadU32(elem + 0x34, eFColor);
          ObjRev_ReadI32(elem + 0x38, eFStart);
          ObjRev_ReadI32(elem + 0x3C, eFTime);
          float fromA = (float)((eFColor >> 24) & 0xFF) / 255.0f;
          float toA = alpha;
          TextHook_SetNativeFadeParams(resolvedKey, fromA, toA,
                                        eFStart, eFTime,
                                        effectiveServerTime);
        }
      } else {
        unsigned long nowTick = GetTickCount();
        auto itNoKey = s_cfgNoKeyLogTick.find(resolvedCfg);
        if (itNoKey == s_cfgNoKeyLogTick.end() ||
            (nowTick - itNoKey->second) > 1200) {
          char sbuf[320];
          bool shouldLogNoKey = true;
          const bool statusLaneWithoutObjective =
              (statusLaneKey.empty() ||
               !IsObjectiveStatusMessageKeyName(statusLaneKey));
          if (kRequireAuthoritativeBind && !hasAuthoritativeBind &&
              statusLaneWithoutObjective) {
            const bool nonLaneNotificationLike =
                (ey <= -20.0f && eFxBirth <= 0 &&
                 (eFlags == 0x7 || efs >= 1.60f));
            if (nonLaneNotificationLike) {
              sprintf_s(
                  sbuf,
                  "[OBJ-NATIVE-SKIP-NONLANE] cfgLabel=0x%X cfgText=0x%X idx=%d "
                  "x=%.1f y=%.1f fs=%.2f st=%d fxB=%d fxL=%d",
                  labelSlc, textSlc, i, ex, ey, efs, effectiveServerTime,
                  eFxBirth, eFxLetter);
            } else {
              // NOBIND can transiently occur while authority producer and live
              // lane instance converge. Count as unresolved only after a
              // short grace period and outside status-heavy windows.
              bool suppressNoBindLog = false;
              const uint64_t noBindSig =
                  ((uint64_t)resolvedCfg << 32) ^
                  ((uint64_t)(uint32_t)eFxBirth << 8) ^
                  (uint64_t)(uint8_t)(authElemIndex & 0xFF);
              static std::unordered_map<uint64_t, unsigned long>
                  s_noBindFirstSeenTick;
              auto itFirstNoBind = s_noBindFirstSeenTick.find(noBindSig);
              if (itFirstNoBind == s_noBindFirstSeenTick.end()) {
                s_noBindFirstSeenTick[noBindSig] = nowTick;
                suppressNoBindLog = true;
              } else if (nowTick >= itFirstNoBind->second &&
                         (nowTick - itFirstNoBind->second) < 30000) {
                suppressNoBindLog = true;
              }
              if (s_noBindFirstSeenTick.size() > 8192) {
                for (auto itf = s_noBindFirstSeenTick.begin();
                     itf != s_noBindFirstSeenTick.end();) {
                  if (nowTick >= itf->second &&
                      (nowTick - itf->second) > 120000) {
                    itf = s_noBindFirstSeenTick.erase(itf);
                  } else {
                    ++itf;
                  }
                }
              }

              if (!suppressNoBindLog) {
                bool recentStatusEvent = false;
                std::vector<ObjectiveStatusEvent> statusEvents =
                    TextHook_GetObjectiveStatusEvents();
                for (const auto &ev : statusEvents) {
                  if (ev.key.empty()) {
                    continue;
                  }
                  const unsigned long ageMs =
                      (nowTick >= ev.triggerTick)
                          ? (nowTick - ev.triggerTick)
                          : (ev.triggerTick - nowTick);
                  if (ageMs <= 3500) {
                    recentStatusEvent = true;
                    break;
                  }
                }
                const bool listLaneBand =
                    (ey >= -5.0f && ey <= 70.0f && efs >= 1.10f &&
                     efs <= 1.40f);
                if (recentStatusEvent && listLaneBand) {
                  suppressNoBindLog = true;
                }
              }

              if (suppressNoBindLog) {
                shouldLogNoKey = false;
              } else {
                sprintf_s(
                    sbuf,
                    "[OBJ-NATIVE-SKIP-NOBIND] cfgLabel=0x%X cfgText=0x%X idx=%d "
                    "x=%.1f y=%.1f fs=%.2f st=%d fxB=%d fxL=%d",
                    labelSlc, textSlc, i, ex, ey, efs, effectiveServerTime,
                    eFxBirth, eFxLetter);
              }
            }
          } else if (!statusLaneWithoutObjective) {
            sprintf_s(sbuf,
                      "[OBJ-NATIVE-SKIP-STATUSKEY] cfgLabel=0x%X cfgText=0x%X idx=%d "
                      "x=%.1f y=%.1f key=%s",
                      labelSlc, textSlc, i, ex, ey, statusLaneKey.c_str());
          } else {
            sprintf_s(sbuf,
                      "[OBJ-NATIVE-SKIP-NOKEY] cfgLabel=0x%X cfgText=0x%X idx=%d "
                      "x=%.1f y=%.1f fs=%.2f st=%d fxB=%d fxL=%d",
                      labelSlc, textSlc, i, ex, ey, efs, effectiveServerTime,
                      eFxBirth, eFxLetter);
          }
          if (shouldLogNoKey) {
            s_cfgNoKeyLogTick[resolvedCfg] = nowTick;
            LogToFile(sbuf);
          }
        }
      }
    } else if (resolvedKey.empty()) {
      // Hint lanes are resolved from the reversed native struct/cfg path only.
      // Do not use SLC runtime key-cache or raw SLC payload here.
      if (resolvedCfg != 0) {
        resolvedKey = ObjRev_ResolveConfigKey(resolvedCfg, false, true);
        if (resolvedKey.empty()) {
          resolvedKey = ObjRev_ResolveConfigKey(resolvedCfg, false, false);
        }
      }
      validKey = (!resolvedKey.empty() &&
                  ObjRev_LooksLikeLocalizationKey(resolvedKey.c_str()));
      if (validKey) {
        static std::unordered_map<uint64_t, unsigned long> s_hintCfgRecoverLogTick;
        const unsigned long nowRecover = GetTickCount();
        const uint64_t sig =
            ((uint64_t)(resolvedCfg & 0xFFFFu) << 32) ^
            (uint64_t)(std::hash<std::string>{}(resolvedKey) & 0xFFFFFFFFull);
        auto itRecover = s_hintCfgRecoverLogTick.find(sig);
        if (itRecover == s_hintCfgRecoverLogTick.end() ||
            (nowRecover - itRecover->second) > 1600) {
          s_hintCfgRecoverLogTick[sig] = nowRecover;
          char rbuf[320];
          sprintf_s(
              rbuf,
              "[HUD-NATIVE-HINT-CFG] cfg=0x%X key=%s idx=%d x=%.1f y=%.1f fs=%.2f",
              resolvedCfg, resolvedKey.c_str(), i, ex, ey, efs);
          LogToFile(rbuf);
        }
        // Propagate native fontScale to HUDHINT synth pipeline.
        if (efs > 0.1f && efs < 10.0f) {
          std::lock_guard<std::mutex> lk(g_NativeCfgFontScaleMutex);
          g_NativeCfgFontScaleByKey[resolvedKey] = efs;
        }
        // Propagate native alpha to HUDHINT synth pipeline.
        // Always write (including 0) so hidden HudElems clear the
        // latch left from a previous visible state.
        {
          std::lock_guard<std::mutex> lk(g_NativeCfgAlphaMutex);
          g_NativeCfgAlphaByKey[resolvedKey] = alpha;
        }
        // Propagate fade parameters for native pulsing reproduction.
        {
          uint32_t eFColor = 0;
          int eFStart = 0, eFTime = 0;
          ObjRev_ReadU32(elem + 0x34, eFColor);
          ObjRev_ReadI32(elem + 0x38, eFStart);
          ObjRev_ReadI32(elem + 0x3C, eFTime);
          float fromA = (float)((eFColor >> 24) & 0xFF) / 255.0f;
          float toA = alpha;
          TextHook_SetNativeFadeParams(resolvedKey, fromA, toA,
                                        eFStart, eFTime,
                                        effectiveServerTime);
        }
      }
    } else if (validKey) {
      // Hint lane with pre-resolved key — propagate native params
      // so TextHook_GetNativeFadeAlpha() can return interpolated alpha
      // for pulse reproduction.  Without this, hint-lane keys whose
      // resolvedKey was already set skip the recovery branch above and
      // never call SetNativeFadeParams, causing GetNativeFadeAlpha()
      // to return -1 → flat synth alpha → no pulse.
      if (efs > 0.1f && efs < 10.0f) {
        std::lock_guard<std::mutex> lk(g_NativeCfgFontScaleMutex);
        g_NativeCfgFontScaleByKey[resolvedKey] = efs;
      }
      {
        std::lock_guard<std::mutex> lk(g_NativeCfgAlphaMutex);
        g_NativeCfgAlphaByKey[resolvedKey] = alpha;
      }
      {
        uint32_t eFColor = 0;
        int eFStart = 0, eFTime = 0;
        ObjRev_ReadU32(elem + 0x34, eFColor);
        ObjRev_ReadI32(elem + 0x38, eFStart);
        ObjRev_ReadI32(elem + 0x3C, eFTime);
        float fromA = (float)((eFColor >> 24) & 0xFF) / 255.0f;
        float toA = alpha;
        TextHook_SetNativeFadeParams(resolvedKey, fromA, toA,
                                      eFStart, eFTime,
                                      effectiveServerTime);
      }
    }

    std::string nativeLaneKey = resolvedKey;
    bool nativeLaneValidKey = validKey;
    const ObjectiveInstanceId nativeInstanceId =
        TextHook_OSAuth_MakeInstanceId(resolvedCfg, eFxBirth,
                                       (uint64_t)elem);
    const uint64_t nativeInstanceSig =
        TextHook_OSAuth_MakeInstanceKey(nativeInstanceId);

    // OSAUTH bridge: when NativeCapture authority fails, query the OSAUTH
    // objective text map which is populated from the SLC hook producer path.
    // This bridges the gap between the two authority systems.
    // Skip if statusLaneKey indicates this cfg is currently a status slot.
    const bool bridgeBlockedByStatusLane =
        (!statusLaneKey.empty() && IsObjectiveStatusMessageKeyName(statusLaneKey));
    if (isObjLane && !nativeLaneValidKey && nativeInstanceSig != 0 &&
        !bridgeBlockedByStatusLane) {
      std::lock_guard<std::mutex> osLock(g_OSAuthSnapshotMutex);
      auto itOsAuth = g_OSAuthObjectiveTextByInstance.find(nativeInstanceSig);
      if (itOsAuth != g_OSAuthObjectiveTextByInstance.end() &&
          !itOsAuth->second.key.empty() &&
          ObjRev_LooksLikeLocalizationKey(itOsAuth->second.key.c_str()) &&
          !IsObjectiveStatusMessageKeyName(itOsAuth->second.key)) {
        nativeLaneKey = itOsAuth->second.key;
        nativeLaneValidKey = true;
        static std::unordered_map<uint64_t, unsigned long> s_osAuthBridgeLogTick;
        const unsigned long nowBridge = GetTickCount();
        auto itLog = s_osAuthBridgeLogTick.find(nativeInstanceSig);
        if (itLog == s_osAuthBridgeLogTick.end() ||
            (nowBridge - itLog->second) > 1200) {
          s_osAuthBridgeLogTick[nativeInstanceSig] = nowBridge;
          char bbuf[384];
          sprintf_s(bbuf,
                    "[OSAUTH-BRIDGE] cfg=0x%X fxB=%d idx=%d isig=0x%llX key=%s",
                    resolvedCfg, eFxBirth, authElemIndex,
                    (unsigned long long)nativeInstanceSig,
                    nativeLaneKey.c_str());
          LogToFile(bbuf);
        }
      }
    }

    // STATUS LANE BRIDGE: When this cfg is a status slot, assign the status
    // key directly so the unified renderer can use live HudElem position/alpha.
    // This eliminates ghost subtitles (no HudElem → no render) and ensures
    // Korean status text appears at the exact same position as English.
    if (isObjLane && !nativeLaneValidKey && bridgeBlockedByStatusLane &&
        !statusLaneKey.empty()) {
      nativeLaneKey = statusLaneKey;
      nativeLaneValidKey = true;
      static std::unordered_map<uint32_t, unsigned long> s_statusBridgeLogTick;
      const unsigned long nowSB = GetTickCount();
      auto itSBLog = s_statusBridgeLogTick.find(resolvedCfg);
      if (itSBLog == s_statusBridgeLogTick.end() ||
          (nowSB - itSBLog->second) > 1200) {
        s_statusBridgeLogTick[resolvedCfg] = nowSB;
        char sbuf[384];
        sprintf_s(sbuf,
                  "[STATUS-BRIDGE] cfg=0x%X fxB=%d idx=%d key=%s alpha=%.2f y=%.1f",
                  resolvedCfg, eFxBirth, authElemIndex,
                  statusLaneKey.c_str(), alpha, ey);
        LogToFile(sbuf);
      }
    }

    // OBJ-BOOT-OBS override: when the resolved key is a HINT sitting in an
    // objective lane, replace it with the actual objective key observed by the
    // SLC hook (structured payload at +0x204816 / CG_DrawHudElem label).
    // This fixes missions like SHIP_GRAVEYARD where the HudElem text field
    // contains a hint (HINT_WELD) while the real objective (OBJ_2) is
    // delivered through a separate path.
    // Safety: only triggers for HUD_KEY_HINT — all other classifications
    // (including fallback HUD_KEY_OBJ_HUD_HINT) are left untouched.
    if (isObjLane && nativeLaneValidKey && !nativeLaneKey.empty()) {
      const HudKeyType nativeLaneKt = ClassifyHudKey(nativeLaneKey);
      if (nativeLaneKt == HUD_KEY_HINT) {
        auto recentObjObs = TextHook_GetRecentObjectiveKeys(2500);
        // Pick the most recent HUD_KEY_OBJECTIVE_LIST observation.
        const ObjectiveKeyObservation *bestObs = nullptr;
        for (const auto &obs : recentObjObs) {
          if (ClassifyHudKey(obs.key) == HUD_KEY_OBJECTIVE_LIST) {
            if (!bestObs || obs.lastSeen > bestObs->lastSeen) {
              bestObs = &obs;
            }
          }
        }
        if (bestObs) {
          static std::unordered_map<uint32_t, unsigned long> s_bootObsLogTick;
          const unsigned long nowBO = GetTickCount();
          auto itBO = s_bootObsLogTick.find(resolvedCfg);
          if (itBO == s_bootObsLogTick.end() ||
              (nowBO - itBO->second) > 1200) {
            s_bootObsLogTick[resolvedCfg] = nowBO;
            char bobuf[384];
            sprintf_s(
                bobuf,
                "[OBJ-BOOT-OVERRIDE] cfg=0x%X idx=%d hint=%s -> obj=%s "
                "caller=+0x%X age=%lums",
                resolvedCfg, authElemIndex, nativeLaneKey.c_str(),
                bestObs->key.c_str(), bestObs->callerOffset,
                (unsigned long)(nowBO - bestObs->lastSeen));
            LogToFile(bobuf);
          }
          nativeLaneKey = bestObs->key;
          nativeLaneValidKey = true;
        } else {
          // No recent objective observation — suppress the hint from the
          // objective pipeline entirely so it doesn't appear as a false
          // positive in the objective overlay.
          nativeLaneValidKey = false;
          nativeLaneKey.clear();
        }
      }
    }

    LiveHudElem le{};
    le.key = !resolvedKey.empty() ? resolvedKey : statusLaneKey;
    le.slcIndex = bridgeSlcIndex;
    le.cfgIndex = resolvedCfg;
    le.arrayIndex = isObjLane ? authElemIndex : i;
    le.rawPtr = elem;
    le.instanceSig = nativeInstanceSig;
    le.rawLabelSlc = labelSlc;
    le.rawTextSlc = textSlc;
    le.objectiveLane = isObjLane;
    le.hintBridgeLane = (isHintBridgeLane && !isObjLane);
    le.x = ex;
    le.y = ey;
    le.fontScale = efs;
    le.color = ecolor;
    le.fromColor = efromColor;
    le.fadeStartTime = eFadeStart;
    le.fadeTime = eFadeTime;
    le.fxBirthTime = eFxBirth;
    le.fxLetterTime = eFxLetter;
    le.flags = eFlags;
    le.alpha = alpha;
    le.active = true;
    newElems.push_back(le);

    if (isObjLane) {
      if ((alpha > 0.03f || eFxBirth > 0) && std::isfinite(ex) &&
          std::isfinite(ey)) {
        hasObjectiveLaneActivity = true;
      }
      NativeObjectiveItem item{};
      item.arrayIndex = authElemIndex;
      item.rawPtr = elem;
      item.instanceSig = nativeInstanceSig;
      item.cfgIndex = resolvedCfg;
      item.key = nativeLaneKey;
      item.x = ex;
      item.y = ey;
      item.fontScale = efs;
      item.colorPacked = ecolor;
      item.alpha = alpha;
      item.serverTime = effectiveServerTime;
      item.fxBirthTime = eFxBirth;
      item.fxLetterTime = eFxLetter;
      item.flags = eFlags;
      item.validKey = nativeLaneValidKey;
      nativeItems.push_back(std::move(item));

      ObjectiveChannel chHint = OBJ_CHANNEL_NONE;
      if (validKey) {
        chHint = DetermineObjectiveChannelForKey(resolvedKey, false);
      }
      if (chHint == OBJ_CHANNEL_NONE) {
        chHint = (ey <= -20.0f) ? OBJ_CHANNEL_GAMEPLAY_UPDATE
                                : OBJ_CHANNEL_GAMEPLAY_LIST;
      }
      OverlayDrawEvent evt{};
      evt.frameId = 0;
      evt.tick = GetTickCount();
      evt.source = OVERLAY_SOURCE_HUDELEM_REVERSE;
      evt.callerOffset = 0;
      evt.key = resolvedKey;
      evt.english = resolvedKey;
      evt.x = ex;
      evt.y = ey;
      evt.sx = efs;
      evt.sy = efs;
      evt.fontH = 35.0f;
      evt.alpha = alpha;
      evt.style = 0;
      evt.virtualW = 0;
      evt.virtualH = 0;
      evt.flags =
          (((unsigned int)((unsigned short)i)) << 16) | ((unsigned int)eFlags & 0xFFFFu);
      evt.fxBirth = eFxBirth;
      evt.fxLetter = eFxLetter;
      evt.serverTime = effectiveServerTime;
      evt.channelHint = chHint;
      if (validKey) {
        TextHook_PublishOverlayEvent(evt);
      }
    }
  }

  {
    const std::vector<RecentHudHintRuntimeSlot> recentSlots =
        TextHook_GetRecentHudHintRuntimeSlots(2200);
    const size_t totalRuntimeSlots = TextHook_GetHudHintRuntimeSlotCount();
    static std::unordered_map<uint64_t, unsigned long> s_dynHintLogTick;
    static std::unordered_map<uint64_t, unsigned long> s_dynHintMissLogTick;
    static unsigned long s_dynCacheLogTick = 0;
    const unsigned long nowDynCache = GetTickCount();
    if ((nowDynCache - s_dynCacheLogTick) >
        ((recentSlots.empty() && totalRuntimeSlots == 0) ? 1500u : 800u)) {
      s_dynCacheLogTick = nowDynCache;
      char cbuf[160];
      sprintf_s(cbuf, "[NHUD-DYN-CACHE] recent=%zu total=%zu",
                recentSlots.size(), totalRuntimeSlots);
      LogToFile(cbuf);
    }
    std::unordered_set<uintptr_t> dynElemDedup;
    dynElemDedup.reserve(recentSlots.size() * 24 + 8);
    for (const RecentHudHintRuntimeSlot &slotMeta : recentSlots) {
      bool matchedAny = false;
      bool bestGeomFound = false;
      int bestGeomDelta = 0;
      float bestGeomX = 0.0f, bestGeomY = 0.0f, bestGeomFs = 0.0f;
      int bestGeomFlags = 0;
      int bestGeomType = 0;
      uint32_t bestGeomLabel = 0, bestGeomText = 0;
      int bestGeomFade = 0, bestGeomFxBirth = 0;
      float bestGeomAlpha = 0.0f;

      for (int delta = -0x80; delta <= 0x180; delta += 4) {
        const uintptr_t elem = slotMeta.base + (intptr_t)delta;
        if (!(elem > 0x10000 && elem < 0x7FFFFFFFFFFF)) {
          continue;
        }
        if (!dynElemDedup.insert(elem).second) {
          continue;
        }
        if (!IsSafeRead((void *)elem, 0xA8)) {
          continue;
        }

        const int existingIndex = ObjRev_ResolveHudElemArrayIndex(elem);

        uint32_t t = 0;
        if (!ObjRev_ReadU32(elem, t) || t == 0 || t > 13) {
          continue;
        }

        float ex = 0.0f, ey = 0.0f, efs = 0.0f;
        int eFlags = 0;
        if (!ObjRev_ReadF32(elem + 0x04, ex) ||
            !ObjRev_ReadF32(elem + 0x08, ey) ||
            !ObjRev_ReadF32(elem + 0x14, efs) ||
            !ObjRev_ReadI32(elem + 0xA4, eFlags)) {
          continue;
        }
        if (!std::isfinite(ex) || !std::isfinite(ey) || !std::isfinite(efs)) {
          continue;
        }
        if (efs < 0.45f || efs > 4.50f) {
          continue;
        }
        if (ey < -140.0f || ey > 540.0f) {
          continue;
        }

        const int slotBucket = TextHook_HudSlotFromEvent(ey, 480, 0);
        if (slotBucket <= 0) {
          continue;
        }

        uint32_t labelSlc = 0, textSlc = 0;
        uint32_t ecolor = 0, efromColor = 0;
        int eFadeStart = 0, eFadeTime = 0, eFxBirth = 0, eFxLetter = 0;
        if (!ObjRev_ReadU32(elem + 0x80, labelSlc) ||
            !ObjRev_ReadU32(elem + 0x84, textSlc) ||
            !ObjRev_ReadU32(elem + 0x30, ecolor) ||
            !ObjRev_ReadU32(elem + 0x34, efromColor) ||
            !ObjRev_ReadI32(elem + 0x38, eFadeStart) ||
            !ObjRev_ReadI32(elem + 0x3C, eFadeTime) ||
            !ObjRev_ReadI32(elem + 0x90, eFxBirth) ||
            !ObjRev_ReadI32(elem + 0x94, eFxLetter)) {
          continue;
        }

        const float alpha = (float)((ecolor >> 24) & 0xFFu) / 255.0f;
        if (!bestGeomFound ||
            ((slotBucket == 3) && (std::fabs(ey - 240.0f) <
                                   std::fabs(bestGeomY - 240.0f)))) {
          bestGeomFound = true;
          bestGeomDelta = delta;
          bestGeomX = ex;
          bestGeomY = ey;
          bestGeomFs = efs;
          bestGeomFlags = eFlags;
          bestGeomType = (int)t;
          bestGeomLabel = labelSlc;
          bestGeomText = textSlc;
          bestGeomFade = eFadeTime;
          bestGeomFxBirth = eFxBirth;
          bestGeomAlpha = alpha;
        }

        uint32_t resolvedCfg = 0;
        uint32_t bridgeSlcIndex = 0;
        if (ObjRev_IsPlausibleConfigIndex(textSlc)) {
          resolvedCfg = textSlc;
        } else if (ObjRev_IsPlausibleConfigIndex(labelSlc)) {
          resolvedCfg = labelSlc;
        }
        if (resolvedCfg != 0) {
          uint32_t directSlc = 0;
          uint32_t directBias = 0;
          if (ObjRev_GetDirectCfgSlc(resolvedCfg, directSlc, directBias, false) &&
              directSlc > 1) {
            bridgeSlcIndex = directSlc;
          } else {
            bridgeSlcIndex = resolvedCfg;
          }
        }
        if (bridgeSlcIndex == 0) {
          if (textSlc > 1 && textSlc < 0x40000u) {
            bridgeSlcIndex = textSlc;
          } else if (labelSlc > 1 && labelSlc < 0x40000u) {
            bridgeSlcIndex = labelSlc;
          }
        }
        if (bridgeSlcIndex == 0) {
          continue;
        }

        std::string resolvedKey;
        if (bridgeSlcIndex > 1) {
          std::string slcKey;
          if (TextHook_GetSlcKey(bridgeSlcIndex, slcKey) &&
              ObjRev_LooksLikeLocalizationKey(slcKey.c_str())) {
            resolvedKey = slcKey;
          }
        }
        if (resolvedKey.empty() && resolvedCfg != 0) {
          resolvedKey = ObjRev_ResolveConfigKey(resolvedCfg, false, true);
          if (resolvedKey.empty()) {
            resolvedKey = ObjRev_ResolveConfigKey(resolvedCfg, false, false);
          }
        }
        if (resolvedKey.empty() ||
            !ObjRev_LooksLikeLocalizationKey(resolvedKey.c_str()) ||
            IsObjectiveOverlayKeyName(resolvedKey) ||
            IsObjectiveStatusMessageKeyName(resolvedKey)) {
          continue;
        }

        if (alpha <= 0.0f && eFxBirth <= 0 && eFadeTime <= 0) {
          continue;
        }

        matchedAny = true;
        LiveHudElem le{};
        le.key = resolvedKey;
        le.slcIndex = bridgeSlcIndex;
        le.cfgIndex = resolvedCfg;
        le.arrayIndex = (existingIndex >= 0)
                            ? existingIndex
                            : TextHook_MakeSyntheticHudElemIndex(elem);
        le.rawPtr = elem;
        le.rawLabelSlc = labelSlc;
        le.rawTextSlc = textSlc;
        le.objectiveLane = false;
        le.hintBridgeLane = true;
        le.x = ex;
        le.y = ey;
        le.fontScale = efs;
        le.color = ecolor;
        le.fromColor = efromColor;
        le.fadeStartTime = eFadeStart;
        le.fadeTime = eFadeTime;
        le.fxBirthTime = eFxBirth;
        le.fxLetterTime = eFxLetter;
        le.flags = eFlags;
        le.alpha = alpha;
        le.active = true;
        newElems.push_back(le);

        const uint64_t dynSig =
            ((uint64_t)(unsigned int)(le.arrayIndex & 0xFFFF) << 32) ^
            (NativeHudHash64(le.key) & 0xFFFFFFFFull);
        const unsigned long nowDyn = GetTickCount();
        auto itDyn = s_dynHintLogTick.find(dynSig);
        if (itDyn == s_dynHintLogTick.end() || (nowDyn - itDyn->second) > 1200) {
          s_dynHintLogTick[dynSig] = nowDyn;
          char dbuf[448];
          sprintf_s(
              dbuf,
              "[NHUD-DYN-SLOT] key=%s writerBase=0x%llX elem=0x%llX delta=%+d "
              "caller=+0x%X x=%.1f y=%.1f fs=%.2f a=%.2f cfg=0x%X slc=0x%X slot=%d",
              le.key.c_str(), (unsigned long long)slotMeta.base,
              (unsigned long long)elem, delta, slotMeta.callerHint, le.x, le.y,
              le.fontScale, le.alpha, le.cfgIndex, le.slcIndex, slotBucket);
          LogToFile(dbuf);
        }
      }

      if (!matchedAny) {
        const unsigned long nowDyn = GetTickCount();
        const uint64_t missSig =
            ((uint64_t)(slotMeta.base >> 4) << 16) ^
            (uint64_t)(slotMeta.callerHint & 0xFFFFu);
        auto itMiss = s_dynHintMissLogTick.find(missSig);
        if (itMiss == s_dynHintMissLogTick.end() ||
            (nowDyn - itMiss->second) > 1200) {
          s_dynHintMissLogTick[missSig] = nowDyn;
          char mbuf[448];
          if (bestGeomFound) {
            sprintf_s(
                mbuf,
                "[NHUD-DYN-CAND] writerBase=0x%llX caller=+0x%X delta=%+d "
                "type=%d x=%.1f y=%.1f fs=%.2f flags=0x%X label=0x%X text=0x%X "
                "fadeT=%d fxB=%d a=%.2f lastOff=0x%X hits=%lu",
                (unsigned long long)slotMeta.base, slotMeta.callerHint,
                bestGeomDelta, bestGeomType, bestGeomX, bestGeomY, bestGeomFs,
                bestGeomFlags, bestGeomLabel, bestGeomText, bestGeomFade,
                bestGeomFxBirth, bestGeomAlpha, slotMeta.lastWriteOff,
                (unsigned long)slotMeta.hitCount);
          } else {
            sprintf_s(
                mbuf,
                "[NHUD-DYN-MISS] writerBase=0x%llX caller=+0x%X lastOff=0x%X "
                "hits=%lu keyHint=%s",
                (unsigned long long)slotMeta.base, slotMeta.callerHint,
                slotMeta.lastWriteOff, (unsigned long)slotMeta.hitCount,
                slotMeta.keyHint.c_str());
          }
          LogToFile(mbuf);
        }
      }
    }
  }

  {
    static unsigned long s_lastLiveLog = 0;
    static int s_liveLogCount = 0;
    unsigned long nowTick = GetTickCount();
    if (!nativeItems.empty() && (nowTick - s_lastLiveLog) > 2000 &&
        s_liveLogCount < 80) {
      s_lastLiveLog = nowTick;
      s_liveLogCount++;
      for (const auto &e : nativeItems) {
        char lbuf[384];
        sprintf_s(lbuf,
                  "[OBJ-REVERSE] LIVE-ELEM[%d] ptr=0x%llX key=\"%s\" cfg=0x%X "
                  "x=%.1f y=%.1f fs=%.2f a=%.2f st=%d gst=%d fxB=%d fxL=%d "
                  "flags=0x%X",
                  e.arrayIndex, (unsigned long long)e.rawPtr, e.key.c_str(),
                  e.cfgIndex, e.x, e.y,
                  e.fontScale, e.alpha, e.serverTime, globalServerTime, e.fxBirthTime,
                  e.fxLetterTime, e.flags);
        LogToFile(lbuf);
      }
    }
  }

  if (!g_TimeScriptHooked) {
    TimeScript_PollFallbackDiagnostics();
  }

  for (auto &kv : bestCountdownByKey) {
    liveCountdowns.push_back(std::move(kv.second));
  }
  std::stable_sort(liveCountdowns.begin(), liveCountdowns.end(),
                   [](const LiveHudScriptCountdown &a,
                      const LiveHudScriptCountdown &b) {
                     if (a.key != b.key) {
                       return a.key < b.key;
                     }
                     return a.rawPtr < b.rawPtr;
                   });
  for (auto it = s_countdownSuppressionByPtr.begin();
       it != s_countdownSuppressionByPtr.end();) {
    if (activeCountdownPtrs.find(it->first) != activeCountdownPtrs.end()) {
      ++it;
      continue;
    }

    uint32_t currentColor = 0;
    if (ObjRev_ReadU32(it->first + 0x30, currentColor)) {
      currentColor =
          (currentColor & 0x00FFFFFFu) | ((uint32_t)it->second.colorAlpha << 24);
      ObjRev_WriteU32(it->first + 0x30, currentColor);
    }
    uint32_t currentFromColor = 0;
    if (ObjRev_ReadU32(it->first + 0x34, currentFromColor)) {
      currentFromColor = (currentFromColor & 0x00FFFFFFu) |
                         ((uint32_t)it->second.fromColorAlpha << 24);
      ObjRev_WriteU32(it->first + 0x34, currentFromColor);
    }
    it = s_countdownSuppressionByPtr.erase(it);
  }

  const unsigned long updateTick = GetTickCount();
  if (hasObjectiveLaneActivity) {
    ObjectiveRuntime::SetLastObjectiveLaneActivity(updateTick);
  }
  ObjRev_UpdateActiveObjectiveCfg(activeObjectiveCfgSet);
  {
    std::lock_guard<std::mutex> lock(g_liveHudElemsMutex);
    g_liveHudElems = std::move(newElems);
    g_liveHudElemsLastUpdate = updateTick;
  }
  {
    std::lock_guard<std::mutex> lock(g_liveHudScriptCountdownMutex);
    g_liveHudScriptCountdowns = std::move(liveCountdowns);
  }
  {
    ObjectiveRuntime::SetNativeObjectiveItems(std::move(nativeItems), updateTick);
  }
}

std::vector<NativeObjectiveItem> TextHook_GetNativeObjectiveItems() {
  return ObjectiveRuntime::GetNativeObjectiveItems();
}

unsigned long TextHook_GetNativeObjectiveItemsTick() {
  return ObjectiveRuntime::GetNativeObjectiveItemsTick();
}

// Public API: get current live objective HudElems
std::vector<LiveHudElem> TextHook_GetLiveHudElems() {
  std::lock_guard<std::mutex> lock(g_liveHudElemsMutex);
  return g_liveHudElems;
}

std::vector<LiveHudScriptCountdown> TextHook_GetLiveHudScriptCountdowns() {
  std::lock_guard<std::mutex> lock(g_liveHudScriptCountdownMutex);
  return g_liveHudScriptCountdowns;
}

std::vector<LiveHudElem> TextHook_GetRuntimeHudHintLiveElems() {
  std::vector<LiveHudElem> out;
  const unsigned long now = GetTickCount();
  const std::vector<RecentHudHintRuntimeSlot> recentSlots =
      TextHook_GetRecentHudHintRuntimeSlots(2200);
  const size_t totalRuntimeSlots = TextHook_GetHudHintRuntimeSlotCount();
  static std::unordered_map<uint64_t, unsigned long> s_rtHintLogTick;
  static std::unordered_map<uint64_t, unsigned long> s_rtHintMissLogTick;
  static unsigned long s_rtCacheLogTick = 0;
  if ((now - s_rtCacheLogTick) >
      ((recentSlots.empty() && totalRuntimeSlots == 0) ? 1500u : 800u)) {
    s_rtCacheLogTick = now;
    char cbuf[160];
    sprintf_s(cbuf, "[NHUD-RT-CACHE] recent=%zu total=%zu", recentSlots.size(),
              totalRuntimeSlots);
    LogToFile(cbuf);
  }

  if (recentSlots.empty()) {
    return out;
  }

  std::unordered_set<uintptr_t> dynElemDedup;
  dynElemDedup.reserve(recentSlots.size() * 24 + 8);
  out.reserve(recentSlots.size() * 2 + 8);

  for (const RecentHudHintRuntimeSlot &slotMeta : recentSlots) {
    RecentHudHintRuntimeSlot slotView = slotMeta;
    if (!slotView.hasParamSnapshot && !slotView.keyHint.empty()) {
      RecentHudHintRuntimeParams param{};
      if (TextHook_TryGetHudHintRuntimeParams(slotView.keyHint,
                                              slotView.callerHint, param)) {
        slotView.paramTick = param.tick;
        slotView.paramX = param.x;
        slotView.paramY = param.y;
        slotView.paramSX = param.sx;
        slotView.paramSY = param.sy;
        slotView.paramVW = param.virtualW;
        slotView.paramVH = param.virtualH;
        slotView.paramSlotBucket = param.slotBucket;
        slotView.hasParamSnapshot = true;
        slotView.hasColorSnapshot = param.hasColor;
        slotView.paramColor[0] = param.color[0];
        slotView.paramColor[1] = param.color[1];
        slotView.paramColor[2] = param.color[2];
        slotView.paramColor[3] = param.color[3];
      }
    }
    bool matchedAny = false;
    bool bestGeomFound = false;
    int bestGeomDelta = 0;
    float bestGeomX = 0.0f, bestGeomY = 0.0f, bestGeomFs = 0.0f;
    int bestGeomFlags = 0;
    int bestGeomType = 0;
    uint32_t bestGeomLabel = 0, bestGeomText = 0;
    int bestGeomFade = 0, bestGeomFxBirth = 0;
    float bestGeomAlpha = 0.0f;

    for (int delta = -0x80; delta <= 0x180; delta += 4) {
      const uintptr_t elem = slotView.base + (intptr_t)delta;
      if (!(elem > 0x10000 && elem < 0x7FFFFFFFFFFF)) {
        continue;
      }
      if (!dynElemDedup.insert(elem).second) {
        continue;
      }
      if (!IsSafeRead((void *)elem, 0xA8)) {
        continue;
      }

      const int existingIndex = ObjRev_ResolveHudElemArrayIndex(elem);

      uint32_t t = 0;
      if (!ObjRev_ReadU32(elem, t) || t == 0 || t > 13) {
        continue;
      }

      float ex = 0.0f, ey = 0.0f, efs = 0.0f;
      int eFlags = 0;
      if (!ObjRev_ReadF32(elem + 0x04, ex) ||
          !ObjRev_ReadF32(elem + 0x08, ey) ||
          !ObjRev_ReadF32(elem + 0x14, efs) ||
          !ObjRev_ReadI32(elem + 0xA4, eFlags)) {
        continue;
      }
      if (!std::isfinite(ex) || !std::isfinite(ey) || !std::isfinite(efs)) {
        continue;
      }
      if (efs < 0.45f || efs > 4.50f) {
        continue;
      }
      if (ey < -140.0f || ey > 540.0f) {
        continue;
      }

      const int slotBucket = TextHook_HudSlotFromEvent(ey, 480, 0);
      if (slotBucket <= 0) {
        continue;
      }

      uint32_t labelSlc = 0, textSlc = 0;
      uint32_t ecolor = 0, efromColor = 0;
      int eFadeStart = 0, eFadeTime = 0, eFxBirth = 0, eFxLetter = 0;
      if (!ObjRev_ReadU32(elem + 0x80, labelSlc) ||
          !ObjRev_ReadU32(elem + 0x84, textSlc) ||
          !ObjRev_ReadU32(elem + 0x30, ecolor) ||
          !ObjRev_ReadU32(elem + 0x34, efromColor) ||
          !ObjRev_ReadI32(elem + 0x38, eFadeStart) ||
          !ObjRev_ReadI32(elem + 0x3C, eFadeTime) ||
          !ObjRev_ReadI32(elem + 0x90, eFxBirth) ||
          !ObjRev_ReadI32(elem + 0x94, eFxLetter)) {
        continue;
      }

      const float alpha = (float)((ecolor >> 24) & 0xFFu) / 255.0f;
      if (!bestGeomFound ||
          ((slotBucket == 3) &&
           (std::fabs(ey - 240.0f) < std::fabs(bestGeomY - 240.0f)))) {
        bestGeomFound = true;
        bestGeomDelta = delta;
        bestGeomX = ex;
        bestGeomY = ey;
        bestGeomFs = efs;
        bestGeomFlags = eFlags;
        bestGeomType = (int)t;
        bestGeomLabel = labelSlc;
        bestGeomText = textSlc;
        bestGeomFade = eFadeTime;
        bestGeomFxBirth = eFxBirth;
        bestGeomAlpha = alpha;
      }

      uint32_t resolvedCfg = 0;
      uint32_t bridgeSlcIndex = 0;
      if (ObjRev_IsPlausibleConfigIndex(textSlc)) {
        resolvedCfg = textSlc;
      } else if (ObjRev_IsPlausibleConfigIndex(labelSlc)) {
        resolvedCfg = labelSlc;
      }
      if (resolvedCfg != 0) {
        uint32_t directSlc = 0;
        uint32_t directBias = 0;
        if (ObjRev_GetDirectCfgSlc(resolvedCfg, directSlc, directBias, false) &&
            directSlc > 1) {
          bridgeSlcIndex = directSlc;
        } else {
          bridgeSlcIndex = resolvedCfg;
        }
      }
      if (bridgeSlcIndex == 0) {
        if (textSlc > 1 && textSlc < 0x40000u) {
          bridgeSlcIndex = textSlc;
        } else if (labelSlc > 1 && labelSlc < 0x40000u) {
          bridgeSlcIndex = labelSlc;
        }
      }

      std::string resolvedKey;
      if (bridgeSlcIndex > 1) {
        std::string slcKey;
        if (TextHook_GetSlcKey(bridgeSlcIndex, slcKey) &&
            ObjRev_LooksLikeLocalizationKey(slcKey.c_str())) {
          resolvedKey = slcKey;
        }
      }
      if (resolvedKey.empty() && resolvedCfg != 0) {
        resolvedKey = ObjRev_ResolveConfigKey(resolvedCfg, false, true);
        if (resolvedKey.empty()) {
          resolvedKey = ObjRev_ResolveConfigKey(resolvedCfg, false, false);
        }
      }
      if (resolvedKey.empty() && !slotMeta.keyHint.empty() &&
          ObjRev_LooksLikeLocalizationKey(slotMeta.keyHint.c_str())) {
        // Last-resort writer-bound key association. Geometry still comes from
        // the native runtime slot; this only supplies the key when cfg/slc
        // fields are unavailable in the slot family.
        resolvedKey = slotMeta.keyHint;
      }
      if (resolvedKey.empty() ||
          !ObjRev_LooksLikeLocalizationKey(resolvedKey.c_str()) ||
          IsObjectiveOverlayKeyName(resolvedKey) ||
          IsObjectiveStatusMessageKeyName(resolvedKey)) {
        continue;
      }

      if (alpha <= 0.0f && eFxBirth <= 0 && eFadeTime <= 0) {
        continue;
      }

      matchedAny = true;
      LiveHudElem le{};
      le.key = resolvedKey;
      le.slcIndex = bridgeSlcIndex;
      le.cfgIndex = resolvedCfg;
      le.arrayIndex = (existingIndex >= 0)
                          ? existingIndex
                          : TextHook_MakeSyntheticHudElemIndex(elem);
      le.rawPtr = elem;
      le.rawLabelSlc = labelSlc;
      le.rawTextSlc = textSlc;
      le.objectiveLane = false;
      le.hintBridgeLane = true;
      le.x = ex;
      le.y = ey;
      le.fontScale = efs;
      le.color = ecolor;
      le.fromColor = efromColor;
      le.fadeStartTime = eFadeStart;
      le.fadeTime = eFadeTime;
      le.fxBirthTime = eFxBirth;
      le.fxLetterTime = eFxLetter;
      le.flags = eFlags;
      le.alpha = alpha;
      le.active = true;
      out.push_back(le);

      const uint64_t dynSig =
          ((uint64_t)(unsigned int)(le.arrayIndex & 0xFFFF) << 32) ^
          (NativeHudHash64(le.key) & 0xFFFFFFFFull);
      auto itDyn = s_rtHintLogTick.find(dynSig);
      if (itDyn == s_rtHintLogTick.end() || (now - itDyn->second) > 1200) {
        s_rtHintLogTick[dynSig] = now;
        char dbuf[448];
        sprintf_s(
            dbuf,
            "[NHUD-RT-SLOT] key=%s writerBase=0x%llX elem=0x%llX delta=%+d "
            "caller=+0x%X x=%.1f y=%.1f fs=%.2f a=%.2f cfg=0x%X slc=0x%X slot=%d",
              le.key.c_str(), (unsigned long long)slotView.base,
            (unsigned long long)elem, delta, slotView.callerHint, le.x, le.y,
            le.fontScale, le.alpha, le.cfgIndex, le.slcIndex, slotBucket);
        LogToFile(dbuf);
      }
    }

    if (!matchedAny) {
      const uint64_t missSig =
          ((uint64_t)(slotView.base >> 4) << 16) ^
          (uint64_t)(slotView.callerHint & 0xFFFFu);
      auto itMiss = s_rtHintMissLogTick.find(missSig);
      if (itMiss == s_rtHintMissLogTick.end() || (now - itMiss->second) > 1200) {
        s_rtHintMissLogTick[missSig] = now;
        char mbuf[448];
        if (bestGeomFound) {
          sprintf_s(
              mbuf,
              "[NHUD-RT-CAND] writerBase=0x%llX caller=+0x%X delta=%+d "
              "type=%d x=%.1f y=%.1f fs=%.2f flags=0x%X label=0x%X text=0x%X "
              "fadeT=%d fxB=%d a=%.2f lastOff=0x%X hits=%lu keyHint=%s",
              (unsigned long long)slotView.base, slotView.callerHint,
              bestGeomDelta, bestGeomType, bestGeomX, bestGeomY, bestGeomFs,
              bestGeomFlags, bestGeomLabel, bestGeomText, bestGeomFade,
              bestGeomFxBirth, bestGeomAlpha, slotView.lastWriteOff,
              (unsigned long)slotView.hitCount, slotView.keyHint.c_str());
        } else {
          sprintf_s(
              mbuf,
              "[NHUD-RT-MISS] writerBase=0x%llX caller=+0x%X lastOff=0x%X "
              "hits=%lu keyHint=%s",
              (unsigned long long)slotView.base, slotView.callerHint,
              slotView.lastWriteOff, (unsigned long)slotView.hitCount,
              slotView.keyHint.c_str());
        }
        LogToFile(mbuf);
      }
    }
  }

  return out;
}

// Public API: get SLC?萸웕y mapping for a given SLC index
bool TextHook_GetSlcKey(uint32_t slcIndex, std::string &outKey) {
  std::lock_guard<std::mutex> lock(g_slcToKeyMutex);
  auto it = g_slcToKey.find(slcIndex);
  if (it != g_slcToKey.end()) {
    outKey = it->second;
    return true;
  }
  return false;
}

#endif // GHOSTSKOR_OBJECTIVE_REVERSE

// === CG_DrawHudElem coordinate capture ===
static std::mutex g_cgDrawCaptureMutex;
static std::unordered_map<std::string, CgDrawHudElemCapture> g_cgDrawCapture;

void TextHook_StoreCgDrawCapture(const std::string &key,
                                 const CgDrawHudElemCapture &cap) {
  std::lock_guard<std::mutex> lock(g_cgDrawCaptureMutex);
  g_cgDrawCapture[key] = cap;
  if (g_cgDrawCapture.size() > 256) {
    const DWORD now = GetTickCount();
    for (auto it = g_cgDrawCapture.begin(); it != g_cgDrawCapture.end();) {
      if ((now - it->second.tick) > 10000) {
        it = g_cgDrawCapture.erase(it);
      } else {
        ++it;
      }
    }
  }
}

bool TextHook_GetCgDrawCapture(const std::string &key,
                               CgDrawHudElemCapture &out,
                               unsigned long ttlMs) {
  std::lock_guard<std::mutex> lock(g_cgDrawCaptureMutex);
  auto it = g_cgDrawCapture.find(key);
  if (it == g_cgDrawCapture.end()) {
    return false;
  }
  const DWORD now = GetTickCount();
  const DWORD age = (now >= it->second.tick) ? (now - it->second.tick) : 0;
  if (age > ttlMs) {
    return false;
  }
  out = it->second;
  return true;
}

// === Per-elemPtr capture cache ===
static std::mutex g_cgDrawElemPtrCaptureMutex;
static std::unordered_map<uintptr_t, CgDrawHudElemCapture>
    g_cgDrawElemPtrCapture;

void TextHook_StoreCgDrawElemPtrCapture(uintptr_t elemPtr,
                                        const CgDrawHudElemCapture &cap) {
  std::lock_guard<std::mutex> lock(g_cgDrawElemPtrCaptureMutex);
  g_cgDrawElemPtrCapture[elemPtr] = cap;
  if (g_cgDrawElemPtrCapture.size() > 128) {
    const DWORD now = GetTickCount();
    for (auto it = g_cgDrawElemPtrCapture.begin();
         it != g_cgDrawElemPtrCapture.end();) {
      if ((now - it->second.tick) > 3000) {
        it = g_cgDrawElemPtrCapture.erase(it);
      } else {
        ++it;
      }
    }
  }
}

bool TextHook_GetCgDrawElemPtrCapture(uintptr_t elemPtr,
                                      CgDrawHudElemCapture &out,
                                      unsigned long ttlMs) {
  std::lock_guard<std::mutex> lock(g_cgDrawElemPtrCaptureMutex);
  auto it = g_cgDrawElemPtrCapture.find(elemPtr);
  if (it == g_cgDrawElemPtrCapture.end()) {
    return false;
  }
  const DWORD now = GetTickCount();
  const DWORD age = (now >= it->second.tick) ? (now - it->second.tick) : 0;
  if (age > ttlMs) {
    return false;
  }
  out = it->second;
  return true;
}




