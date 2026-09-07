#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Feature toggles (compile-time)
#ifndef GHOSTSKOR_NATIVE_SUBTITLE
#define GHOSTSKOR_NATIVE_SUBTITLE 1
#endif
#ifndef GHOSTSKOR_NATIVE_HUD
#define GHOSTSKOR_NATIVE_HUD 1
#endif
#ifndef GHOSTSKOR_HUD_SCANNER
#define GHOSTSKOR_HUD_SCANNER 0
#endif

// Experimental: native subtitle remove sync via reverse (PAGE_GUARD/VEH).
// Default OFF because it is expensive and can be unstable depending on the
// environment. Enable only for reversing sessions.
#ifndef GHOSTSKOR_SUBTITLE_REVERSE
#define GHOSTSKOR_SUBTITLE_REVERSE 0
#endif
#ifndef GHOSTSKOR_SUBTITLE_REVERSE_VEH
#define GHOSTSKOR_SUBTITLE_REVERSE_VEH 0
#endif

// Debug: log timescale(com_timescale) changes that drive subtitle clock.
#ifndef GHOSTSKOR_SUBCLOCK_LOG_TS
#define GHOSTSKOR_SUBCLOCK_LOG_TS 0
#endif

// Experimental: HudElem array reverse-engineering for objectives.
// Phase 1 = diagnostic logging only (no behavior changes).
// Enable only for reversing sessions.
#ifndef GHOSTSKOR_OBJECTIVE_REVERSE
#define GHOSTSKOR_OBJECTIVE_REVERSE 0
#endif

// Objective/Status source hard switch:
// ON  -> consume native draw/producer reverse snapshot only
// OFF -> keep legacy SLC/consumer-derived authority paths
#ifndef GHOSTSKOR_OSREV_ONLY
#define GHOSTSKOR_OSREV_ONLY 1
#endif

namespace TextHook {
void Init();
void Shutdown();
}

// Screen size (from SwapChain) for menu size classification
void TextHook_UpdateScreenSize(int width, int height);
void TextHook_GetScreenSize(int &width, int &height);

// Get current in-game subtitle (for D3D11Hook to render)
std::string TextHook_GetInGameSubtitle();

// Snapshot with timestamp for animation
struct InGameSubtitleSnapshot {
  std::string text;
  unsigned long timestamp;
};

InGameSubtitleSnapshot TextHook_GetInGameSubtitleSnapshot();

struct InGameSubtitleEntry {
  std::string text;
  unsigned long timestamp;       // "lastRefresh" - updated every SEH call
  unsigned long firstSeen;       // When first appeared (for fadeIn animation)
  std::string key;
  std::string english;
  int stackIndex;                // Current logical stack position (0=newest)
  unsigned long pushAnimStart;   // When this entry was pushed up (for animation)
  int renderSlot;                // Fixed render slot (0=newest bottom)
  int msgTimeMs;                 // Runtime subtitle msg time (from +0x235620 a3)
  int fadeOutMs;                 // Runtime fade-out time (from +0x235620 a4)
  int runtimeArg6;               // Runtime extra arg (from +0x235620 stack arg)
  unsigned long runtimeTick;     // When runtime timing was captured
  uintptr_t nativeSlotPtr;       // Bound native subtitle slot pointer
  bool nativeSlotValid;          // True when nativeSlotPtr is trusted
  unsigned long nativeRemoveTick; // Native remove/disappear tick
  bool nativeRemoveValid;        // True when nativeRemoveTick is latched
  int nativeRemoveReason;        // 1=text clear, 2=flag off, 3=alpha/progress
};

std::vector<InGameSubtitleEntry> TextHook_GetInGameSubtitleList();

// Poll-based subtitle injection: given raw English text from engine ring buffer,
// look up Korean translation and inject into the subtitle queue.
// Returns true if subtitle was found and injected.
bool TextHook_PollInjectSubtitleByEnglish(const std::string &rawEnglishText);

struct SubtitleReverseStats {
  unsigned long eventsSeen;
  unsigned long bindAttempts;
  unsigned long bindSuccess;
  unsigned long bindConflicts;
  unsigned long guardInstall;
  unsigned long guardHits;
  unsigned long guardRearms;
  unsigned long removeDetected;
  unsigned long removeByTextPtr;
  unsigned long removeByFlag;
  unsigned long removeByAlpha;
  unsigned long failOpenCount;
  bool reverseEnabled;
  bool vehEnabled;
};

SubtitleReverseStats TextHook_GetSubtitleReverseStats();

// Diagnostic: last subtitle English observed in SEH hook
struct LastSubtitleDiag {
  std::string key;
  std::string english;
  unsigned long timestamp;
};

LastSubtitleDiag TextHook_GetLastSubtitleDiag();

// Encapsulated In-Game Subtitle State (from Game Engine)
struct NativeSubtitleState {
  float x;
  float y;
  float xScale;
  float yScale;
  float color[4];
  float fontHeight;
  int style;                // Added Style
  unsigned long lastUpdate; // Timestamp
  bool valid;
};

// Accessors for Native Animation
NativeSubtitleState TextHook_GetNativeSubtitleState();
void TextHook_UpdateNativeSubtitleState(float x, float y, const float *color,
                                        float fontHeight, int style,
                                        float xScale, float yScale);

// Per-line native subtitle capture (for typewriter + per-line sync)
struct SubtitleNativeEntry {
  float x;
  float y;
  float xScale;
  float yScale;
  float color[4];
  float fontHeight;
  int style;
  unsigned long lastUpdate;
  int visibleChars;
  int totalChars;
  bool valid;
};

