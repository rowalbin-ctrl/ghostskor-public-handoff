#pragma once

#include "TextHook.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ObjectiveRuntime {

struct OverlayState {
  std::vector<ObjectiveOverlayEntry> entries;
  std::unordered_map<std::string, ObjectiveKeyObservation> keyObservations;
  std::unordered_map<std::string, unsigned long> loadStickyUntil;
};

struct StatusState {
  std::deque<ObjectiveStatusEvent> events;
  unsigned int sequence = 1;
};

struct TopLeftObjectiveRuntimeState {
  std::unordered_map<uint64_t, ObjectiveStatusNativeOwner> owners;
  std::unordered_map<std::string, unsigned long> recentNativeStatusKeySeenTick;
  std::atomic<uint32_t> generation{1};
};


void WithOverlayState(const std::function<void(OverlayState &)> &fn);
std::vector<ObjectiveOverlayEntry>
GetOverlayEntriesSnapshot(unsigned long now, unsigned long timeoutMs,
                         bool consumerOwnedOnly);
std::vector<ObjectiveKeyObservation>
GetRecentObjectiveKeys(unsigned long now, unsigned long maxAgeMs);
void ClearOverlayState();
void ResetObjectiveEntryAge(const std::string &key, unsigned long now);

void WithStatusState(const std::function<void(StatusState &)> &fn);
std::vector<ObjectiveStatusEvent>
GetStatusEventsSnapshot(unsigned long now, unsigned long resetTick,
                        unsigned long retentionMs);
void ClearStatusState(size_t &outPrevCount, unsigned int &outPrevSequence);

void WithTopLeftObjectiveRuntimeState(
    const std::function<void(TopLeftObjectiveRuntimeState &)> &fn);
std::vector<ObjectiveStatusNativeOwner> GetTopLeftOwnerSnapshot();
bool HasActiveTopLeftOwnerForKey(const std::string &key);
bool HasRecentTopLeftStatusOwnerForKey(const std::string &key,
                                       unsigned long maxAgeMs);
void ClearTopLeftObjectiveRuntimeState();
uint32_t GetTopLeftObjectiveGeneration();


uint64_t NextRuntimeInstanceId();
uint32_t NextGeneration();

void SetNativeObjectiveItems(std::vector<NativeObjectiveItem> items,
                             unsigned long tick);
std::vector<NativeObjectiveItem> GetNativeObjectiveItems();
unsigned long GetNativeObjectiveItemsTick();
void ClearNativeObjectiveItems(unsigned long tick);

void SetRuntimeResetTick(unsigned long tick);
unsigned long GetRuntimeResetTick();

void SetLastObjectiveLaneActivity(unsigned long tick);
bool HasRecentObjectiveLaneActivity(unsigned long now, unsigned long maxAgeMs);

} // namespace ObjectiveRuntime
