
// =============================================================================
// HUD OVERLAY GETTER - For D3D11Hook to render HUD translations
// =============================================================================
namespace {
static uint32_t OverlayHash32FNV1a(const std::string &text) {
  uint32_t h = 2166136261u;
  for (unsigned char c : text) {
    h ^= (uint32_t)c;
    h *= 16777619u;
  }
  return h;
}

static std::string OverlayNormalizeTextForHash(const OverlayDrawEvent &event) {
  std::string seed = event.english.empty() ? event.key : event.english;
  seed = TrimSpaces(StripColorCodes(seed));
  seed = ToUpper(seed);
  return seed;
}

static int OverlaySlotFromEvent(const OverlayDrawEvent &e) {
  const int packedSlot = (int)(short)((unsigned int)((e.flags >> 16) & 0xFFFFu));
  if (packedSlot != 0) {
    return packedSlot;
  }
  if (e.virtualH > 0) {
    const float ny = e.y / (float)e.virtualH;
    if (ny < 0.18f) return 1;
    if (ny < 0.42f) return 2;
    if (ny < 0.62f) return 3;
    if (ny < 0.78f) return 4;
    return 5;
  }
  return 0;
}

static int OverlaySourcePriority(OverlaySource src) {
  switch (src) {
  case OVERLAY_SOURCE_HUD560:
    return 400;
  case OVERLAY_SOURCE_ADDCMD:
    return 300;
  case OVERLAY_SOURCE_SLC:
    return 200;
  case OVERLAY_SOURCE_HUDELEM_REVERSE:
    return 150;
  case OVERLAY_SOURCE_FSHOOK:
    return 100;
  default:
    return 0;
  }
}

struct OverlaySemanticKey {
  uint8_t track = 0;
  uint32_t keyHash = 0;
  uint32_t textHash = 0;

  bool operator==(const OverlaySemanticKey &o) const {
    return track == o.track && keyHash == o.keyHash && textHash == o.textHash;
  }
};

struct OverlaySemanticKeyHash {
  size_t operator()(const OverlaySemanticKey &k) const {
    size_t h = (size_t)k.track;
    h = (h * 2654435761u) ^ (size_t)k.keyHash;
    h = (h * 40503u) ^ (size_t)k.textHash;
    return h;
  }
};

static uint32_t OverlayNormalizedKeyHash(const OverlayDrawEvent &event) {
  std::string keyNorm = TrimSpaces(ToUpper(event.key));
  if (keyNorm.empty()) {
    return 0;
  }
  return OverlayHash32FNV1a(keyNorm);
}
} // namespace