bool TextHook_GetSubtitleNativeState(const std::string &key,
                                     const std::string &english,
                                     SubtitleNativeEntry &out);
void TextHook_UpdateSubtitleNativeFromKey(const std::string &key, float x,
                                          float y, float xScale, float yScale,
                                          const float *color, float fontHeight,
                                          int style);

// Menu context flag (for subtitle suppression in menu)
bool TextHook_IsMenuContext();
bool TextHook_IsFrontendMenuLikely();
bool TextHook_IsMenuActiveRaw();
bool TextHook_IsPauseMenuLikely();
bool TextHook_IsMenuDiagActive();
unsigned long long TextHook_GetMenuDiagSessionId();
void TextHook_ArmMenuDiagWindow(unsigned long durationMs,
                                const char *reason = nullptr);
unsigned long TextHook_GetSubtitleClockNow();

struct HudStateSignalSnapshot {
  bool hasClPaused;
  bool clPaused;
  bool hideHudFast;
  bool creditsActive;
  bool hudShowObjectives;
  bool hudShowIntel;
  bool uiShowPopup;
  bool uiSecuringActive;
  float uiSecuringProgress;
  bool uiPrevMapChanged;
  unsigned long sampleTick;
};

HudStateSignalSnapshot TextHook_GetHudStateSignalSnapshot();

// Runtime flags
bool TextHook_IsNativeSubtitleEnabled();
bool TextHook_IsHudNativeEnabled();
void TextHook_SetNativeSubtitleEnabled(bool enabled);
void TextHook_SetHudNativeEnabled(bool enabled);
void TextHook_ArmHudHintWriterFocus(const std::string &key,
                                    unsigned int callerOffset);

// Best-effort dvar access (used for matching native UI colors without
// hardcoding). Currently supports vec4 dvars by name.
bool TextHook_TryGetDvarVec4(const char *name, float out[4]);
bool TextHook_TryGetDvarInt(const char *name, int &outValue);

// Resolve English -> localization key (best-effort)
bool TextHook_ResolveKeyFromEnglish(const std::string &english,
                                    std::string &outKey);
bool TextHook_GetLocalizedKeyText(const std::string &key,
                                  std::string &outKorean,
                                  std::string &outEnglish);
bool TextHook_IsSubtitleKeyActive(const std::string &key,
                                  unsigned long maxAgeMs = 2000);
bool TextHook_HasRecentHudActivity(unsigned long maxAgeMs = 1500);
bool TextHook_HasRecentSubtitleActivity(unsigned long maxAgeMs = 1500);
bool TextHook_HasRecentObjectiveLaneActivity(unsigned long maxAgeMs = 1200);

// =============================================================================
// HUD OVERLAY SYSTEM (Objectives, Prompts, Game Messages)
// =============================================================================

enum ObjectiveChannel : int;

// Unified overlay event/claim bus (Objective / Hint / ObjHudHint).
enum OverlayTrack {
  OVERLAY_TRACK_OBJECTIVE = 0,
  OVERLAY_TRACK_HINT = 1,
  OVERLAY_TRACK_OBJHUDHINT = 2
};

enum OverlaySource {
  OVERLAY_SOURCE_HUD560 = 0,
  OVERLAY_SOURCE_ADDCMD = 1,
  OVERLAY_SOURCE_HUDELEM_REVERSE = 2,
  OVERLAY_SOURCE_FSHOOK = 3,
  OVERLAY_SOURCE_SLC = 4
};

enum HudRouteChannel {
  HUD_ROUTE_UNSPECIFIED = 0,
  HUD_ROUTE_HINT = 1,
  HUD_ROUTE_OBJHUDHINT = 2,
  HUD_ROUTE_OBJECTIVE = 3,
  HUD_ROUTE_IGNORE = 4,
  HUD_ROUTE_UNRESOLVED = 5
};

struct HudConsumerSignature {
  OverlaySource source;
  unsigned int callerOffset;
  int slotBucket;
  unsigned short virtualSpaceTag;
};

struct HudRouteDecision {
  HudRouteChannel channel;
  float confidence;
  uint64_t consumerId;
  HudConsumerSignature signature;
  std::string reason;
  bool learned;
  bool quarantined;
  HudRouteChannel previousChannel;
  HudRouteChannel incomingChannel;
};

enum OverlaySessionState {
  OVERLAY_SESSION_GAMEPLAY = 0,
  OVERLAY_SESSION_PAUSE = 1,
  OVERLAY_SESSION_FRONTEND = 2,
  OVERLAY_SESSION_LOADING = 3,
  OVERLAY_SESSION_WARMUP = 4,
  OVERLAY_SESSION_RESTARTING = 5,
  OVERLAY_SESSION_DEAD = 6
};

enum OverlayResetReason {
  OVERLAY_RESET_DISCONNECT = 0,
  OVERLAY_RESET_FRONTEND_ENTER = 1,
  OVERLAY_RESET_MAP_RESTART = 2,
  OVERLAY_RESET_DEATH_RESPAWN = 3,
  OVERLAY_RESET_MANUAL = 4,
  OVERLAY_RESET_CHECKPOINT_RESTART = 5,
  OVERLAY_RESET_MISSION_RESTART = 6,
  OVERLAY_RESET_FAST_RESTART = 7
};

struct OverlayDrawEvent {
  uint64_t frameId;
  unsigned long tick;
  OverlaySource source;
  unsigned int callerOffset;
  std::string key;
  std::string english;
  float x;
  float y;
  float sx;
  float sy;
  float fontH;
  float alpha;
  int style;
  unsigned short virtualW;
  unsigned short virtualH;
  unsigned int flags;
  int fxBirth;
  int fxLetter;
  int serverTime;
  ObjectiveChannel channelHint;
  uint64_t consumerId;
  HudRouteChannel routeChannel;
  float routeConfidence;
  // Precomputed in PublishOverlayEvent to avoid per-frame recompute churn.
  uint32_t normalizedTextHash;
  uint32_t normalizedKeyHash;
  short slotCached;
  unsigned short reservedOverlay;
};

