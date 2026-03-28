# SSOT: TimeScript English Timer Suppression via CG_DrawHudElem

**Date**: 2026-03-18
**Status**: Active — regression-prone subsystem, handle with extreme care

## Why This Document Exists

타이머 영어 억제 시스템은 **상호 의존적인 휴리스틱**으로 구성되어 있어, 하나를 수정하면 다른 곳이 깨지는 무한회귀가 반복되었다. 이 문서는 각 가드의 역할과 상호작용을 명확히 기록하여 향후 수정 시 회귀를 방지한다.

## Architecture Overview

```
[HudElem Scanner] ──→ TimeScript_StoreSnapshotFromHudElemTimer()
[IAR-DIRECT]      ──→ g_TimeScriptPersistSuppress 등록
                         ↓
              CG_DrawHudElem (매 프레임, 모든 HudElem)
                         ↓
              elemPtr == registered? ──YES──→ slot-reuse 체크 → suppress (x=9999)
                         │NO
              companion timeField match? ──YES──→ suppress
                         │NO
              auto-registration (label 기반)
```

## CG_DrawHudElem Suppress Logic (TextHook.HookInstallAndDetours.inl)

### Primary Suppress (line 147-180)

```
elemPtr == g_TimeScriptPersistSuppress.elemPtr
  → slot-reuse 체크:
    1. labelKnown: originalLabel != 0 (label이 기록되었는가?)
    2. labelChanged: labelKnown && curLabel(+0x40) != originalLabel
    3. textReused: !labelKnown && curText(+0xA0) != 0 && !IsKnownTimeScriptTextCfg
  → if (labelChanged || textReused) → deactivate
  → else → write x=9999

  NOTE: label이 기록되어 있으면(labelKnown=true) +0xA0 체크를 완전히 건너뜀.
  +0xA0은 text configstring이 아니라 매 프레임 증가하는 카운터(0x13→0x14→...)이므로
  whitelist 방식으로는 절대 안정적으로 작동하지 않음.
```

### Companion Suppress (line 181-186)

```
cachedTimeField > 30000 && elemPtr(+0x78) == cachedTimeField
  → write x=9999
```

### Auto-Registration (line 207+)

```
!g_TimeScriptPersistSuppress.active && label match
  → SATFARM: label == 0x3B || 0x3C (no type guard)
  → CLOCKWORK: label == 0x23 && IsHudElemTimerType(type)
  → register elemPtr, originalLabel, active=true
```

## Slot-Reuse Detection: Two Guards, Both Required

### Guard 1: Label Check (`originalLabel` vs `curLabel`)

| Scenario | originalLabel | curLabel | labelChanged | Result |
|----------|--------------|----------|-------------|--------|
| Timer active | 0x23 | 0x23 | false | suppress continues ✓ |
| Gauge reuses slot | 0x23 | 0x00 | **true** | deactivate ✓ |
| Objective reuses slot | 0x23 | 0x09 | **true** | deactivate ✓ |
| Never recorded | 0x00 | anything | false | falls through to Guard 2 |

### Guard 2: Text Whitelist (`IsKnownTimeScriptTextCfg`)

| +0xA0 value | In whitelist? | textReused | Result |
|-------------|--------------|-----------|--------|
| 0x00 | yes (no text) | false | suppress continues ✓ |
| 0x23 | yes | false | suppress continues ✓ |
| 0x24 | yes | false | suppress continues ✓ |
| 0x3B, 0x3C | yes | false | suppress continues ✓ |
| 0xFF (random) | no | **true** | deactivate ✓ |

### CRITICAL: Why 0x23/0x24 MUST Stay in the Whitelist

타이머 HudElem 자체의 +0xA0 필드가 0x23 또는 0x24를 포함할 수 있다 (+0xA0은 text SLC가 아닌 다른 필드). 이 값을 whitelist에서 제거하면 **타이머 자체를 "slot reuse"로 오감지**하여 매 프레임 suppress가 해제된다.

게이지/목표 등 비타이머 요소가 같은 슬롯을 재사용할 때의 오탐은 **Guard 1 (label check)**이 처리한다. Guard 2는 타이머 자체의 +0xA0 값이 whitelist에 있음을 보장하는 역할만 한다.

**절대로 Guard 1 없이 Guard 2만 수정하거나, Guard 2의 whitelist에서 값을 제거하지 말 것.**

## Registration Points (originalLabel 저장 위치)

| Path | File | Line (approx) | When |
|------|------|---------------|------|
| HudElem Scanner | TextHook.TimeScript.inl:1360 | 매 프레임 스캐너가 타이머 감지 시 |
| IAR-DIRECT | TextHook.TimeScript.inl:603 | SEH 훅에서 카운트다운 키 감지 시 |
| Auto-Registration | TextHook.HookInstallAndDetours.inl:236 | CG_DrawHudElem에서 label 매칭 시 |

모든 경로에서 `originalLabel`을 반드시 저장해야 한다. 누락 시 Guard 1이 작동하지 않는다.

## Known Timer Keys and Their Configstrings

| Key | Mission | Label cfg (+0x40) | Notes |
|-----|---------|-------------------|-------|
| CLOCKWORK_POWERDOWN | Clockwork | 0x23 | 첫 번째 타이머 (70초) |
| CLOCKWORK_EXFIL | Clockwork | 0x23 | 두 번째 타이머 (탈출 2분). cfg=0x23 공유 (슬롯 재사용) |
| SATFARM_TIME_IMACT | Loki | 0x3C → 0x3B | 맵 전환 후 label 변경 |

