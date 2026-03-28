# TimeScript Countdown Timer System — 기술 문서

## 목적

Call of Duty: Ghosts (iw6sp64_ship.exe) Clockwork 미션의 카운트다운 타이머 HUD를
한글화하는 시스템. 소스 코드 접근 없이 DLL 프록시 + D3D11 오버레이로 구현.

```
원본:  "Powerdown in 1:07.7"    →  한글: "전력 차단까지 1:07.7"
원본:  "Time to Exfil 0:45.3"   →  한글: "탈출 시간 0:45.3"
```

---

## 1. 엔진 구조: HudElem

IW6 엔진은 HudElem 배열로 HUD 요소를 관리한다.
카운트다운 타이머는 `HE_TYPE_TIMER_DOWN` (type=7) 타입의 HudElem이다.

### HudElem 배열 위치

```
Base: moduleBase + IW6Offsets::HudElem_Array_SP
Stride: IW6Offsets::HudElem_Array_Stride_SP
Max: 1024 elements
```

### HudElem 구조체 (확인된 오프셋)

| Offset | Type     | Field           | 용도                                     |
|--------|----------|-----------------|------------------------------------------|
| +0x00  | uint32   | type            | 요소 타입 (0=FREE, 1=TEXT, 7=TIMER_DOWN) |
| +0x04  | float    | x               | 가상 좌표 X                              |
| +0x08  | float    | y               | 가상 좌표 Y                              |
| +0x14  | float    | fontScale       | 폰트 크기 배율                           |
| +0x1C  | float    | timerValue      | 렌더 구조체의 float 값 (PreDrawHook용)   |
| +0x28  | uint32   | alignOrg        | 요소 원점 정렬                           |
| +0x2C  | uint32   | alignScreen     | 화면 앵커 (상위4bit=H, 하위4bit=V)       |
| +0x30  | uint32   | color           | ABGR 색상 (상위 byte = alpha)            |
| +0x40  | uint32   | label           | SLC 인덱스 (라벨 텍스트 키)              |
| +0x50  | uint64   | textPtr         | 텍스트 문자열 포인터                     |
| +0x78  | int32    | time            | 절대 종료 시각 (서버 ms)                 |
| +0x7C  | int32    | duration        | 지속 시간 (ms)                           |
| +0x80  | float    | value           | 일반 값 (TIMER에서는 미사용)             |
| +0x84  | uint32   | text            | SLC 인덱스 (텍스트)                      |
| +0xA4  | int32    | flags           | HUD 요소 플래그                          |

### alignScreen 인코딩

```
alignScreen = (horzScreenAlign << 4) | vertScreenAlign
예: 0x31 → horiz=3(RIGHT), vert=1(TOP)

horiz: 0=SUBLEFT, 1=LEFT, 2=CENTER, 3=RIGHT, 7=CENTER_SA
vert:  0=SUBTOP, 1=TOP, 2=CENTER, 3=BOTTOM, 7=CENTER_SA
```

### Clockwork 타이머의 관측된 HudElem 값

```
type = 7 (TIMER_DOWN)
x = -250.0, y = 100.0
fontScale = 1.60
alignScreen = 0x31 (RIGHT, TOP)
color = 0xFFCCFFCC (연녹색, alpha=0xFF)
label SLC = 0x23 (CLOCKWORK_POWERDOWN) 또는 0x24 (CLOCKWORK_EXFIL)
```

---

## 2. 타이머 값 계산

엔진 타이머는 **절대 종료 시각** 방식:

```
remaining_seconds = (elem.time - serverTime) / 1000.0f
```

- `elem.time` (+0x78): 카운트다운이 끝나는 서버 시각 (ms, 절대값)
- `serverTime`: 현재 게임 시각 (ms)
- `elem.value` (+0x80)는 TIMER 타입에서 **사용되지 않음** (항상 0)

### SP 게임 시각 발견

SP 모드에서 `serverTime`은 고정 오프셋에 없다. 동적 프로브로 발견:

1. **인라인 프로브**: 알려진 후보 오프셋 목록에서 즉시 시도
   - 확인된 SP 오프셋: `+0x1473528` (wide scan에서 발견)
2. **Wide scan**: 2초 간격 2-pass 스캔으로 게임 시각 주소 확정
   - `g_countdownTimeField - candidate`가 1~120초 범위인 주소를 찾음

---

## 3. 시스템 아키텍처

### 코드 파일 구조

| 파일 | 역할 |
|------|------|
| `TextHook.TimeScript.inl` | 핵심 로직: HudElem 스캔, 스냅샷 생성, 엔진 suppression |
| `D3D11Hook.Process.TimeScript.inl` | D3D11 오버레이 렌더링 (한글 라벨 + 타이머 숫자) |
| `TextHook.IntroRendering.inl` | IntroActualRender 훅에서 CLOCKWORK 감지 및 skipOriginal |
| `texthook.cpp` | TimeScriptPersistentSuppress 구조체 정의, HudElem 배열 스캔 |