struct OverlayInstanceId {
  OverlaySource source;
  unsigned int callerOffset;
  int slotOrElemIndex;
  int fxBirth;
  uint32_t textHash;
  uint64_t consumerId;
};

struct OverlayClaim {
  uint64_t frameId;
  OverlayInstanceId instanceId;
  OverlayTrack track;
  int priority;
  std::string reason;
  std::string key;
};

void TextHook_PublishOverlayEvent(const OverlayDrawEvent &event);
void TextHook_BeginOverlayFrame(uint64_t frameId, OverlaySessionState state);
void TextHook_EndOverlayFrame(uint64_t frameId);
std::vector<OverlayClaim> TextHook_GetOverlayClaims(uint64_t frameId);
void TextHook_ResetOverlayRuntime(OverlayResetReason reason);



// ??? Hint Pipeline (Phase 3) ????????????????????????????????????????????????
// Dedicated container for HUD hint keys (HINT_*, PLATFORM_*, *_HINT etc.)
// Native-first strict path; generic HUD overlay routing has been removed.
struct HintOverlayEntry {
  std::string key;
  std::string korean;
  std::string english;
  uint64_t consumerId;
  int slotBucket;
  float nativeX, nativeY;          // From HUD560 / native entry
  float nativeXScale, nativeYScale;
  float fontHeight;
  float color[4];
  unsigned long firstSeen;
  unsigned long lastSeen;
  bool hasNativeParams;            // True if HUD560 provided placement
  unsigned char nativeSource;      // HudNativeSource (RENDER=1, SCANNER=2, ADDCMD=3)
  uint8_t nativeSourceToken;       // NativeHudSourceToken (0 when legacy/unknown)
  unsigned short nativeVirtualW;
  unsigned short nativeVirtualH;
  unsigned int callerOffset;
  unsigned long lastNativeSeenTick;
  unsigned long lastEnglishSeenTick;
  unsigned long lastRenderedTick;
};

// Get snapshot of current hint entries (thread-safe copy)
std::vector<HintOverlayEntry> TextHook_GetHintSnapshot();
// Clear all hint entries (e.g., on checkpoint restart)
void TextHook_ClearHintEntries();
// ??? ObjHudHint Pipeline (Phase 4) ??????????????????????????????????????????
// HUD560-captured prompts that don't match Objective or Hint patterns.
// Mission-specific prompt overlays (mid-bottom screen).
struct ObjHudHintEntry {
  std::string key;
  std::string korean;
  std::string english;
  float nativeX, nativeY;
  float nativeXScale, nativeYScale;
  float fontHeight;
  float color[4];
  unsigned long firstSeen;
  unsigned long lastSeen;
  bool hasNativeParams;
  unsigned char nativeSource;
  uint8_t nativeSourceToken;      // NativeHudSourceToken (0 when legacy/unknown)
  unsigned short nativeVirtualW;
  unsigned short nativeVirtualH;
  unsigned int callerOffset;
  unsigned long lastNativeSeenTick;
  unsigned long lastEnglishSeenTick;
  unsigned long lastRenderedTick;
};

// Get snapshot of current ObjHudHint entries (thread-safe copy)
std::vector<ObjHudHintEntry> TextHook_GetObjHudHintSnapshot();
// Clear all ObjHudHint entries
void TextHook_ClearObjHudHintEntries();

// Clear native HUD capture cache (e.g., on video/loading transition)
void TextHook_ClearHudNativeEntries();
// Clear all in-game subtitle entries
void TextHook_ClearSubtitleQueue();

// Native HUD entry source
enum HudNativeSource {
  HUD_NATIVE_RENDER = 1,
  HUD_NATIVE_SCANNER = 2,
  HUD_NATIVE_ADDCMD = 3
};

// Objective rendering channels (native ownerdraw-like grouping)
enum ObjectiveChannel : int {
  OBJ_CHANNEL_NONE = 0,
  OBJ_CHANNEL_GAMEPLAY_LIST = 1,
  OBJ_CHANNEL_GAMEPLAY_UPDATE = 2,
  OBJ_CHANNEL_PAUSE_LIST = 3,
  OBJ_CHANNEL_PAUSE_HEADER = 4,
  OBJ_CHANNEL_GAME_SAVED = 5,
  OBJ_CHANNEL_SAVE_PROGRESS = 6
};

// Native HUD entry (captured from render or scanner)
struct HudNativeEntry {
  std::string key;
  float x;
  float y;
  float xScale;
  float yScale;
  float fontHeight;
  float color[4];
  int style;
  unsigned long lastUpdate;
  HudNativeSource source;
  uint8_t sourceToken;
  // Coordinate space:
  // - virtualW/H == 0: x/y/fontHeight are already in screen pixel space
  // - virtualW/H != 0: x/y/fontHeight are in a virtual coordinate system
  //   (IW6 HUD commonly uses 640x480).
  unsigned short virtualW;
  unsigned short virtualH;
  unsigned int callerOffset;
  unsigned char objectiveChannel;
  unsigned char reserved0;
  unsigned short reserved1;
  uint64_t consumerId;
  int slotBucket;
  bool objectiveLike;
  bool valid;
};

