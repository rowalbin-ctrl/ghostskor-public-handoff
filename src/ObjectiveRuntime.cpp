#include "ObjectiveRuntime.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <Windows.h>

namespace ObjectiveRuntime {
namespace {

OverlayState g_OverlayState;
std::mutex g_OverlayMutex;

StatusState g_StatusState;
std::mutex g_StatusMutex;

TopLeftObjectiveRuntimeState g_TopLeftObjectiveRuntimeState;
std::mutex g_NativeOwnerMutex;

std::vector<NativeObjectiveItem> g_NativeObjectiveItems;
std::mutex g_NativeObjectiveItemsMutex;
std::atomic<unsigned long> g_NativeObjectiveItemsLastUpdate{0};

std::atomic<uint64_t> g_RuntimeInstanceSeed{1};
std::atomic<uint32_t> g_GenerationSeed{1};
std::atomic<unsigned long> g_RuntimeResetTick{0};
std::atomic<unsigned long> g_LastObjectiveLaneActivityTime{0};

} // namespace

void WithOverlayState(const std::function<void(OverlayState &)> &fn) {
  std::lock_guard<std::mutex> lock(g_OverlayMutex);
  fn(g_OverlayState);
}

std::vector<ObjectiveOverlayEntry>
GetOverlayEntriesSnapshot(unsigned long now, unsigned long timeoutMs,
                         bool consumerOwnedOnly) {
  std::lock_guard<std::mutex> lock(g_OverlayMutex);

  g_OverlayState.entries.erase(
      std::remove_if(
          g_OverlayState.entries.begin(), g_OverlayState.entries.end(),
          [now, timeoutMs](const ObjectiveOverlayEntry &entry) {
            if ((now - entry.lastSeen) <= timeoutMs) {
              return false;
            }
            auto itSticky =
                g_OverlayState.loadStickyUntil.find(entry.key);
            if (itSticky != g_OverlayState.loadStickyUntil.end() &&
                now <= itSticky->second) {
              return false;
            }
            return true;
          }),
      g_OverlayState.entries.end());

  for (auto it = g_OverlayState.loadStickyUntil.begin();
       it != g_OverlayState.loadStickyUntil.end();) {
    if (now > it->second) {
      it = g_OverlayState.loadStickyUntil.erase(it);
    } else {
      ++it;
    }
  }

  if (!consumerOwnedOnly) {
    return g_OverlayState.entries;
  }

  std::vector<ObjectiveOverlayEntry> out;
  out.reserve(g_OverlayState.entries.size());
  for (const auto &entry : g_OverlayState.entries) {
    if (entry.consumerOwned) {
      out.push_back(entry);
    }
  }
  return out;
}

std::vector<ObjectiveKeyObservation>
GetRecentObjectiveKeys(unsigned long now, unsigned long maxAgeMs) {
  constexpr unsigned long kObservationRetentionMs = 12000;

  std::lock_guard<std::mutex> lock(g_OverlayMutex);
  for (auto it = g_OverlayState.keyObservations.begin();
       it != g_OverlayState.keyObservations.end();) {
    if ((now - it->second.lastSeen) > kObservationRetentionMs) {
      it = g_OverlayState.keyObservations.erase(it);
    } else {
      ++it;
    }
  }

  std::vector<ObjectiveKeyObservation> out;
  out.reserve(g_OverlayState.keyObservations.size());
  for (const auto &entry : g_OverlayState.keyObservations) {
    if ((now - entry.second.lastSeen) <= maxAgeMs) {
      out.push_back(entry.second);
    }
  }
  return out;
}

void ClearOverlayState() {
  std::lock_guard<std::mutex> lock(g_OverlayMutex);
  g_OverlayState.entries.clear();
  g_OverlayState.keyObservations.clear();
  g_OverlayState.loadStickyUntil.clear();
}

void ResetObjectiveEntryAge(const std::string &key, unsigned long now) {
  std::lock_guard<std::mutex> lock(g_OverlayMutex);
  for (auto &entry : g_OverlayState.entries) {
    if (entry.key == key) {
      entry.firstSeen = now;
      entry.lastSeen = now;
      entry.lastFSHookSeen = now;
      break;
    }
  }
}

void WithStatusState(const std::function<void(StatusState &)> &fn) {
  std::lock_guard<std::mutex> lock(g_StatusMutex);
  fn(g_StatusState);
}

std::vector<ObjectiveStatusEvent>
GetStatusEventsSnapshot(unsigned long now, unsigned long resetTick,
                        unsigned long retentionMs) {
  std::lock_guard<std::mutex> lock(g_StatusMutex);
  for (auto it = g_StatusState.events.begin();
       it != g_StatusState.events.end();) {
    const bool staleBeforeReset =
        (resetTick != 0 && it->triggerTick != 0 && it->triggerTick < resetTick);
    if (staleBeforeReset ||
        (now >= it->triggerTick && (now - it->triggerTick) > retentionMs)) {
      it = g_StatusState.events.erase(it);
    } else {
      ++it;
    }
  }
  return std::vector<ObjectiveStatusEvent>(g_StatusState.events.begin(),
                                           g_StatusState.events.end());
}

void ClearStatusState(size_t &outPrevCount, unsigned int &outPrevSequence) {
  std::lock_guard<std::mutex> lock(g_StatusMutex);
  outPrevCount = g_StatusState.events.size();
  outPrevSequence = g_StatusState.sequence;
  g_StatusState.events.clear();
  g_StatusState.sequence = 1;
}

void WithTopLeftObjectiveRuntimeState(
    const std::function<void(TopLeftObjectiveRuntimeState &)> &fn) {
  std::lock_guard<std::mutex> lock(g_NativeOwnerMutex);
  fn(g_TopLeftObjectiveRuntimeState);
}

std::vector<ObjectiveStatusNativeOwner> GetTopLeftOwnerSnapshot() {
  std::lock_guard<std::mutex> lock(g_NativeOwnerMutex);
  std::vector<ObjectiveStatusNativeOwner> out;
  out.reserve(g_TopLeftObjectiveRuntimeState.owners.size());
  for (const auto &kv : g_TopLeftObjectiveRuntimeState.owners) {
    out.push_back(kv.second);
  }
  return out;
}

bool HasActiveTopLeftOwnerForKey(const std::string &key) {
  if (key.empty()) {
    return false;
  }
  const unsigned long now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_NativeOwnerMutex);
  // Read-only scan: check for key match without modifying the map.
  // Stale entries are cleaned up during write paths (WithTopLeftObjectiveRuntimeState,
  // HasRecentTopLeftStatusOwnerForKey, ClearTopLeftObjectiveRuntimeState).
  for (const auto &kv : g_TopLeftObjectiveRuntimeState.owners) {
    const ObjectiveStatusNativeOwner &owner = kv.second;
    bool active = true;
    if (owner.ownerKind == OBJSTAT_OWNER_STATUS_PROBE) {
      active = (owner.line.lifeEnd == 0 || now <= owner.line.lifeEnd);
    } else if (owner.lastSeenTick != 0 && now >= owner.lastSeenTick) {
      const unsigned long maxAge =
          (owner.channel == OBJSTAT_NATIVE_OBJECTIVE) ? 48UL : 250UL;
      active = (now - owner.lastSeenTick) <= maxAge;
    }
    if (active && owner.key == key) {
      return true;
    }
  }
  return false;
}