### 데이터 흐름

```
[HudElem 배열 스캐너] ─── StoreSnapshotFromHudElemTimer() ──┐
                                                              │
[IntroActualRender 훅] ── TextHook_TimeScriptOnIAR...() ─────┤
                                                              │
[R_AddCmdDrawText 훅] ── TimeScript_PreDrawHook() ───────────┤
                                                              ▼
                                              TimeScriptRenderSnapshot
                                              (key, renderText, x, y,
                                               scale, fontHeight,
                                               timerSeconds, color)
                                                              │
                                                              ▼
                                    [D3D11 Present() 훅]
                                    D3D11Hook.Process.TimeScript.inl
                                              │
                                              ▼
                                    KoreanRenderer::QueueText()
                                    "전력 차단까지 1:07.7"
```

### 스냅샷 소유권 시스템

세 가지 경로가 스냅샷을 생성할 수 있으며, 우선순위로 관리:

1. **directOwner** (HudElem 스캐너): 가장 정확. `StoreSnapshotFromHudElemTimer()`
2. **IAR-direct**: HudElem 스캐너 스냅샷 없을 때 하드코딩 레이아웃으로 즉시 생성
3. **IAR-fallback**: `TryBuildIarFallbackSnapshot()`으로 생성

TTL: `kTimeScriptSnapshotTtlMs = 500ms`. 갱신 없으면 스냅샷 만료.

---

## 4. 엔진 렌더링 Suppression

엔진의 원본 타이머 렌더링을 숨기기 위해 HudElem 필드를 직접 수정한다.

### Suppression 전략 (4중 방어)

| 필드 | 오프셋 | 설정값 | 효과 |
|------|--------|--------|------|
| **type** | +0x00 | 0 (FREE) | 엔진이 요소를 완전히 무시 (가장 강력) |
| fontScale | +0x14 | 0.0f | 렌더링 크기 0 |
| color alpha | +0x30 | 0x00xxxxxx | 투명 |
| time | +0x78 | 0 | 타이머 숫자 "0:00.0" 고정 |

**핵심**: `type=0` (HE_TYPE_FREE)가 가장 효과적. 엔진의 `CG_DrawHudElem`이
첫 번째 타입 체크에서 즉시 skip하므로 라벨과 타이머 숫자 모두 렌더링되지 않음.

### Suppression 적용 지점 (6곳)

| 지점 | 실행 시점 | 코드 위치 |
|------|-----------|-----------|
| StoreSnapshotFromHudElemTimer | Present() 훅 (매 프레임) | TextHook.TimeScript.inl |
| TimeScript_PreDrawHook | R_AddCmdDrawText 호출 시 | TextHook.TimeScript.inl |
| IAR TTL refresh | IntroActualRender 호출 시 | TextHook.TimeScript.inl |
| IAR heartbeat | IAR stale 스냅샷 갱신 시 | TextHook.TimeScript.inl |
| IAR-direct | 최초 감지 시 즉시 | TextHook.TimeScript.inl |
| ProcessIntroCountdownSnapshot | IntroRendering SLC 매칭 시 | TextHook.IntroRendering.inl |

### TimeScriptPersistentSuppress 구조체

```cpp
struct TimeScriptPersistentSuppress {
  uintptr_t rbx = 0;                  // 렌더 구조체 주소
  uintptr_t elemPtr = 0;              // HudElem 배열 요소 주소
  uint32_t originalColor = 0;         // 원본 색상 (복원용)
  uint32_t originalFontScaleBits = 0; // 원본 fontScale IEEE 754 비트
  uint32_t cachedTimeField = 0;       // 캐시된 time 필드 (절대 종료 시각)
  uint32_t originalType = 0;          // 원본 type 필드 (예: 7=TIMER_DOWN)
  DWORD lastSuppressTick = 0;         // 마지막 suppression 시각
  bool active = false;
};
```

### 복원 조건

- 500ms 동안 suppression 갱신 없음 → `RestorePersistentSuppressIfStale()` 호출
- 모든 필드(type, fontScale, time, color) 원래 값으로 복원
- 레벨 전환 / 리셋 시 `TextHook_TimeScriptReset()` 호출

### 스캐너 bypass

type=0으로 제로화하면 HudElem 스캐너가 요소를 무시하므로,
`isSuppressedTimer` 판정으로 suppressed 요소도 스캔하도록 우회:

```cpp
const bool isSuppressedTimer =
    (t == 0 && g_TimeScriptPersistSuppress.active &&
     g_TimeScriptPersistSuppress.elemPtr == elem &&
     g_TimeScriptPersistSuppress.originalType != 0);
const bool isTimerElem = IsHudElemTimerType(t) || isSuppressedTimer;
```

