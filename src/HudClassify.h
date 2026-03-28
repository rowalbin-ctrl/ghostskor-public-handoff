#pragma once
// HudClassify.h — Single source of truth for localization key classification.
// Header-only. All pipeline routing decisions flow from ClassifyHudKey().
//
// Rules:
//   1st pass = key pattern (deterministic)
//   2nd pass = PromptLike heuristic (supplementary, never overrides 1st)
//   Mission namespace (CORNERED_, BLACK_ICE_ etc.) is NOT a classification criterion.
//   EXCLUDE prefixes are blocked here — individual pipelines must NOT re-check.

#include <string>
#include <cstring>

// ─── Enum ────────────────────────────────────────────────────────────────────

enum HudKeyType {
    HUD_KEY_UNKNOWN = 0,

    // === Overlay pipeline targets ===
    HUD_KEY_OBJECTIVE_LIST,      // *_OBJ_*, *_OBJECTIVE_*, GAME_OBJECTIVE*, etc.
    HUD_KEY_OBJECTIVE_UPDATE,    // OBJECTIVESUPDATED, OBJECTIVECOMPLETED, etc.
    HUD_KEY_OBJECTIVE_HEADER,    // CGAME_MISSIONOBJECTIVES
    HUD_KEY_GAME_SAVED,          // EXE_GAMESAVED
    HUD_KEY_NOW_SAVING,          // CGAME_NOW_SAVING
    HUD_KEY_HINT,                // HINT_*, PLATFORM_*, SCRIPT_HINT_*, *_HINT
    HUD_KEY_OBJ_HUD_HINT,       // Assigned by routing layer (HUD560 source, no pattern match)

    // === Separate systems (must NOT enter overlay pipelines) ===
    HUD_KEY_INTRO,               // *INTROSCREEN* + standalone intro center cards
    HUD_KEY_SUBTITLE,            // SUBTITLE_*
    HUD_KEY_MENU,                // MENU_*, LUA_MENU_*

    // === Absolute exclusion (never enters any pipeline) ===
    HUD_KEY_EXCLUDE,
};

// ─── Helpers (inline, internal) ──────────────────────────────────────────────

namespace HudClassifyDetail {

inline bool StartsWith(const std::string &s, const char *prefix) {
    return s.rfind(prefix, 0) == 0;
}

inline bool Contains(const std::string &s, const char *sub) {
    return s.find(sub) != std::string::npos;
}

inline bool EndsWith(const std::string &s, const char *suffix) {
    size_t suffLen = std::strlen(suffix);
    if (s.size() < suffLen) return false;
    return s.compare(s.size() - suffLen, suffLen, suffix) == 0;
}

// Keys ending in _OBJECTIVE or _OBJ (but not MENU_*)
inline bool HasObjectiveSuffix(const std::string &key) {
    if (StartsWith(key, "MENU_")) return false;
    return EndsWith(key, "_OBJECTIVE") || EndsWith(key, "_OBJ");
}

// Objective update/completed/failed variants (all known spellings)
inline bool IsObjectiveUpdatePattern(const std::string &key) {
    return Contains(key, "OBJECTIVESUPDATED")   ||
           Contains(key, "OBJECTIVEUPDATED")    ||
           Contains(key, "OBJECTIVECOMPLETED")  ||
           Contains(key, "OBJECTIVEFAILED")     ||
           Contains(key, "OBJECTIVES_UPDATED")  ||
           Contains(key, "OBJECTIVE_UPDATED")   ||
           Contains(key, "OBJECTIVE_COMPLETED") ||
           Contains(key, "OBJECTIVE_FAILED");
}

// Objective list patterns (mission-specific objectives)
inline bool IsObjectiveListPattern(const std::string &key) {
    // _OBJ_ or _OBJECTIVE_ substring (not MENU_*)
    if (Contains(key, "_OBJ_")) return true;
    if (Contains(key, "_OBJECTIVE_") && !StartsWith(key, "MENU_")) return true;

    // GAME_OBJECTIVE*, CGAME_OBJECTIVE*
    if (StartsWith(key, "GAME_OBJECTIVE")) return true;
    if (StartsWith(key, "CGAME_OBJECTIVE")) return true;

    // MISSIONOBJECTIVE / MISSION_OBJECTIVE
    if (Contains(key, "MISSIONOBJECTIVE")) return true;
    if (Contains(key, "MISSION_OBJECTIVE")) return true;

    // Suffix match: *_OBJECTIVE, *_OBJ
    if (HasObjectiveSuffix(key)) return true;

    return false;
}

// Hint patterns
inline bool IsHintPattern(const std::string &key) {
    // Prefix matches
    if (StartsWith(key, "HINT_")) return true;
    if (StartsWith(key, "SCRIPT_HINT_")) return true;
    if (StartsWith(key, "SCRIPT_PLATFORM_HINT_")) return true;
    if (StartsWith(key, "PLATFORM_")) return true;

    // Suffix/substring _HINT (but not MENU_* or PLATFORM_*)
    if (Contains(key, "_HINT") &&
        !StartsWith(key, "MENU_") &&
        !StartsWith(key, "PLATFORM_")) {
        return true;
    }
    // Mission prompt keys that don't use *_HINT naming.
    if ((Contains(key, "_PROMPT") || StartsWith(key, "PROMPT_")) &&
        !StartsWith(key, "MENU_") &&
        !StartsWith(key, "LUA_MENU_")) {
        return true;
    }

    return false;
}

} // namespace HudClassifyDetail

// ─── Main classification function ────────────────────────────────────────────

