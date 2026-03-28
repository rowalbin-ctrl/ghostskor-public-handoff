# SSOT: CG_DrawHudElem Hook (2026-03-15)

작성일: 2026-03-15
범위: `CG_DrawHudElem` 런타임 함수 디스커버리 및 훅을 통한 영문 HUD 억제
파일: `TextHook.HookInstallAndDetours.inl` (훅 구현), `texthook.cpp` (suppress 맵/API)

---

## 0) 배경 — 왜 CG_DrawHudElem 훅이 필요한가

오브젝티브/타임스크립트 HudElem의 영문 텍스트를 한글 오버레이로 대체할 때,
엔진이 매 프레임 `CG_UpdateHudElem`에서 HudElem 필드(x, color 등)를 덮어쓴다.

### 기존 접근 (Present 시점 x=9999 쓰기)의 문제

```
Frame N:
  CG_UpdateHudElem → x를 원본값으로 복원
  CG_DrawHudElem   → x 읽음 → 원본값 → 영문 렌더됨 (깜빡!)
  Present hook     → x=9999 쓰기 (이미 늦음)
```

Present 시점은 CG_DrawHudElem이 이미 x를 읽은 후이므로 **레이스 컨디션** 발생.
영문이 1~수 프레임씩 깜빡이며 보이는 현상(잔상 flicker)의 원인.

### 해결 — CG_DrawHudElem 직전 필드 강제

```
Frame N:
  CG_UpdateHudElem → x를 원본값으로 복원
  Detour_CG_DrawHudElem:
    → 억제 대상이면 x=9999 (또는 alpha=0) 쓰기
    → 원본 CG_DrawHudElem 호출 → 수정된 값 읽음 → 화면 밖 또는 투명
  Present hook     → 한글 오버레이 렌더
```

**레이스 윈도우 = 0**. 원본 함수가 필드를 읽기 직전에 강제하므로 깜빡임 없음.

---

## 1) 런타임 함수 디스커버리

게임 바이너리(`iw6sp64_ship.exe`)는 디스크에서 패킹/암호화되어 있어
정적 분석으로 주소를 얻을 수 없다. 런타임에 복호화된 메모리에서 찾아야 한다.

### 알려진 사실

- `SEH_StringEd_GetString`은 `CG_DrawHudElem` 내부에서 호출됨
- 호출 지점의 리턴 주소 RVA = **0x1F087D** (이 주소에서 RDI = hudelem_s 포인터)
- CG_DrawHudElem 함수 시작 RVA = **0x1F06B0** (호출 지점으로부터 461바이트 전)

### 탐색 과정 (`DelayedHookThread`)

```
1. SEH_StringEd_GetString 훅 설치 대기 (seh_hooked == true)
2. moduleBase + 0x1F087D 주소가 실행 가능 메모리인지 VirtualQuery로 확인
   (IsProbablyExecutableCode 사용 불가 — 0x1F087D는 함수 중간이라 프롤로그 패턴 불일치)
3. ScanBackForFunctionStart(): 0x1F087D부터 역방향으로 CC(int3) 패딩 탐색
   - MSVC는 함수 사이에 CC 패딩 삽입
   - CC 바이트 직후 non-CC 바이트 = 다음 함수의 첫 바이트
4. 발견된 주소에 MinHook으로 CreateHook 설치
```

### 프롤로그 시그니처

```
48 89 5C 24 08    MOV [RSP+8], RBX
48 89 6C 24 18    MOV [RSP+18], RBP
48 89 74 24 20    MOV [RSP+20], RSI
57                PUSH RDI
```

---

## 2) 훅 구현 — `Detour_CG_DrawHudElem`

파일: `TextHook.HookInstallAndDetours.inl` (line ~76)

### 시그니처

```cpp
void __fastcall Detour_CG_DrawHudElem(uintptr_t rcx, uintptr_t rdx,
                                      uintptr_t r8, uintptr_t r9);
```

x64 __fastcall: RCX, RDX 중 하나가 `hudelem_s*` 포인터.
두 파라미터 모두 검사하여 포인터 범위 확인 후 처리.

### 동작 흐름

```
1. Fast-path 체크
   - g_hasObjSuppressed (atomic<bool>) — 오브젝티브 억제 활성?
   - g_TimeScriptPersistSuppress.active — 타임스크립트 억제 활성?
   - 둘 다 false면 → 바로 원본 호출 (오버헤드 ≈ 0)

2. RCX, RDX 순회 (유효 포인터 범위 검사)

3-A. 오브젝티브 억제 (x = +9999)
   - g_objSuppressedElems 맵에서 ptr 조회 (mutex lock)
   - 일치 → ptr+0x04에 9999.0f 기록 (IEEE 754 비트 복사)
   - lastRefreshTick 갱신

3-B. 타임스크립트 억제 (alpha = 0)
   - ptr == g_TimeScriptPersistSuppress.elemPtr 비교
   - 일치 → ptr+0x30의 컬러에서 알파 바이트만 0으로 마스킹
     color & 0x00FFFFFFu (ABGR 포맷, 최상위 바이트 = alpha)
   - 원래 위치에 투명하게 렌더됨

4. 원본 CG_DrawHudElem 호출 — 사운드/애니메이션 정상 동작
```