---

## 5. D3D11 오버레이 렌더링

### 합성 텍스트 생성

한글 라벨과 타이머 숫자를 하나의 문자열로 합성:

```cpp
// 타이머 포맷: 67.4초 → "1:07.4"
sprintf_s(timerBuf, "%d:%02d.%d", mins, secsI, tenths);
finalRenderText = "전력 차단까지 " + "1:07.4";
```

### 초기 0:00.0 방지

게임 시각 미발견 시 timerSeconds=0. 이때 타이머 숫자 미표시:

```cpp
if (timeScriptSnapshot.timerSeconds > 0.05f) {
  // 타이머 숫자 포맷 및 표시
}
// timerSeconds <= 0.05 이면 한글 라벨만 표시 (숫자 없음)
```

### 즉시 사라짐

타이머가 0에 도달하면 한글 오버레이도 즉시 중단:

```cpp
static bool s_timerEverActive = false;
if (timerSeconds > 0.05f) s_timerEverActive = true;
if (s_timerEverActive && timerSeconds <= 0.05f) {
  // 렌더링 완전 중단 → 라벨+숫자 동시 사라짐
}
```

### 레이아웃 계산

```cpp
// Clockwork 타이머 감지 조건
clockworkPowerdownLane =
    horzAlign==2 && vertAlign==0 &&
    x in [-340, -140] && y in [60, 140] &&
    fontScale in [1.20, 2.10]

// 적용 값
outFontHeight = 72.0f
outScale = snap.fontScale * 1.10f  // fontScale=1.60 → scale=1.76
outY += 36.0f * (screenHeight / 1080.0f)  // 베이스라인 보정
outX -= 20.0f * (screenWidth / 1920.0f)   // 수평 보정
```

### 스타일 (Glow)

```cpp
glowR = 0.10f, glowG = 0.22f, glowB = 0.10f  // 진한 초록 배경
glowA = 0.85f
glowSpread = 7.0f
glowAlphaMax = 0.35f
```

---

## 6. 번역 데이터

`localize.json` / `localize_kr.json`:

| 키 | 영어 | 한국어 |
|----|------|--------|
| CLOCKWORK_POWERDOWN | "Powerdown in " | "전력 차단까지 " |
| CLOCKWORK_EXFIL | "Time to Exfil " | "탈출 시간 " |

SLC 인덱스:
- CLOCKWORK_POWERDOWN = 0x23
- CLOCKWORK_EXFIL = 0x24

---

## 7. 알려진 제한사항

1. **SP 게임 시각 주소가 고정 아님**: 인라인 프로브 + wide scan으로 매번 동적 발견 필요.
   확인된 오프셋 `+0x1473528`이 후보 목록에 포함되어 있어 보통 즉시 발견됨.

2. **type 제로화의 잠재 위험**: 게임 스크립트가 HudElem type을 확인하여
   카운트다운 완료 이벤트를 처리할 수 있음. 현재까지 부작용 미관찰.

3. **Clockwork 미션 전용**: 현재 CLOCKWORK_POWERDOWN과 CLOCKWORK_EXFIL만 처리.
   다른 미션의 타이머가 있을 경우 `IsTimeScriptManagedCountdownKeyName()` 확장 필요.

---

## 8. 주요 함수 참조

| 함수 | 위치 | 역할 |
|------|------|------|
| `TimeScript_StoreSnapshotFromHudElemTimer` | TimeScript.inl | HudElem → 렌더 스냅샷 변환 |
| `TryComputeTimeScriptLayout` | TimeScript.inl | 가상좌표 → 화면좌표 변환 |
| `TimeScript_PreDrawHook` | TimeScript.inl | R_AddCmdDrawText 훅 (suppression) |
| `TextHook_TimeScriptOnIntroActualRenderCandidate` | TimeScript.inl | IAR 훅 진입점 |
| `TimeScript_RestorePersistentSuppressIfStale` | TimeScript.inl | Suppression 만료 복원 |
| `TextHook_TimeScriptReset` | TimeScript.inl | 전체 리셋 |
| `TextHook_GetTimeScriptRenderSnapshot` | TimeScript.inl | D3D11에서 스냅샷 읽기 |
| `TextHook_ReadSPGameTime` | texthook.cpp | SP 게임 시각 반환 |
| `Detour_IntroActualRender` | IntroRendering.inl | CLOCKWORK skipOriginal |

---

## 9. 엔진 주소 참조 (iw6sp64_ship.exe 기준)

| 용도 | RVA |
|------|-----|
| IntroActualRender | 0x416AE0 |
| HudElem Array (SP) | IW6Offsets::HudElem_Array_SP |
| 확인된 SP serverTime | moduleBase + 0x1473528 |