inline HudKeyType ClassifyHudKey(const std::string &key) {
    namespace D = HudClassifyDetail;

    if (key.empty()) return HUD_KEY_EXCLUDE;

    // ── EXCLUDE layer (absolute block, checked first) ────────────────────
    // These prefixes never enter any overlay pipeline.
    if (D::StartsWith(key, "WEAPON_"))      return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "ATTACHMENT_"))   return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "PERK_"))        return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "KILLSTREAK_"))  return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "RANK_"))        return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "CHALLENGE_"))   return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "ZM_"))          return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "MP_"))          return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "UI_"))          return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "CREDITS_"))     return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "PLAYERCARDS_")) return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "CUSTOMIZE_"))   return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "PERKS_"))       return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "LOADOUT_"))     return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "SQUAD_"))       return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "PRESTIGE_"))    return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "CLASS_"))       return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "CAMO_"))        return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "RETICLE_"))     return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "EXTINCTION_"))  return HUD_KEY_EXCLUDE;
    if (D::StartsWith(key, "SPECIAL_"))     return HUD_KEY_EXCLUDE;

    // ── Separate systems (must not enter overlay pipelines) ──────────────
    if (D::Contains(key, "INTROSCREEN"))    return HUD_KEY_INTRO;
    // Some intro center cards do not use the *_INTROSCREEN_* naming even
    // though they are rendered by the intro pipeline.
    if (key == "CARRIER_3DAYS")             return HUD_KEY_INTRO;
    // Non-INTROSCREEN intro text variants rendered via IntroRenderFn:
    //   SATFARM_B_INTRO_LINE_1, DEER_HUNT_JEEP_INTROLINE_*, LAS_VEGAS_INTRO_TIME*
    if (D::Contains(key, "_INTRO_LINE_"))   return HUD_KEY_INTRO;
    if (D::Contains(key, "_INTROLINE"))     return HUD_KEY_INTRO;
    if (D::Contains(key, "_INTRO_TIME"))    return HUD_KEY_INTRO;
    if (D::StartsWith(key, "SUBTITLE_"))    return HUD_KEY_SUBTITLE;
    if (D::StartsWith(key, "MENU_"))        return HUD_KEY_MENU;
    if (D::StartsWith(key, "LUA_MENU_"))    return HUD_KEY_MENU;

    // ── 1st pass: deterministic key-pattern classification ───────────────

    // Exact-match specials
    if (key == "EXE_GAMESAVED")          return HUD_KEY_GAME_SAVED;
    if (key == "CGAME_NOW_SAVING")       return HUD_KEY_NOW_SAVING;
    if (key == "CGAME_MISSIONOBJECTIVES") return HUD_KEY_OBJECTIVE_HEADER;

    // Objective update/completed/failed (before list, because list patterns
    // could partially overlap with update patterns in rare key names)
    if (D::IsObjectiveUpdatePattern(key))   return HUD_KEY_OBJECTIVE_UPDATE;

    // Objective list (mission-specific objectives)
    if (D::IsObjectiveListPattern(key))     return HUD_KEY_OBJECTIVE_LIST;

    // Hint patterns
    if (D::IsHintPattern(key))              return HUD_KEY_HINT;

    // ── Center-screen overlay patterns (mission-agnostic) ─────────────
    // These patterns appear center-screen regardless of mission prefix.
    // Exclude-layer and system-layer filters already ran above.
    if (D::Contains(key, "BINOCULAR") || D::Contains(key, "SCANNING") ||
        D::Contains(key, "IDENTIFIED") || D::Contains(key, "DEGREE_SYMBOL")) {
        return HUD_KEY_HINT;
    }
    if (D::Contains(key, "_FAIL") || D::Contains(key, "FAIL_") ||
        D::Contains(key, "_DEATH") || D::Contains(key, "_DROWN") ||
        D::Contains(key, "_SUICIDE") || D::Contains(key, "_QUOTE_") ||
        D::Contains(key, "_KILLED") || D::Contains(key, "GOT_AWAY")) {
        return HUD_KEY_HINT;
    }

    // ── General fallback ────────────────────────────────────────────────
    // Any key surviving all filters is gameplay text the engine actively
    // renders. Default to ObjHudHint (cursorHint bottom).
    return HUD_KEY_OBJ_HUD_HINT;
}

// ─── Convenience predicates ──────────────────────────────────────────────────

// Is this key part of the Objective pipeline?
inline bool IsObjectivePipelineKey(const std::string &key) {
    HudKeyType t = ClassifyHudKey(key);
    return t == HUD_KEY_OBJECTIVE_LIST   ||
           t == HUD_KEY_OBJECTIVE_UPDATE ||
           t == HUD_KEY_OBJECTIVE_HEADER ||
           t == HUD_KEY_GAME_SAVED       ||
           t == HUD_KEY_NOW_SAVING;
}

// Is this key part of the HUD Hint pipeline?
inline bool IsHintPipelineKey(const std::string &key) {
    return ClassifyHudKey(key) == HUD_KEY_HINT;
}

// Should this key be excluded from all pipelines?
inline bool IsExcludedKey(const std::string &key) {
    return ClassifyHudKey(key) == HUD_KEY_EXCLUDE;
}

// Is this an objective update/completed token? (for cycle detection)
inline bool IsObjectiveUpdateToken(const std::string &key) {
    HudKeyType t = ClassifyHudKey(key);
    return t == HUD_KEY_OBJECTIVE_UPDATE;
}

// Is this a gameplay-renderable objective channel?
inline bool IsGameplayObjectiveKey(const std::string &key) {
    HudKeyType t = ClassifyHudKey(key);
    return t == HUD_KEY_OBJECTIVE_LIST   ||
           t == HUD_KEY_OBJECTIVE_UPDATE ||
           t == HUD_KEY_GAME_SAVED       ||
           t == HUD_KEY_NOW_SAVING;
}