enum NativeHudTrack : uint8_t {
  NATIVE_HUD_TRACK_HINT = 0,
  NATIVE_HUD_TRACK_OBJHINT = 1,
  NATIVE_HUD_TRACK_OBJECTIVE = 2,
  NATIVE_HUD_TRACK_MISC = 3
};

enum NativeHudSourceToken : uint8_t {
  NATIVE_HUD_SOURCE_HUD560 = 1,
  NATIVE_HUD_SOURCE_ADDCMD = 2,
  NATIVE_HUD_SOURCE_HUDELEM = 3,
  NATIVE_HUD_SOURCE_BRIDGE = 4
};

struct NativeHudEvent {
  uint8_t sourceToken;
  NativeHudTrack track;
  unsigned long tick;
  uint64_t instanceId;
  uintptr_t hudElemPtr;
  unsigned short ownerdrawId;
  uint32_t cfgIndex;
  int arrayIndex;
  float x;
  float y;
  unsigned short virtualW;
  unsigned short virtualH;
  float xScale;
  float yScale;
  float fontHeight;
  float color[4];
  float fromColor[4];
  int fadeStartTime;
  int fadeTime;
  int fxBirthTime;
  int fxLetterTime;
  int serverTime;
  int style;
  unsigned int flags;
  unsigned int callerOffset;
  std::string keyToken;
  std::string rawText;
  bool hasNativeEvidence;
  bool hasEnglishEvidence;
};

struct NativeHudInstance {
  uint64_t instanceId;
  NativeHudTrack track;
  uint8_t sourceToken;
  uintptr_t hudElemPtr;
  unsigned short ownerdrawId;
  uint32_t cfgIndex;
  int arrayIndex;
  uint64_t consumerId;
  unsigned int callerOffset;
  std::string keyToken;
  std::string rawText;
  float x;
  float y;
  unsigned short virtualW;
  unsigned short virtualH;
  float xScale;
  float yScale;
  float fontHeight;
  float color[4];
  float fromColor[4];
  int fadeStartTime;
  int fadeTime;
  int fxBirthTime;
  int fxLetterTime;
  int serverTime;
  int style;
  unsigned int flags;
  unsigned long firstSeenTick;
  unsigned long lastSeenTick;
  unsigned int seenFrames;
  bool hasNativeEvidence;
  bool hasEnglishEvidence;
  bool objectiveLaneEvidence;
};

struct NativeHudFrameSnapshot {
  unsigned long sampleTick;
  unsigned long lastHudNativeTick;
  unsigned long lastHudElemTick;
  std::vector<HudNativeEntry> nativeEntries;
  std::vector<NativeHudEvent> events;
  std::vector<NativeHudInstance> instances;
};

int TextHook_HudSlotFromEvent(float y, unsigned short virtualH,
                              unsigned int packedFlags);
uint64_t TextHook_MakeHudConsumerId(OverlaySource source,
                                    unsigned int callerOffset, int slotBucket,
                                    unsigned short virtualW,
                                    unsigned short virtualH);

bool TextHook_GetHudNativeEntry(const std::string &key, HudNativeEntry &out);
std::vector<HudNativeEntry> TextHook_GetHudNativeEntries();
void TextHook_UpdateHudNativeEntry(const HudNativeEntry &entry);
bool TextHook_PromoteStrictHintSnapshotByCfg(uint32_t textCfg,
                                             uint32_t labelCfg,
                                             float x,
                                             float y,
                                             float fontScale,
                                             uint32_t horzAlign,
                                             uint32_t vertAlign,
                                             uint32_t colorPacked,
                                             int elemTime,
                                             int fxBirthTime,
                                             int fxLetterTime,
                                             int flags,
                                             unsigned int callerOffset,
                                             const char *logTag);
bool TextHook_GetNativeHudFrameSnapshot(NativeHudFrameSnapshot &out);
// Deprecated: native-first path does not consume route decisions at render time.
bool TextHook_GetHudRouteForNativeEntry(const HudNativeEntry &entry,
                                        HudRouteChannel &outChannel);
bool TextHook_GetHudRuntimeTail(const std::string &key, float hintX, float hintY,
                                unsigned int callerOffset, std::string &outTail);

// Objective-only runtime overlay entries (native-first path)
struct ObjectiveOverlayEntry {
  std::string key;
  std::string korean;
  std::string english;
  ObjectiveChannel channel;
  float color[4];
  float scale;
  float fontHeight;
  int style;
  float nativeX;
  float nativeY;
  float nativeXScale;
  float nativeYScale;
  unsigned short nativeVirtualW;
  unsigned short nativeVirtualH;
  unsigned int callerOffset;
  HudNativeSource nativeSource;
  bool nativeValid;
  bool consumerOwned;
  unsigned short ownerdrawId;
  short visibleChars;
  short totalChars;
  unsigned int frameSeen;
  unsigned int nativeClockStart;
  unsigned int nativeClockEnd;
  uint64_t runtimeInstanceId;
  uint32_t generation;
  unsigned long firstSeen;
  unsigned long lastSeen;
  unsigned long lastFSHookSeen; // Last time FSHook fired for this key (not consumer)
  unsigned long lastForceSeen;  // Last forceGameplayList preload tick
};

struct ObjectiveStatusEvent {
  std::string key;
  std::string korean;
  std::string english;
  unsigned long triggerTick;
  unsigned int callerOffset;
  uint32_t sourceCfg;
  uint64_t sourceInstanceSig;
  unsigned int sequence;
};

enum ObjectiveStatusNativeChannel : uint8_t {
  OBJSTAT_NATIVE_OBJECTIVE = 1,
  OBJSTAT_NATIVE_STATUS = 2
};