### 왜 skip하지 않고 원본을 호출하는가

CG_DrawHudElem을 아예 건너뛰면 타자기 효과음이 사라진다.
원본 함수는 정상 실행시키되, 좌표 또는 알파만 수정하여
시각적으로만 숨기고 사이드이펙트(사운드)는 보존.

---

## 3) 억제 대상별 전략

| 대상 | 억제 필드 | 오프셋 | 값 | 이유 |
|------|-----------|--------|------|------|
| **오브젝티브** | x (float) | +0x04 | 9999.0f | 화면 밖으로 이동. 오브젝티브는 R_AddCmdDrawText를 타지 않으므로 이 방법만 유효 |
| **타임스크립트** | color alpha | +0x30 | `color & 0x00FFFFFFu` | 원래 위치에서 투명 처리. 위치 변경 시 레이아웃 부작용 가능성 회피 |

### HudElem 필드 오프셋 (사용되는 것만)

| 오프셋 | 타입 | 필드 |
|--------|------|------|
| +0x00 | int32 | type (7=TIMER_DOWN 등) |
| +0x04 | float | x |
| +0x08 | float | y |
| +0x30 | uint32 | color (ABGR packed) |

---

## 4) Suppress 맵 API (`texthook.cpp`)

### 오브젝티브

```cpp
// 억제 등록: x=9999 즉시 적용 + 맵에 originalXBits 저장
void TextHook_ObjSuppressElem(uintptr_t elemPtr, uint32_t originalXBits);

// 만료 복원: 500ms 이상 갱신 없는 엔트리의 x를 originalXBits로 복원
void TextHook_ObjSuppressRestoreStale();

// 전체 복원: 맵 로드/미션 재시작 시 모든 x를 원본으로 복원 후 맵 클리어
void TextHook_ObjSuppressClearAll();
```

**데이터 구조:**

```cpp
struct ObjSuppressEntry {
  uint32_t originalXBits;    // 억제 전 원본 x (IEEE 754 bits)
  DWORD lastRefreshTick;     // 마지막 갱신 시각
};
static std::unordered_map<uintptr_t, ObjSuppressEntry> g_objSuppressedElems;
static std::mutex g_objSuppressMutex;
static std::atomic<bool> g_hasObjSuppressed{false};  // fast-path hint
```

`g_hasObjSuppressed`는 CG_DrawHudElem 호출 빈도(~30/frame)에서
매번 mutex lock을 피하기 위한 fast-path 힌트.
억제 중인 엘리먼트가 없으면 atomic load 하나로 early-out.

### 타임스크립트

```cpp
struct TimeScriptPersistentSuppress {
  uintptr_t rbx;                    // R_AddCmdDrawText 경로의 RBX
  uintptr_t elemPtr;                // 실제 HudElem 포인터
  uint32_t originalColor;           // 억제 전 원본 컬러 (ABGR)
  uint32_t originalFontScaleBits;   // 원본 fontScale (IEEE 754)
  uint32_t cachedTimeField;         // +0x78 절대 종료 시간(ms)
  uint32_t originalType;            // +0x00 원본 타입 (e.g., 7)
  uint32_t originalXBits;           // +0x04 원본 x (IEEE 754)
  DWORD lastSuppressTick;
  bool active;
};
static TimeScriptPersistentSuppress g_TimeScriptPersistSuppress{};
```

`TimeScript_RestorePersistentSuppressIfStale()`: 500ms 이상 갱신 없으면
originalColor, originalType, originalFontScaleBits, originalXBits를 복원하고 active=false.

---

## 5) 로그 키워드

| 로그 메시지 | 의미 |
|-------------|------|
| `[TextHook] CG_DrawHudElem found at RVA 0x...` | 런타임 함수 발견 성공 |
| `[TextHook] SUCCESS: CG_DrawHudElem hook applied!` | MinHook 훅 설치 성공 |
| `[TextHook] FAILED: CG_DrawHudElem hook.` | MinHook 훅 설치 실패 |
| `[TextHook] CG_DrawHudElem: CC-scan failed` | CC 패딩 역방향 탐색 실패 |
| `[TIMESCRIPT-ALPHA-RESTORE]` | 타임스크립트 억제 만료, 원본 복원 |

---

## 6) 주의사항

1. **0x1F087D는 하드코딩된 RVA** — 게임 업데이트 시 변경될 수 있음.
   게임 업데이트가 사실상 불가능하므로(2013년 출시, 서비스 종료) 현실적 위험은 없음.

2. **VirtualQuery 사용 필수** — `IsProbablyExecutableCode()`는 함수 프롤로그 패턴을
   검사하므로 함수 중간 주소(0x1F087D)에서는 false를 반환함.

3. **RCX vs RDX** — hudelem_s 포인터가 어느 레지스터에 오는지는
   호출 규약과 인라이닝에 따라 다를 수 있어 둘 다 검사.

4. **원본 함수는 반드시 호출** — skip 금지 (타자기 사운드 등 사이드이펙트 손실).