bool HasRecentTopLeftStatusOwnerForKey(const std::string &key,
                                       unsigned long maxAgeMs) {
  if (key.empty()) {
    return false;
  }
  const unsigned long now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_NativeOwnerMutex);

  for (auto it = g_TopLeftObjectiveRuntimeState.recentNativeStatusKeySeenTick.begin();
       it != g_TopLeftObjectiveRuntimeState.recentNativeStatusKeySeenTick.end();) {
    const unsigned long age =
        (now >= it->second) ? (now - it->second) : 0;
    if (age > 10000UL) {
      it = g_TopLeftObjectiveRuntimeState.recentNativeStatusKeySeenTick.erase(it);
    } else {
      ++it;
    }
  }

  auto itRecent = g_TopLeftObjectiveRuntimeState.recentNativeStatusKeySeenTick.find(key);
  if (itRecent != g_TopLeftObjectiveRuntimeState.recentNativeStatusKeySeenTick.end()) {
    const unsigned long age =
        (now >= itRecent->second) ? (now - itRecent->second) : 0;
    if (age <= maxAgeMs) {
      return true;
    }
  }

  for (auto it = g_TopLeftObjectiveRuntimeState.owners.begin();
       it != g_TopLeftObjectiveRuntimeState.owners.end();) {
    const ObjectiveStatusNativeOwner &owner = it->second;
    bool active = true;
    if (owner.ownerKind == OBJSTAT_OWNER_STATUS_PROBE) {
      active = (owner.line.lifeEnd == 0 || now <= owner.line.lifeEnd);
    } else if (owner.lastSeenTick != 0 && now >= owner.lastSeenTick) {
      const unsigned long maxAge =
          (owner.channel == OBJSTAT_NATIVE_OBJECTIVE) ? 24UL : 250UL;
      active = (now - owner.lastSeenTick) <= maxAge;
    }

    if (!active) {
      if (owner.channel == OBJSTAT_NATIVE_STATUS &&
          owner.ownerKind != OBJSTAT_OWNER_STATUS_PROBE && !owner.key.empty()) {
        g_TopLeftObjectiveRuntimeState.recentNativeStatusKeySeenTick[owner.key] =
            (owner.lastSeenTick != 0) ? owner.lastSeenTick : now;
      }
      it = g_TopLeftObjectiveRuntimeState.owners.erase(it);
      continue;
    }
    if (owner.channel == OBJSTAT_NATIVE_STATUS &&
        owner.ownerKind != OBJSTAT_OWNER_STATUS_PROBE && owner.key == key) {
      return true;
    }
    ++it;
  }
  return false;
}