enum ObjectiveStatusNativeLineFlags : uint32_t {
  OBJSTAT_LINE_NONE = 0u,
  OBJSTAT_LINE_HAS_VISUAL = 1u << 0,
  OBJSTAT_LINE_STATUS_PROBE_FALLBACK = 1u << 1,
  OBJSTAT_LINE_OWNER_LOCKED = 1u << 2
};

enum ObjectiveNativeOwnerKind : uint8_t {
  OBJSTAT_OWNER_NONE = 0,
  OBJSTAT_OWNER_NATIVE_JOIN = 1,
  OBJSTAT_OWNER_NATIVE_VISUAL_ONLY = 2,
  OBJSTAT_OWNER_STATUS_PROBE = 3
};

struct ObjectiveStatusNativeLine {
  ObjectiveStatusNativeChannel channel = OBJSTAT_NATIVE_OBJECTIVE;
  ObjectiveNativeOwnerKind ownerKind = OBJSTAT_OWNER_NONE;
  int laneSlot = 0;
  std::string key;
  std::string text;
  uint64_t instanceSig = 0;
  uint32_t cfgIndex = 0;
  uint64_t sourceElemPtrOrIndex = 0;
  int fxBirthTime = 0;
  int fxLetterTime = 0; // typewriter ms-per-char (0 = no typewriter)
  float x = 0.0f;
  float y = 0.0f;
  float scale = 1.25f;
  float fontHeight = 78.0f;
  float alpha = 1.0f;
  uint32_t colorPacked = 0xFFFFFFFFu;
  unsigned short virtualW = 0;
  unsigned short virtualH = 0;
  unsigned long lifeStart = 0;
  unsigned long lifeEnd = 0;
  uint32_t authorityFlags = OBJSTAT_LINE_NONE;
  unsigned int sourceSeq = 0;
  unsigned int authorityScore = 0;
  unsigned int producerCallerRva = 0;
  unsigned int drawCallerRva = 0;
  unsigned int producerCalleeRva = 0;
  unsigned int drawCalleeRva = 0;
};

struct ObjectiveStatusNativeOwner {
  uint64_t instanceSig = 0;
  ObjectiveStatusNativeChannel channel = OBJSTAT_NATIVE_OBJECTIVE;
  ObjectiveNativeOwnerKind ownerKind = OBJSTAT_OWNER_NONE;
  std::string key;
  int laneSlot = 0;
  uint32_t cfgIndex = 0;
  int fxBirthTime = 0;
  uint64_t elemPtrOrIndex = 0;
  unsigned int authorityScore = 0;
  unsigned int sourceSeq = 0;
  unsigned long firstSeenTick = 0;
  unsigned long lastSeenTick = 0;
  uint32_t resetGeneration = 0;
  ObjectiveStatusNativeLine line{};
};

struct TopLeftTextEvent {
  uint64_t instanceSig = 0;
  ObjectiveStatusNativeChannel channel = OBJSTAT_NATIVE_OBJECTIVE;
  std::string key;
  uint32_t cfgIndex = 0;
  uint32_t slcIndex = 0;
  uint64_t elemPtrOrIndex = 0;
  int fxBirthTime = 0;
  int laneSlot = 0;
  unsigned int callerRva = 0;
  unsigned int calleeRva = 0;
  unsigned int sourceSeq = 0;
  unsigned int authorityScore = 0;
  unsigned long captureTick = 0;
  bool exactJoin = false;
  bool syntheticOnly = false;
};

struct TopLeftVisualSample {
  uint64_t instanceSig = 0;
  uint32_t cfgIndex = 0;
  uint64_t elemPtrOrIndex = 0;
  int fxBirthTime = 0;
  int laneSlot = 0;
  float x = 0.0f;
  float y = 0.0f;
  float scale = 1.25f;
  float fontHeight = 78.0f;
  float alpha = 1.0f;
  uint32_t colorPacked = 0xFFFFFFFFu;
  unsigned short virtualW = 0;
  unsigned short virtualH = 0;
  unsigned int callerRva = 0;
  unsigned int calleeRva = 0;
  unsigned long captureTick = 0;
};

using TopLeftOwner = ObjectiveStatusNativeOwner;
using TopLeftSnapshotLine = ObjectiveStatusNativeLine;

struct ObjectiveInstanceId {
  uint64_t elemPtrOrIndex = 0;
  int fxBirth = 0;
  uint32_t cfgIndex = 0;
  bool valid = false;
};

struct ObjectiveTextCapture {
  ObjectiveInstanceId instanceId;
  ObjectiveStatusNativeChannel channel = OBJSTAT_NATIVE_OBJECTIVE;
  std::string key;
  std::string rawText;
  uint32_t slcIndex = 0;
  unsigned int callerRva = 0;
  unsigned int calleeRva = 0;
  unsigned int threadId = 0;
  unsigned long captureTick = 0;
};

struct ObjectiveVisualCapture {
  ObjectiveInstanceId instanceId;
  ObjectiveStatusNativeChannel channel = OBJSTAT_NATIVE_OBJECTIVE;
  int laneSlot = 0;
  std::string key;
  std::string rawText;
  float x = 0.0f;
  float y = 0.0f;
  float scale = 1.25f;
  float fontHeight = 78.0f;
  float alpha = 1.0f;
  uint32_t colorPacked = 0xFFFFFFFFu;
  unsigned short virtualW = 0;
  unsigned short virtualH = 0;
  int fxLetterTime = 0; // typewriter ms-per-char from HudElem snapshot
  unsigned int callerRva = 0;
  unsigned int calleeRva = 0;
  unsigned long captureTick = 0;
};

struct AuthoritativeObjectiveInstance {
  ObjectiveInstanceId instanceId;
  ObjectiveStatusNativeLine line;
  ObjectiveTextCapture textCapture;
  ObjectiveVisualCapture visualCapture;
  bool joined = false;
};