## HudElem Slot Reuse Timeline (CLOCKWORK, ptr=0x14147FB74)

```
SKYWAY:     type=1 label=0x0 text=0x721 (SKYWAY_HINT_*)
CLOCKWORK:  type=5 label=0x23 text=0x0  (POWERDOWN timer) ← suppress target
            type=1 label=0x9  text=...  (NIGHTVISION hint)
            type=1 label=0x23 text=...  (DISABLE_THE_SECURITY objective)
            type=? label=?    text=0x24 (gauge bar, key="")
            type=1 label=0xA  text=...  (TEARGAS prompt)
            type=1 label=0xC  text=...  (MINE prompt)
EXFIL:      type=? label=0x23 text=?    (EXFIL timer) ← needs new suppress
```

## Regression History

| Date | Change | Broke | Root Cause |
|------|--------|-------|------------|
| 03-18 | Removed originalType check | SATFARM OK, CLOCKWORK gauge suppressed | Guard 2 alone insufficient for slot reuse |
| 03-18 | Removed 0x23/0x24 from whitelist | CLOCKWORK_POWERDOWN suppress broken | Timer's own +0xA0 field contains 0x23 |
| 03-18 | Added label check + restored whitelist | Pending test | Both guards now active |
| 03-18 | Added +0x84 text SLC guard | Gauge still suppressed | Scanner re-registration loop |
| 03-18 | Added deactivation cooldown (5s) | Still suppressed on checkpoint load | Auto-reg captures gauge values |
| 03-18 | **REAL FIX**: obj suppress stale entry | **CLOCKWORK gauge FIXED** | CG_DrawHudElem refreshed tick → entry never expired |
| 03-18 | SATFARM 0x3B scan: removed type guard | 1st transition FIXED | Type changes to 1/4 during cutscene |
| 03-18 | Scanner: label 0x3B/0x3C bypass type check | 1st transition FIXED (scanner) | Scanner skipped non-timer-type elements |
| 03-18 | Stale-timer invalidation (2s freeze) | All transitions FIXED | IAR-TTL blocked IAR-DIRECT rescan |

## CLOCKWORK File/Gauge Bug (FIXED 03-18)

게이지가 억제된 원인은 **TimeScript가 아니라 Objective suppress** (`g_objSuppressedElems`).
`0x14147FCC4`가 `CLOCKWORK_OBJ_DEFEND`로 등록됨 → File 구간에서 슬롯 재사용 →
CG_DrawHudElem이 `lastRefreshTick`을 매 프레임 갱신 → stale 엔트리 영구 유지.

**Fix**: CG_DrawHudElem에서 tick 갱신 제거. 렌더러만 갱신. 2초간 갱신 없으면 엔트리 자동 삭제.

## SATFARM Map Transition Fixes (FIXED 03-18)

### 문제 1: 1차 전환 후 타이머/억제 실패
- IAR-DIRECT 0x3B 보조 스캔이 `eType == 5` 요구 → 전환 후 type=1/4라 실패
- HudElem 스캐너가 `IsHudElemTimerType(t)` 체크로 type≠5인 요소 건너뜀
- **Fix**: 0x3B 스캔 type 가드 제거 + 스캐너에서 label 0x3B/0x3C은 type 무관 처리

### 문제 2: 2차/3차 전환 후 타이머 값 멈춤 (81.7s 고정)
- IAR-TTL이 `hasFreshSnapshot=true`로 `return false` → IAR-DIRECT 재스캔 차단
- 캐시된 elemPtr의 +0x78이 stale → 타이머 값 변하지 않음
- **Fix**: 타이머 값 2초간 미변화 시 스냅샷 무효화 + `goto iar_direct_rescan`

## Rules for Future Modifications

1. **Guard 1 (label)과 Guard 2 (text whitelist)는 독립적으로 동작해야 한다**. 하나만으로는 모든 케이스를 커버할 수 없다.
2. **IsKnownTimeScriptTextCfg의 값을 제거하기 전에** 해당 값이 타이머 HudElem의 +0xA0에 나타나지 않음을 로그로 확인할 것.
3. **auto-registration 조건을 완화하기 전에** 해당 label이 비타이머 요소에도 사용되는지 OBJ-REVERSE 로그로 확인할 것.
4. **companion suppress (timeField 매칭)는 false positive 위험이 있다**. cachedTimeField와 우연히 일치하는 비타이머 요소가 억제될 수 있다.
5. **configstring 인덱스는 미션 내에서 재사용된다**. 0x23은 CLOCKWORK_POWERDOWN일 수도, CLOCKWORK_DISABLE_THE_SECURITY일 수도, CLOCKWORK_EXFIL일 수도 있다.
6. **SATFARM label 0x3B/0x3C은 type 체크 없이 타이머로 취급**. 컷씬 중 type 5→1/4 전환 발생.
7. **obj suppress 엔트리는 렌더러만 tick 갱신**. CG_DrawHudElem에서 갱신하면 stale 엔트리가 영구 유지됨.
8. **IAR-TTL이 timer 값을 freeze하면 스냅샷을 무효화**해서 IAR-DIRECT 재스캔을 허용해야 함.