void ClearTopLeftObjectiveRuntimeState() {
  std::lock_guard<std::mutex> lock(g_NativeOwnerMutex);
  g_TopLeftObjectiveRuntimeState.owners.clear();
  g_TopLeftObjectiveRuntimeState.recentNativeStatusKeySeenTick.clear();
  uint32_t gen =
      g_TopLeftObjectiveRuntimeState.generation.fetch_add(
          1, std::memory_order_release) +
      1;
  if (gen == 0) {
    g_TopLeftObjectiveRuntimeState.generation.store(
        1, std::memory_order_release);
  }
}

uint32_t GetTopLeftObjectiveGeneration() {
  return g_TopLeftObjectiveRuntimeState.generation.load(
      std::memory_order_acquire);
}


uint64_t NextRuntimeInstanceId() {
  uint64_t id = g_RuntimeInstanceSeed.fetch_add(1);
  if (id == 0) {
    id = g_RuntimeInstanceSeed.fetch_add(1);
  }
  return id;
}

uint32_t NextGeneration() {
  uint32_t generation = g_GenerationSeed.fetch_add(1);
  if (generation == 0) {
    generation = g_GenerationSeed.fetch_add(1);
  }
  return generation;
}

void SetNativeObjectiveItems(std::vector<NativeObjectiveItem> items,
                             unsigned long tick) {
  std::lock_guard<std::mutex> lock(g_NativeObjectiveItemsMutex);
  g_NativeObjectiveItems = std::move(items);
  g_NativeObjectiveItemsLastUpdate.store(tick, std::memory_order_release);
}

std::vector<NativeObjectiveItem> GetNativeObjectiveItems() {
  std::lock_guard<std::mutex> lock(g_NativeObjectiveItemsMutex);
  return g_NativeObjectiveItems;
}

unsigned long GetNativeObjectiveItemsTick() {
  return g_NativeObjectiveItemsLastUpdate.load(std::memory_order_acquire);
}

void ClearNativeObjectiveItems(unsigned long tick) {
  std::lock_guard<std::mutex> lock(g_NativeObjectiveItemsMutex);
  g_NativeObjectiveItems.clear();
  g_NativeObjectiveItemsLastUpdate.store(tick, std::memory_order_release);
}

void SetRuntimeResetTick(unsigned long tick) {
  g_RuntimeResetTick.store(tick, std::memory_order_relaxed);
}

unsigned long GetRuntimeResetTick() {
  return g_RuntimeResetTick.load(std::memory_order_relaxed);
}

void SetLastObjectiveLaneActivity(unsigned long tick) {
  g_LastObjectiveLaneActivityTime.store(tick, std::memory_order_relaxed);
}

bool HasRecentObjectiveLaneActivity(unsigned long now, unsigned long maxAgeMs) {
  const unsigned long last =
      g_LastObjectiveLaneActivityTime.load(std::memory_order_relaxed);
  if (last == 0) {
    return false;
  }
  return (now - last) <= maxAgeMs;
}

} // namespace ObjectiveRuntime