std::vector<ObjectiveOverlayEntry> TextHook_GetObjectiveEntries();
std::vector<ObjectiveStatusEvent> TextHook_GetObjectiveStatusEvents();
void TextHook_ReadAuthoritativeObjectiveSnapshot();
std::vector<ObjectiveStatusNativeLine>
TextHook_GetAuthoritativeObjectiveSnapshot();
// OSREV snapshot declarations removed — OSAUTH is the sole authority.
struct ObjectiveKeyObservation {
  std::string key;
  ObjectiveChannel channel;
  unsigned long lastSeen;
  unsigned int ordinal = 0;
  unsigned int callerOffset = 0;
};
std::vector<ObjectiveKeyObservation>
TextHook_GetRecentObjectiveKeys(unsigned long maxAgeMs);
void TextHook_ClearObjectiveEntries();
void TextHook_ClearObjectiveStatusEvents();
void TextHook_ResetObjectiveRuntimeForMapRestart();
unsigned long TextHook_GetObjectiveRuntimeResetTick();
// Clear probe bias caches and negative probe set (call on game/map reset).
void ObjRev_ClearProbeAndBiasCaches();
// Reset firstSeen/lastSeen for a specific objective key (for new-cycle detection)
void TextHook_ResetObjectiveEntryAge(const std::string& key);

struct ObjectiveConsumerSample {
  std::string key;
  unsigned short ownerdrawId;
  float x;
  float y;
  unsigned short virtualW;
  unsigned short virtualH;
  float scale;
  float alpha;
  int visibleChars;
  int totalChars;
  unsigned int callerOffset;
  unsigned int frameId;
  unsigned int tick;
};

struct HudHintConsumerSample {
  std::string key;
  unsigned int callerOffset;
  uint32_t cfgIndex;
  uint32_t slcIndex;
  uint32_t queryIndex;
  unsigned long tick;
};

struct HudHintRuntimeParamSnapshot {
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
  float color[4] = {1.0f, 1.0f, 1.0f, 0.0f};
};

void TextHook_OnObjectiveConsumerSample(const ObjectiveConsumerSample &sample);
std::vector<HudHintConsumerSample> TextHook_GetRecentHudHintConsumerSamples(
    unsigned long maxAgeMs, unsigned int maxItems = 24);
bool TextHook_GetRecentHudHintRuntimeParams(const std::string &key,
                                            unsigned int callerHint,
                                            unsigned long maxAgeMs,
                                            HudHintRuntimeParamSnapshot &out);
// QTE initial high-scale capture.  Returns true if R_AddCmdDrawText was ever
// called for this key with scale > 1.8 (within a 10-second window).
bool TextHook_ConsumeQteInitialHighScale(const std::string &key,
                                         float &outScale,
                                         unsigned long &outTick);
void TextHook_ClearQteInitialHighScale(const std::string &key);
unsigned int TextHook_RecordObjectiveStatusEvent(const std::string &key,
                                                 unsigned int callerOffset,
                                                 const char *sourceTag,
                                                 uint64_t instanceSig = 0);
bool TextHook_GetRecentObjectiveStatusProbe(unsigned long maxAgeMs,
                                            unsigned int requiredCallerOffset,
                                            std::string &outKey,
                                            uint32_t *outSlc = nullptr,
                                            bool *outHasRenderParams = nullptr);
bool TextHook_GetRecentObjectiveStatusProbeByKey(
    unsigned long maxAgeMs, const char *requiredKey,
    unsigned int requiredCallerOffset, std::string &outKey,
    uint32_t *outSlc = nullptr, bool *outHasRenderParams = nullptr,
    unsigned int *outCallerOffset = nullptr);

// Called by FSHook when HUD key is loaded. Returns true if Korean found & queued.
bool TextHook_OnHudKey(const char* key);
void TextHook_OnObjectiveLocalizeLookup(const char *key, const char *englishValue,
                                        bool loadingPhaseLikely,
                                        bool forceGameplayList);
void TextHook_RegisterRuntimeEnglishKey(const char *key,
                                        const char *englishValue);
bool TextHook_GetRuntimeEnglishForKey(const std::string &key,
                                      std::string &outEnglish);
bool TextHook_GetRuntimeEnglishForKeyRecent(const std::string &key,
                                            std::string &outEnglish,
                                            unsigned long maxAgeMs);
bool TextHook_ResolveLocalizedTemplateArgs(const std::string &key,
                                           const std::string &runtimeEnglish,
                                           std::string &inOutText);

// =============================================================================
// INTRO OVERLAY SYSTEM (Mission intro screens)
// =============================================================================

struct IntroOverlayLine {
  std::string text;       // Korean text
  std::string english;    // Original English (SLC key)
  int order;              // Display order from SLC key (_LINE_N)
  int slcIdx = -1;        // Native HudElem index (for render params)
};

struct IntroOverlaySnapshot {
  std::vector<IntroOverlayLine> lines;
  unsigned long startTime;
  unsigned long lastUpdate;
  bool active;
};

// Get current intro overlay state
IntroOverlaySnapshot TextHook_GetIntroOverlaySnapshot();

// Called by FSHook when INTROSCREEN key is loaded
void TextHook_OnIntroKey(const char* key);

