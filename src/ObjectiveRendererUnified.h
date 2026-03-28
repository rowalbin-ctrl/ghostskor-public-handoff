#pragma once
// ObjectiveRendererUnified.h — Single-path objective/status rendering system.
//
// DESIGN PRINCIPLE:
//   One capture path → one data store → one render loop.
//   No OSAUTH, no multi-path, no "authority join", no "owner eviction".
//
// DATA FLOW:
//   SLC Hook (RDI = hudelem_s*)  →  ObjUnified_CaptureFromSLC()
//   FSHook (save events)         →  ObjUnified_RecordStatusEvent()
//   HudElem array scan (alpha)   →  ObjUnified_UpdateFromLiveHudElems()
//   D3D11 Present()              →  ObjUnified_Render()
//
// CLASSIFICATION:
//   All key classification goes through HudClassify.h — no duplicates.

#include "HudClassify.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================================
// Core data structures
// =============================================================================

// A captured objective or status item from the SLC hook.
// Populated at the exact moment the engine resolves the text string.
struct ObjUnifiedItem {
  // Identity
  uintptr_t   elemPtr = 0;           // RDI value = hudelem_s pointer
  uint32_t    cfgIndex = 0;          // configstring index (+0x84)
  int         fxBirthTime = 0;       // typewriter start time (+0x90)

  // Classification
  std::string key;                   // localization key (e.g. "CLOCKWORK_OBJ_INTO_BASE")
  HudKeyType  keyType = HUD_KEY_UNKNOWN;

  // Visual state (from hudelem_s at capture time)
  float       x = 0.0f;             // virtual x (+0x04)
  float       y = 0.0f;             // virtual y (+0x08) — determines lane
  float       fontScale = 1.25f;    // (+0x14)
  uint32_t    colorPacked = 0xFFFFFFFF; // ABGR (+0x30)
  float       alpha = 1.0f;         // extracted from colorPacked A channel
  int         fxLetterTime = 0;     // typewriter speed (+0x94)
  int         fadeStartTime = 0;    // (+0x38)
  int         fadeTime = 0;         // (+0x3C)
  uint32_t    flags = 0;            // (+0xA4)
  uint32_t    alignOrg = 0;         // (+0x28)

  // Timing
  unsigned long firstSeenTick = 0;  // GetTickCount() when first captured
  unsigned long lastSeenTick = 0;   // GetTickCount() of last SLC/live update
  unsigned long lastLiveAlphaTick = 0; // last frame with alpha > 0

  // Computed lane slot
  int         laneSlot = 0;         // (int)round((y - 10) / 20)

  // State
  bool        valid = false;
  bool        removedByAlpha = false; // marked for removal (alpha=0 too long)
};

// A status event from FSHook or SLC probe (transient messages).
struct ObjUnifiedStatusEvent {
  std::string   key;
  HudKeyType    keyType = HUD_KEY_UNKNOWN;
  unsigned int  sequence = 0;       // unique per event
  unsigned long startTick = 0;      // GetTickCount() when recorded
  uint32_t      sourceCfg = 0;      // cfgIndex if available
};

// =============================================================================
// Public API
// =============================================================================

// Called from SLC hook when a HUD text is resolved.
// elemPtr = RDI (hudelem_s pointer), key = resolved localization key.
void ObjUnified_CaptureFromSLC(
    uintptr_t elemPtr,
    const std::string &key,
    uint32_t cfgIndex,
    uint32_t slcIndex,
    unsigned int callerOffset);

// Called from FSHook or SLC probe for status events.
// Returns the assigned sequence number.
unsigned int ObjUnified_RecordStatusEvent(
    const std::string &key,
    uint32_t sourceCfg,
    const char *sourceTag);

// Called per-frame to update alpha/position from live HudElem memory.
void ObjUnified_UpdateFromLiveHudElems();

// Main render function — called from D3D11 Present().
// This is the ONE AND ONLY place where objectives/status are rendered.
void ObjUnified_Render(
    void *pSwapChain,  // IDXGISwapChain*
    unsigned long now,
    bool suppressHudOverlay,
    bool hudPauseMenu,
    bool objectiveHiddenByGate);

// Called on mission restart / menu exit.
void ObjUnified_Reset();

// Called on map load to clear stale data.
void ObjUnified_OnMapLoad();

// Get current items snapshot (for debug/logging only).
std::vector<ObjUnifiedItem> ObjUnified_GetItemsSnapshot();
std::vector<ObjUnifiedStatusEvent> ObjUnified_GetStatusEventsSnapshot();

// Returns true if ObjUnified_Render() rendered this key in the current frame.
// Used by the hint pipeline to suppress duplicate rendering.
bool ObjUnified_WasKeyRenderedThisFrame(const std::string &key);