void TextHook_PublishOverlayEvent(const OverlayDrawEvent &inEvent) {
  OverlayDrawEvent event = inEvent;
  if (event.key.empty() && event.english.empty()) {
    return;
  }
  if (event.source == OVERLAY_SOURCE_SLC &&
      event.routeChannel != HUD_ROUTE_OBJECTIVE) {
    // Native-only HUD hint/object-hint mode:
    // ignore SLC visual events outside objective authority track.
    return;
  }
  if (event.tick == 0) {
    event.tick = GetTickCount();
  }

    const std::string norm = OverlayNormalizeTextForHash(event);
  const uint32_t textHash = OverlayHash32FNV1a(norm);
  uint32_t keyHash = OverlayNormalizedKeyHash(event);
  if (keyHash == 0) {
    keyHash = textHash;
  }
  event.normalizedTextHash = textHash;
  event.normalizedKeyHash = keyHash;
  event.slotCached = (short)OverlaySlotFromEvent(event);

  {
    std::lock_guard<std::mutex> lock(g_OverlayBusMutex);
    g_OverlayEventBus.push_back(event);
    while (g_OverlayEventBus.size() > 1024) {
      g_OverlayEventBus.pop_front();
    }
  }

  #if GHOSTSKOR_RUNTIME_DIAG
  static std::mutex s_evtLogMtx;
  static std::unordered_map<std::string, DWORD> s_evtLogTick;
  const std::string sig = event.key + "|" + std::to_string((int)event.source);
  bool shouldLog = false;
  {
    std::lock_guard<std::mutex> lk(s_evtLogMtx);
    auto it = s_evtLogTick.find(sig);
    if (it == s_evtLogTick.end() || (event.tick - it->second) > 3000) {
      s_evtLogTick[sig] = event.tick;
      shouldLog = true;
    }
    if (s_evtLogTick.size() > 512) {
      for (auto it = s_evtLogTick.begin(); it != s_evtLogTick.end();) {
        if ((event.tick - it->second) > 45000) {
          it = s_evtLogTick.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  if (shouldLog) {
    char buf[640];
    std::string preview = TrimSpaces(StripColorCodes(event.english));
    if (preview.size() > 92) {
      preview.resize(92);
    }
    sprintf_s(buf,
              "[OVL-EVT] src=%d caller=+0x%X key=%s hash=0x%08X consumer=0x%llX "
              "route=%d conf=%.2f x=%.1f y=%.1f vh=%u ch=%d text=\"%s\"",
              (int)event.source, event.callerOffset, event.key.c_str(),
              event.normalizedTextHash,
              (unsigned long long)event.consumerId, (int)event.routeChannel,
              event.routeConfidence, event.x, event.y, event.virtualH,
              (int)event.channelHint, preview.c_str());
    LogToFile(buf);
  }
#endif
}

void TextHook_BeginOverlayFrame(uint64_t frameId, OverlaySessionState state) {
  const DWORD now = GetTickCount();
  bool frontendEnter = false;
  bool gameplayEnterSync = false;
  bool sessionChanged = false;
  OverlaySessionState prevState = OVERLAY_SESSION_GAMEPLAY;

  {
    std::lock_guard<std::mutex> lock(g_OverlayBusMutex);
    g_OverlayPrevSessionState = g_OverlaySessionState;
    prevState = g_OverlayPrevSessionState;
    sessionChanged = (prevState != state);
    g_OverlaySessionState = state;
    if (state == OVERLAY_SESSION_FRONTEND &&
        g_OverlayPrevSessionState != OVERLAY_SESSION_FRONTEND) {
      frontendEnter = true;
    }
    if (state == OVERLAY_SESSION_GAMEPLAY &&
        (prevState == OVERLAY_SESSION_FRONTEND ||
         prevState == OVERLAY_SESSION_RESTARTING)) {
      gameplayEnterSync = true;
    }
  }

  if (sessionChanged) {
    const bool front = (state == OVERLAY_SESSION_FRONTEND);
    const bool pause = (state == OVERLAY_SESSION_PAUSE);
    const bool objHot = TextHook_HasRecentObjectiveLaneActivity(1200);
    char sbuf[192];
    sprintf_s(sbuf, "[OVL-SESSION] prev=%d now=%d front=%d pause=%d objHot=%d",
              (int)prevState, (int)state, front ? 1 : 0, pause ? 1 : 0,
              objHot ? 1 : 0);
    LogToFile(sbuf);
  }

  if (frontendEnter) {
    TextHook_ResetOverlayRuntime(OVERLAY_RESET_FRONTEND_ENTER);
    std::lock_guard<std::mutex> lock(g_OverlayBusMutex);
    g_OverlaySkipRenderFrame = frameId;
  }
  if (gameplayEnterSync) {
    // Guard against stale status events surviving session edge ordering.
    TextHook_ClearObjectiveStatusEvents();
  }

  struct ClaimCandidate {
    OverlayDrawEvent event;
    OverlayInstanceId iid;
    OverlayTrack track;
    int score = 0;
    std::string reason;
  };

  std::vector<ClaimCandidate> candidates;
  {
    std::lock_guard<std::mutex> lock(g_OverlayBusMutex);
    DWORD ttlMs = 700;
    if (state == OVERLAY_SESSION_PAUSE) {
      ttlMs = 450;
    } else if (state == OVERLAY_SESSION_WARMUP) {
      ttlMs = 350;
    }
    while (!g_OverlayEventBus.empty()) {
      const OverlayDrawEvent &front = g_OverlayEventBus.front();
      if ((now - front.tick) <= ttlMs) {
        break;
      }
      g_OverlayEventBus.pop_front();
    }
    candidates.reserve(g_OverlayEventBus.size());

    for (const auto &e : g_OverlayEventBus) {
            const uint32_t textHash =
          (e.normalizedTextHash != 0)
              ? e.normalizedTextHash
              : OverlayHash32FNV1a(OverlayNormalizeTextForHash(e));
      const int slotOrElem =
          (int)e.slotCached ? (int)e.slotCached : OverlaySlotFromEvent(e);
      const OverlayInstanceId iid{
          e.source, e.callerOffset, slotOrElem, e.fxBirth, textHash,
          e.consumerId};

            if (e.routeChannel == HUD_ROUTE_IGNORE ||
          e.routeChannel == HUD_ROUTE_UNRESOLVED) {
        if (e.routeChannel == HUD_ROUTE_UNRESOLVED) {
#if GHOSTSKOR_RUNTIME_DIAG
          static std::unordered_map<std::string, DWORD> s_unresolvedDropLog;
          const std::string dkey =
              e.key + "|" + std::to_string((unsigned long long)e.consumerId);
          auto itDrop = s_unresolvedDropLog.find(dkey);
          if (itDrop == s_unresolvedDropLog.end() ||
              (now - itDrop->second) > 2000) {
            s_unresolvedDropLog[dkey] = now;
            char dbuf[512];
            sprintf_s(
                dbuf,
                "[OVL-DROP] reason=unresolved key=%s src=%d caller=+0x%X slot=%d consumer=0x%llX",
                e.key.c_str(), (int)e.source, e.callerOffset, slotOrElem,
                (unsigned long long)e.consumerId);
            LogToFile(dbuf);
          }
#endif
        }
        continue;
      }

      OverlayTrack forcedTrack = OVERLAY_TRACK_HINT;
      bool forcedByAuthority = false;
      if (e.routeChannel == HUD_ROUTE_OBJECTIVE) {
        forcedTrack = OVERLAY_TRACK_OBJECTIVE;
        forcedByAuthority = true;
      } else if (e.routeChannel == HUD_ROUTE_HINT) {
        forcedTrack = OVERLAY_TRACK_HINT;
        forcedByAuthority = true;
      } else if (e.routeChannel == HUD_ROUTE_OBJHUDHINT) {
        forcedTrack = OVERLAY_TRACK_OBJHUDHINT;
        forcedByAuthority = true;
      }

            if (!forcedByAuthority) {
#if GHOSTSKOR_RUNTIME_DIAG
        static std::unordered_map<std::string, DWORD> s_dropNoAuthTick;
        const std::string dkey =
            e.key + "|" + std::to_string((int)e.source) + "|noauth";
        auto it = s_dropNoAuthTick.find(dkey);
        if (it == s_dropNoAuthTick.end() || (now - it->second) > 2500) {
          s_dropNoAuthTick[dkey] = now;
          char dbuf[512];
          sprintf_s(dbuf,
                    "[OVL-DROP] reason=no_route_authority key=%s src=%d caller=+0x%X consumer=0x%llX",
                    e.key.c_str(), (int)e.source, e.callerOffset,
                    (unsigned long long)e.consumerId);
          LogToFile(dbuf);
        }
#endif
        continue;
      }

      const OverlayTrack bestTrack = forcedTrack;
      const int bestScore = 1000 + (int)(e.routeConfidence * 100.0f);

      ClaimCandidate c{};
      c.event = e;
      c.iid = iid;
      c.track = bestTrack;
      c.score = bestScore;
      c.reason = std::string("forced_route=") +
                 std::to_string((int)e.routeChannel) + ",slot=" +
                 std::to_string(slotOrElem) + ",consumer=0x" +
                 std::to_string((unsigned long long)e.consumerId);
      candidates.push_back(std::move(c));
    }

    // Per-instance dedup only: same text may legitimately appear in multiple
    // tracks if instance ids differ.
    std::unordered_map<uint64_t, ClaimCandidate> bestByInstance;
    bestByInstance.reserve(candidates.size() * 2 + 8);

    auto makeInstanceKey = [](const OverlayInstanceId &iid) -> uint64_t {
      uint64_t k = (iid.consumerId != 0)
                       ? iid.consumerId
                       : (((uint64_t)(iid.callerOffset & 0xFFFFFFu) << 8) ^
                          (uint64_t)((int)iid.slotOrElemIndex & 0xFF));
      k ^= ((uint64_t)((uint32_t)(iid.slotOrElemIndex & 0xFFFF)) << 40);
      k ^= ((uint64_t)((uint32_t)iid.fxBirth & 0xFFFF) << 24);
      k ^= (uint64_t)iid.textHash;
      return k;
    };

    for (const auto &c : candidates) {
      const uint64_t ikey = makeInstanceKey(c.iid);
      auto itInst = bestByInstance.find(ikey);
      if (itInst == bestByInstance.end() ||
          c.score > itInst->second.score ||
          (c.score == itInst->second.score &&
           c.event.tick > itInst->second.event.tick)) {
        bestByInstance[ikey] = c;
      }
    }

    std::vector<ClaimCandidate> chosen;
    chosen.reserve(bestByInstance.size());
    for (const auto &kv : bestByInstance) {
      chosen.push_back(kv.second);
    }

        // Cross-consumer key dedup:
    // one semantic key (same route+key+text) -> one claim.
    // Priority: sticky winner > source (HUD560 > ADDCMD > others) >
    // route confidence > newest tick.
    static std::unordered_map<OverlaySemanticKey, std::pair<uint64_t, DWORD>,
                              OverlaySemanticKeyHash>
        s_semanticWinner;
#if GHOSTSKOR_RUNTIME_DIAG
    static std::unordered_map<uint64_t, DWORD> s_dupLogTick;
#endif
    const DWORD kStickyMs = 1800;
    const DWORD kPruneMs = 12000;

    auto makeSemanticKey = [](const ClaimCandidate &c) -> OverlaySemanticKey {
      OverlaySemanticKey sem{};
      sem.track = (uint8_t)c.track;
      sem.textHash = c.iid.textHash;
      sem.keyHash = c.event.normalizedKeyHash;
      if (sem.keyHash == 0) {
        std::string keyNorm = TrimSpaces(ToUpper(c.event.key));
        sem.keyHash =
            keyNorm.empty() ? sem.textHash : OverlayHash32FNV1a(keyNorm);
      }
      if (sem.keyHash == 0) {
        sem.keyHash = sem.textHash;
      }
      return sem;
    };

    auto betterCandidate = [&](const ClaimCandidate &cand,
                               const ClaimCandidate &incumbent,
                               uint64_t stickyConsumer) -> bool {
      const int candSrcPri = OverlaySourcePriority(cand.event.source);
      const int incSrcPri = OverlaySourcePriority(incumbent.event.source);
      if (candSrcPri != incSrcPri) {
        return candSrcPri > incSrcPri;
      }

      const int candVirt =
          (cand.event.virtualH > 0 || cand.event.virtualW > 0) ? 1 : 0;
      const int incVirt =
          (incumbent.event.virtualH > 0 || incumbent.event.virtualW > 0) ? 1 : 0;
      if (candVirt != incVirt) {
        return candVirt > incVirt;
      }

      // Same producer but different slot bucket can happen from noisy captures
      // (e.g. mantle occasionally emits a spurious slot-1 sample at origin).
      // Prefer lower-screen slot for gameplay hint track.
      if (cand.track == incumbent.track && cand.track == OVERLAY_TRACK_HINT &&
          cand.event.source == incumbent.event.source &&
          cand.event.callerOffset == incumbent.event.callerOffset &&
          cand.iid.slotOrElemIndex != incumbent.iid.slotOrElemIndex) {
        return cand.iid.slotOrElemIndex > incumbent.iid.slotOrElemIndex;
      }

      // Sticky winner is a tie-breaker only.
      // Source authority (HUD560 > ADDCMD > SLC) must stay dominant to prevent
      // AddCmd latch from reintroducing duplicate/misaligned HUD draws.
      if (stickyConsumer != 0) {
        const bool candSticky = (cand.iid.consumerId == stickyConsumer);
        const bool incSticky = (incumbent.iid.consumerId == stickyConsumer);
        if (candSticky != incSticky) {
          return candSticky;
        }
      }

      if (cand.score != incumbent.score) {
        return cand.score > incumbent.score;
      }

      return cand.event.tick > incumbent.event.tick;
    };

    std::unordered_map<OverlaySemanticKey, size_t, OverlaySemanticKeyHash>
        bestBySemantic;
    bestBySemantic.reserve(chosen.size() * 2 + 8);
    std::vector<unsigned char> keep(chosen.size(), 1);

#if GHOSTSKOR_RUNTIME_DIAG
    auto makeDupLogKey = [](const OverlaySemanticKey &sem,
                            uint64_t droppedConsumer,
                            uint64_t keptConsumer) -> uint64_t {
      uint64_t sig = ((uint64_t)sem.textHash << 32) ^ (uint64_t)sem.keyHash;
      sig ^= ((uint64_t)sem.track << 56);
      sig ^= (droppedConsumer * 0x9E3779B185EBCA87ull);
      sig ^= (keptConsumer * 0xC2B2AE3D27D4EB4Full);
      return sig;
    };
#endif

    for (size_t i = 0; i < chosen.size(); ++i) {
      if (chosen[i].track == OVERLAY_TRACK_OBJECTIVE) {
        continue;
      }
      const OverlaySemanticKey sem = makeSemanticKey(chosen[i]);

      uint64_t stickyConsumer = 0;
      auto itSticky = s_semanticWinner.find(sem);
      if (itSticky != s_semanticWinner.end() &&
          (now - itSticky->second.second) <= kStickyMs) {
        stickyConsumer = itSticky->second.first;
      }

      auto itBest = bestBySemantic.find(sem);
      if (itBest == bestBySemantic.end()) {
        bestBySemantic.emplace(sem, i);
        continue;
      }

      const size_t curIdx = itBest->second;
      const bool takeNew =
          betterCandidate(chosen[i], chosen[curIdx], stickyConsumer);
      const size_t winner = takeNew ? i : curIdx;
      const size_t loser = takeNew ? curIdx : i;
      bestBySemantic[sem] = winner;
      keep[loser] = 0;

#if GHOSTSKOR_RUNTIME_DIAG
      const ClaimCandidate &kept = chosen[winner];
      const ClaimCandidate &dropped = chosen[loser];
      const uint64_t dlogKey =
          makeDupLogKey(sem, dropped.iid.consumerId, kept.iid.consumerId);
      auto itLog = s_dupLogTick.find(dlogKey);
      if (itLog == s_dupLogTick.end() || (now - itLog->second) > 1000) {
        s_dupLogTick[dlogKey] = now;
        char dbuf[512];
        sprintf_s(
            dbuf,
            "[HUD-DUP-BLOCK] consumer=0x%llX key=%s kept=src:%d,caller:+0x%X,slot:%d,consumer:0x%llX dropped=src:%d,caller:+0x%X,slot:%d,consumer:0x%llX",
            (unsigned long long)dropped.iid.consumerId, dropped.event.key.c_str(),
            (int)kept.event.source, kept.event.callerOffset,
            kept.iid.slotOrElemIndex, (unsigned long long)kept.iid.consumerId,
            (int)dropped.event.source, dropped.event.callerOffset,
            dropped.iid.slotOrElemIndex,
            (unsigned long long)dropped.iid.consumerId);
        LogToFile(dbuf);
      }
#endif
    }

    for (const auto &kv : bestBySemantic) {
      const size_t idx = kv.second;
      s_semanticWinner[kv.first] = {chosen[idx].iid.consumerId, now};
    }
    for (auto it = s_semanticWinner.begin(); it != s_semanticWinner.end();) {
      if ((now - it->second.second) > kPruneMs) {
        it = s_semanticWinner.erase(it);
      } else {
        ++it;
      }
    }
#if GHOSTSKOR_RUNTIME_DIAG
    for (auto it = s_dupLogTick.begin(); it != s_dupLogTick.end();) {
      if ((now - it->second) > kPruneMs) {
        it = s_dupLogTick.erase(it);
      } else {
        ++it;
      }
    }
#endif

    std::vector<ClaimCandidate> finalClaims;
    finalClaims.reserve(chosen.size());
    for (size_t i = 0; i < chosen.size(); ++i) {
      if (keep[i]) {
        finalClaims.push_back(std::move(chosen[i]));
      }
    }

    g_OverlayClaims.clear();
    g_OverlayClaims.reserve(finalClaims.size());
    g_OverlayClaimFrameId = frameId;

    for (const auto &c : finalClaims) {
      OverlayClaim out{};
      out.frameId = frameId;
      out.instanceId = c.iid;
      out.track = c.track;
      out.priority = c.score;
      out.reason = c.reason;
      out.key = c.event.key;
      g_OverlayClaims.push_back(std::move(out));

      if (c.event.callerOffset != 0) {
        g_OverlayCallerTrackHistory[c.event.callerOffset] = {c.track, now};
      }
      g_OverlayLaneTrackHistory[c.iid.slotOrElemIndex] = {c.track, now};
    }

    if (g_OverlayCallerTrackHistory.size() > 1024) {
      for (auto it = g_OverlayCallerTrackHistory.begin();
           it != g_OverlayCallerTrackHistory.end();) {
        if ((now - it->second.tick) > 15000) {
          it = g_OverlayCallerTrackHistory.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (g_OverlayLaneTrackHistory.size() > 256) {
      for (auto it = g_OverlayLaneTrackHistory.begin();
           it != g_OverlayLaneTrackHistory.end();) {
        if ((now - it->second.tick) > 10000) {
          it = g_OverlayLaneTrackHistory.erase(it);
        } else {
          ++it;
        }
      }
    }

    #if GHOSTSKOR_RUNTIME_DIAG
    static DWORD s_lastClaimLog = 0;
    if ((now - s_lastClaimLog) > 1200) {
      s_lastClaimLog = now;
      char cbuf[256];
      sprintf_s(cbuf, "[OVL-CLAIM] frame=%llu count=%d state=%d",
                (unsigned long long)frameId, (int)g_OverlayClaims.size(),
                (int)state);
      LogToFile(cbuf);
    }
#endif
  }
}

void TextHook_EndOverlayFrame(uint64_t frameId) {
  std::lock_guard<std::mutex> lock(g_OverlayBusMutex);
  if (g_OverlaySkipRenderFrame != 0 && frameId >= g_OverlaySkipRenderFrame) {
    g_OverlaySkipRenderFrame = 0;
  }
}

std::vector<OverlayClaim> TextHook_GetOverlayClaims(uint64_t frameId) {
  std::lock_guard<std::mutex> lock(g_OverlayBusMutex);
  if (g_OverlaySkipRenderFrame != 0 &&
      (frameId == g_OverlaySkipRenderFrame || frameId == 0)) {
    return {};
  }
  return g_OverlayClaims;
}

void TextHook_ResetOverlayRuntime(OverlayResetReason reason) {
  const DWORD now = GetTickCount();

  // Frontend transition (e.g., Save & Quit): drop lingering in-game subtitle
  // queue immediately so main menu never shows stale SEH subtitles.
  if (reason == OVERLAY_RESET_FRONTEND_ENTER) {
    TextHook_ClearSubtitleQueue();
  }

  TextHook_ObjSuppressClearAll();
  TextHook_ClearHintEntries();
  TextHook_ClearObjHudHintEntries();
  TextHook_ClearObjectiveEntries();
  TextHook_ClearHudNativeEntries();
  {
    std::lock_guard<std::mutex> lock(g_liveHudElemsMutex);
    g_liveHudElems.clear();
    g_liveHudElemsLastUpdate = now;
  }
  ObjectiveRuntime::ClearNativeObjectiveItems(now);
#if GHOSTSKOR_OBJECTIVE_REVERSE
  ObjectiveRuntime::SetRuntimeResetTick(now);
  ObjectiveRuntime::ClearTopLeftObjectiveRuntimeState();
#endif

#if GHOSTSKOR_OBJECTIVE_REVERSE
  {
    std::lock_guard<std::mutex> lock(g_objAuthoritativeCaptureMutex);
    g_objAuthoritativeByInstance.clear();
  }
  ObjRev_ClearPendingObjectiveAuthorityEvents();
  {
    std::lock_guard<std::mutex> lk(g_objCfgSlcRawMutex);
    g_objCfgSlcRawBySlc.clear();
  }
  g_objCfgSlcTlsCtx = {};
  g_objTypeChkTlsCtx = {};
  {
    std::lock_guard<std::mutex> lk(g_cfgDirectMapMutex);
    g_cfgDirectMap.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_slcToKeyMutex);
    g_slcToKey.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_activeObjectiveCfgMutex);
    g_activeObjectiveCfgTick.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_objSlcObsMutex);
    g_objSlcObservations.clear();
  }
  g_objSlcDirectBase.store(0);
  ObjRev_ResetBiasState();
#endif

  {
    std::lock_guard<std::mutex> lock(g_OverlayBusMutex);
    g_OverlayEventBus.clear();
    g_OverlayClaims.clear();
    g_OverlayClaimFrameId = 0;
    g_OverlayCallerTrackHistory.clear();
    g_OverlayLaneTrackHistory.clear();
  }
  {
    std::unique_lock<std::shared_mutex> lk(g_RuntimeEnglishMutex);
    g_RuntimeEnglishToKey.clear();
    g_RuntimeBindingTemplateToKey.clear();
    g_RuntimeKeyToEnglish.clear();
    g_RuntimeKeyLastSeenTick.clear();
  }

  // Reset credits guard: clear both soft guard and full guard on any reset.
  {
    extern std::atomic<bool> g_creditsGuardActive;
    extern std::atomic<bool> g_creditsSoftGuard;
    if (g_creditsGuardActive.load(std::memory_order_relaxed)) {
      TextHook_ResumeHooksAfterCredits();
      g_creditsGuardActive.store(false, std::memory_order_relaxed);
      LogToFile("[CREDITS-GUARD] OFF — overlay reset (full)");
    }
    if (g_creditsSoftGuard.load(std::memory_order_relaxed)) {
      TextHook_SoftResumeSLC();
      g_creditsSoftGuard.store(false, std::memory_order_relaxed);
      LogToFile("[CREDITS-GUARD] OFF — overlay reset (soft)");
    }
  }

  char buf[256];
  sprintf_s(buf, "[OVL-RESET] reason=%d", (int)reason);
  LogToFile(buf);
}

std::vector<ObjectiveOverlayEntry> TextHook_GetObjectiveEntries() {
  DWORD now = GetTickCount();
  return ObjectiveRuntime::GetOverlayEntriesSnapshot(now, OBJECTIVE_ENTRY_TIMEOUT,
                                                     false);
}
std::vector<ObjectiveKeyObservation>
TextHook_GetRecentObjectiveKeys(unsigned long maxAgeMs) {
  return ObjectiveRuntime::GetRecentObjectiveKeys(GetTickCount(), maxAgeMs);
}

// ??? Hint Pipeline Getter / Clear (Phase 3) ?????????????????????????????????
std::vector<HintOverlayEntry> TextHook_GetHintSnapshot() {
  std::lock_guard<std::mutex> lock(g_HintMutex);
  DWORD now = GetTickCount();
  // Remove expired entries
  g_HintEntries.erase(
      std::remove_if(g_HintEntries.begin(), g_HintEntries.end(),
                     [now](const HintOverlayEntry &e) {
                       return (now - e.lastSeen) > HINT_ENTRY_TIMEOUT;
                     }),
      g_HintEntries.end());
  return g_HintEntries;
}
void TextHook_ClearHintEntries() {
  std::lock_guard<std::mutex> lock(g_HintMutex);
  g_HintEntries.clear();
}

// ??? ObjHudHint Pipeline Getter / Clear (Phase 4) ???????????????????????????
std::vector<ObjHudHintEntry> TextHook_GetObjHudHintSnapshot() {
  std::lock_guard<std::mutex> lock(g_ObjHudHintMutex);
  DWORD now = GetTickCount();
  g_ObjHudHintEntries.erase(
      std::remove_if(g_ObjHudHintEntries.begin(), g_ObjHudHintEntries.end(),
                     [now](const ObjHudHintEntry &e) {
                       return (now - e.lastSeen) > OBJHUDHINT_ENTRY_TIMEOUT;
                     }),
      g_ObjHudHintEntries.end());
  return g_ObjHudHintEntries;
}
void TextHook_ClearObjHudHintEntries() {
  std::lock_guard<std::mutex> lock(g_ObjHudHintMutex);
  g_ObjHudHintEntries.clear();
}
void TextHook_ClearObjectiveEntries() {
  ObjectiveRuntime::ClearOverlayState();
  TextHook_ClearObjectiveStatusEvents();
}
void TextHook_ResetObjectiveRuntimeForMapRestart() {
  const unsigned long resetTick = GetTickCount();
  TextHook_TimeScriptReset();
  TextHook_ClearObjectiveEntries();
  TextHook_ClearHintEntries();
  TextHook_ClearObjHudHintEntries();
  TextHook_ClearHudNativeEntries();
  {
    std::lock_guard<std::mutex> lock(g_liveHudElemsMutex);
    g_liveHudElems.clear();
    g_liveHudElemsLastUpdate = resetTick;
  }
  ObjectiveRuntime::ClearNativeObjectiveItems(resetTick);
#if GHOSTSKOR_OBJECTIVE_REVERSE
  ObjectiveRuntime::SetRuntimeResetTick(resetTick);
  ObjectiveRuntime::ClearTopLeftObjectiveRuntimeState();
  {
    std::lock_guard<std::mutex> lock(g_objAuthoritativeCaptureMutex);
    g_objAuthoritativeByInstance.clear();
  }
  ObjRev_ClearPendingObjectiveAuthorityEvents();
  {
    std::lock_guard<std::mutex> lk(g_objCfgSlcRawMutex);
    g_objCfgSlcRawBySlc.clear();
  }
  g_objCfgSlcTlsCtx = {};
  g_objTypeChkTlsCtx = {};
  {
    std::lock_guard<std::mutex> lk(g_cfgDirectMapMutex);
    g_cfgDirectMap.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_slcToKeyMutex);
    g_slcToKey.clear();
  }
  {
    std::lock_guard<std::mutex> lk(g_activeObjectiveCfgMutex);
    g_activeObjectiveCfgTick.clear();
  }
  // Reset SLC observation state so new mission can rediscover base/bias.
  {
    std::lock_guard<std::mutex> lk(g_objSlcObsMutex);
    g_objSlcObservations.clear();
  }
  g_objSlcDirectBase.store(0);
  ObjRev_ResetBiasState();
#endif
}
unsigned long TextHook_GetObjectiveRuntimeResetTick() {
#if GHOSTSKOR_OBJECTIVE_REVERSE
  return ObjectiveRuntime::GetRuntimeResetTick();
#else
  return 0;
#endif
}
void TextHook_ResetObjectiveEntryAge(const std::string& key) {
  ObjectiveRuntime::ResetObjectiveEntryAge(key, GetTickCount());
}
void TextHook_ClearHudNativeEntries() {
  std::lock_guard<std::mutex> lock(g_HudNativeMutex);
  g_HudNativeMap.clear();
  {
    std::lock_guard<std::mutex> snapLock(g_NativeHudSnapshotMutex);
    g_NativeHudInstanceCache.clear();
    g_NativeHudLastSnapshotTick = 0;
  }
  {
    std::lock_guard<std::mutex> routeLock(g_HudRouteMutex);
    g_HudRouteByConsumer.clear();
    g_HudRouteQuarantineUntil.clear();
    g_HudRouteConflictHold.clear();
  }
  {
    std::lock_guard<std::mutex> tailLock(g_HudRuntimeTailMutex);
    g_HudRuntimeTailFragments.clear();
  }
  // Keep recent writer-observed hint slots across overlay clears.
  // TTL pruning in TextHook_GetRecentHudHintRuntimeSlots() prevents stale
  // carryover while still allowing snapshot collection to observe slots that
  // were written shortly before a reset edge.
}
void TextHook_ClearSubtitleQueue() {
  std::lock_guard<std::mutex> lock(g_InGameSubtitleMutex);
  const size_t clearedCount = g_InGameSubtitleQueue.size();
  const bool hadLegacyCurrent = !g_InGameSubtitle.korean.empty();
  std::string clearedKey;
  if (!g_InGameSubtitleQueue.empty()) {
    clearedKey = g_InGameSubtitleQueue.front().key;
  }
  g_InGameSubtitleQueue.clear();
  g_InGameSubtitle.korean = "";
  g_InGameSubtitle.timestamp = 0;
  {
    std::lock_guard<std::mutex> tlock(g_SubtitleRuntimeTimingMutex);
    g_SubtitleRuntimeTimingByKey.clear();
    g_SubtitleRuntimeTimingByNormText.clear();
  }
  if (clearedCount > 0 || hadLegacyCurrent) {
    char qbuf[256];
    sprintf_s(qbuf,
              "[SUBQ-CLEAR] count=%zu legacy=%d key=%s",
              clearedCount, hadLegacyCurrent ? 1 : 0,
              clearedKey.empty() ? "-" : clearedKey.c_str());
    LogToFile(qbuf);
  }
}

bool TextHook_HasRecentHudActivity(unsigned long maxAgeMs) {
  DWORD last = g_LastHudActivityTime.load();
  if (last == 0)
    return false;
  return (GetTickCount() - last) <= maxAgeMs;
}
bool TextHook_HasRecentSubtitleActivity(unsigned long maxAgeMs) {
  DWORD last = g_LastSubtitleActivityTime.load();
  if (last == 0)
    return false;
  return (GetTickCount() - last) <= maxAgeMs;
}
bool TextHook_HasRecentObjectiveLaneActivity(unsigned long maxAgeMs) {
  return ObjectiveRuntime::HasRecentObjectiveLaneActivity(GetTickCount(),
                                                          maxAgeMs);
}