// Native HudElem render state captured from engine (RBX struct at parent RSP+78)
struct NativeIntroRenderState {
  float x, y;        // IW6 virtual coords (+0x04, +0x08)
  float fontScale;    // +0x14
  uint32_t horzAlign; // +0x24
  uint32_t vertAlign; // +0x28
  uint32_t colorPacked; // +0x30: 0xAABBGGRR
  float r, g, b, a;  // extracted from colorPacked
  uint32_t slcIdx;    // +0x84
  int elemTime;       // +0x78: element server-time field (candidate for cg.time)
  int fxBirthTime;    // +0x90: typewriter start (server time)
  int fxLetterTime;   // +0x94: ms per character (30 = 33.33 CPS)
  unsigned long firstSeen;    // First tick this slcIdx appeared in render fn
  unsigned long firstVisible; // First tick alpha exceeded visibility threshold
  unsigned long lastTick;
  // Optional secondary pass captured for same slc in same frame window.
  // Used for intro title ghost/shadow pass that is rendered separately natively.
  bool hasAlt;
  float altX, altY;
  float altFontScale;
  float altR, altG, altB, altA;
  unsigned long altTick;
  bool hasAlt2;
  float alt2X, alt2Y;
  float alt2FontScale;
  float alt2R, alt2G, alt2B, alt2A;
  unsigned long alt2Tick;
  bool valid;
};

// Get per-slcIdx native render params (returns false if stale/unavailable)
bool TextHook_GetNativeIntroRender(int slcIdx, NativeIntroRenderState &out);
// Get all active native intro states
std::vector<NativeIntroRenderState> TextHook_GetAllNativeIntroStates();

// Get engine safe area offset (pixels) from base+0x706058.
// Returns {0,0} if not yet readable.
struct EngineSafeArea { float x; float y; };
EngineSafeArea TextHook_GetEngineSafeArea();

// Get engine-computed screen coordinates for intro line (from 0x3FE560 hook)
bool TextHook_GetIntroScreenCoord(int slcIdx, float &outX, float &outY,
                                  float &outXScale, float &outYScale);

// =============================================================================
// OBJECTIVE HUDELEM REVERSE ENGINEERING (Phase 1: diagnostic)
// =============================================================================
#if GHOSTSKOR_OBJECTIVE_REVERSE

// Trigger array discovery scan from a known intro HudElem pointer.
// Called from D3D11Hook Present loop at ~1s intervals.
void TextHook_RunHudElemArrayScan();

// Get the discovered array base (0 if not yet found).
struct HudElemArrayInfo {
  uintptr_t baseAddr;       // Absolute address of element [0]
  uintptr_t moduleOffset;   // baseAddr - moduleBase
  int stride;               // 0xA8 or 0xB8
  int count;                // Total elements in array
  int activeCount;          // Elements where type != 0
  int textActiveCount;      // Elements where type == 1
  bool valid;
};
HudElemArrayInfo TextHook_GetHudElemArrayInfo();

// Per-frame HudElem reading for objectives (called from Present every frame).
void TextHook_ReadLiveHudElems();

// Native objective snapshot (HudElem-backed, no lifecycle heuristics).
struct NativeObjectiveItem {
  int arrayIndex;
  uintptr_t rawPtr;
  uint64_t instanceSig;
  uint32_t cfgIndex;
  std::string key;
  float x;
  float y;
  float fontScale;
  uint32_t colorPacked;
  float alpha;
  int serverTime;
  int fxBirthTime;
  int fxLetterTime;
  int flags;
  bool validKey;
};
std::vector<NativeObjectiveItem> TextHook_GetNativeObjectiveItems();
unsigned long TextHook_GetNativeObjectiveItemsTick();

// CG_DrawHudElem context: returns the hudelem_s pointer currently being drawn
// by the engine (set by CG_DrawHudElem detour, 0 when outside that scope).
uintptr_t TextHook_GetCGDrawHudElemPtr();

// Capture the final rendered text for a HudElem (called from R_AddCmdDrawText).
void TextHook_CaptureHudElemRenderedText(uintptr_t elemPtr, const char *text);

// Extract engine-substituted variable value from captured rendered text.
// placeholderIndex: 1 for &&1, 2 for &&2, etc.
// Returns -1 if not found.
int TextHook_GetHudElemVarValue(uintptr_t elemPtr,
                                const std::string &englishTemplate,
                                int placeholderIndex);

// Get native cfg fontScale for a key (from ObjRev scanner). Returns 0 if unknown.
float TextHook_GetNativeCfgFontScale(const std::string &key);
// Get native cfg alpha for a key (from ObjRev scanner). Returns 0 if unknown.
float TextHook_GetNativeCfgAlpha(const std::string &key);
// Store native fade parameters for a key (from ObjRev scanner).
void TextHook_SetNativeFadeParams(const std::string &key,
                                   float fromAlpha, float toAlpha,
                                   int fadeStartTime, int fadeTime,
                                   int serverTime);
// Get interpolated native fade alpha for a key. Returns -1 if no data.
float TextHook_GetNativeFadeAlpha(const std::string &key);

// SLC-LIVEBRIDGE live pulse alpha — captures the actual native pulse
// oscillation for HUDHINT keys that may not flow through HintOverlayEntry.
void TextHook_SetSlcLivePulseAlpha(const std::string &key, float alpha);
float TextHook_GetSlcLivePulseAlpha(const std::string &key,
                                     unsigned long maxAgeMs = 2000);

// Objective HudElem native rendering suppression (x-coordinate offscreen).
// Moves objective HudElems to x=9999 so engine renders them offscreen while
// still processing effects (typewriter sounds).  x=9999 is applied immediately
// in ObjSuppressElem and stays permanently; the scanner reads stored original x
// from the suppress map (no borrow-restore race window).
void TextHook_ObjSuppressElem(uintptr_t elemPtr, uint32_t originalXBits);
bool TextHook_IsObjSuppressedElem(uintptr_t elemPtr);
void TextHook_ObjSuppressRestoreStale();   // Expire + restore x for stale entries
void TextHook_ObjSuppressClearAll();       // Restore all + clear (map load/reset)
void TextHook_ObjSuppressTempRestore();    // No-op (kept for ABI compat)
void TextHook_ObjSuppressReapplyOffscreen(); // No-op (kept for ABI compat)

// Live HudElem snapshot (objective lanes + hint bridge lanes).
// slcIndex can be either cfg index or direct text SLC index depending on source lane.
struct LiveHudElem {
  std::string key;       // Localization key (from SLC key map)
  uint32_t slcIndex;
  uint32_t cfgIndex;     // Resolved configstring index for objective lanes (0 if unknown)
  int arrayIndex;        // Index in hudelem array
  uintptr_t rawPtr;      // Native HudElem pointer when available
  uint64_t instanceSig;  // (cfg, fxBirth, ptr) owner identity
  uint32_t rawLabelSlc;  // Raw elem+0x80 value
  uint32_t rawTextSlc;   // Raw elem+0x84 value
  bool objectiveLane;
  bool hintBridgeLane;
  float x, y;            // Virtual coordinates (640x480)
  float fontScale;
  uint32_t color;        // Current ABGR packed
  uint32_t fromColor;    // Interpolation start color
  int fadeStartTime;     // Server time
  int fadeTime;          // Duration ms
  int fxBirthTime;       // Typewriter start (server time)
  int fxLetterTime;      // Typewriter ms/char
  int flags;
  float alpha;           // Extracted from color A channel
  bool active;
};
std::vector<LiveHudElem> TextHook_GetLiveHudElems();
std::vector<LiveHudElem> TextHook_GetRuntimeHudHintLiveElems();
bool TextHook_GetSlcKey(uint32_t slcIndex, std::string &outKey);

// CG_DrawHudElem coordinate capture for HUDHINT keys.
// Populated in Detour_CG_DrawHudElem, consumed by DedicatedHint renderer.
struct CgDrawHudElemCapture {
  float x, y;                // Raw HudElem coords (480-space)
  float fontScale;
  uint32_t alignOrg;
  uint32_t alignScreen;
  uint32_t colorPacked;      // ABGR packed
  unsigned long tick;
};
// Store a CG_DrawHudElem capture for the given key.
void TextHook_StoreCgDrawCapture(const std::string &key,
                                 const CgDrawHudElemCapture &cap);
// Try to consume the most recent CG_DrawHudElem capture for a key.
// Returns true and fills 'out' if a fresh entry (< ttlMs old) exists.
bool TextHook_GetCgDrawCapture(const std::string &key,
                               CgDrawHudElemCapture &out,
                               unsigned long ttlMs = 2000);

// Per-elemPtr capture cache: Detour stores coordinates for EVERY HudElem
// processed by CG_DrawHudElem.  SEH hook can later match elemPtr to key.
void TextHook_StoreCgDrawElemPtrCapture(uintptr_t elemPtr,
                                        const CgDrawHudElemCapture &cap);
bool TextHook_GetCgDrawElemPtrCapture(uintptr_t elemPtr,
                                      CgDrawHudElemCapture &out,
                                      unsigned long ttlMs = 500);

struct LiveHudScriptCountdown {
  std::string key;
  std::string rawText;
  std::string englishText;
  std::string renderText;
  uintptr_t rawPtr;
  uintptr_t textPtr;
  float x;
  float y;
  float fontScale;
  float timerValue;
  uint32_t horzAlign;
  uint32_t vertAlign;
  uint32_t colorPacked;
  float alpha;
  bool active;
};
std::vector<LiveHudScriptCountdown> TextHook_GetLiveHudScriptCountdowns();

struct TimeScriptRenderSnapshot {
  std::string key;
  std::string englishText;
  std::string renderText;
  float x;
  float y;
  float scale;
  float fontHeight;
  float color[4];
  int forceAlign;
  bool yIsTopOfText;
  float timerSeconds;         // remaining countdown seconds (for self-rendered timer)
  bool timerComputed;         // true = timerSeconds was actually computed (not 0 due to unknown game time)
  bool noTenths;              // true = format as m:ss (SATFARM), false = m:ss.t (CLOCKWORK)
  unsigned long lastSeenTick;
  bool active;
};
void TextHook_TimeScriptReset();
bool TextHook_TimeScriptOnIntroActualRenderCandidate(
    const char *key, const char *rawText, const char *sourceTag,
    uintptr_t stringAddr, uintptr_t rootBase, int score, int slcIdx);
bool TextHook_GetTimeScriptRenderSnapshot(TimeScriptRenderSnapshot &outState);
bool TextHook_HasTimeScriptTimerReplacement();
void TextHook_SuppressActiveCountdownLabel();
void TextHook_TimeScriptCaptureRenderedTimer(const std::string &timerTail,
                                           const std::string &expectedKey);
void TextHook_TimeScriptDiscoverFallbackTimer();
int  TextHook_ReadSPGameTime();  // Returns current SP game time in ms, 0 if unknown
int  TextHook_GetCountdownTimeField();  // Returns HudElem time field (absolute end ms)
void TextHook_ResetGameTimeDiscovery();  // Invalidate discovered game time, trigger re-scan

#endif // GHOSTSKOR_OBJECTIVE_REVERSE




// Soft SLC suspend: disable only the SLC raw hook (for ui_play_credits loading phase)
void TextHook_SoftSuspendSLC();
void TextHook_SoftResumeSLC();

// Credits hook suspension: disable/re-enable ALL hooks during credits
void TextHook_SuspendHooksForCredits();
void TextHook_ResumeHooksAfterCredits();
