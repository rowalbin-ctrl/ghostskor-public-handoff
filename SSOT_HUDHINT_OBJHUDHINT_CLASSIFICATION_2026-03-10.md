ㅛ# SSOT: HUDHINT / OBJHUDHINT Classification Contract (2026-03-10)

## 0) Purpose
이 문서는 HUD 프롬프트 분류 기준을 고정한다.
목표는 "하나 고치면 하나 터지는" 현상을 막기 위해, 용어/분기/검증 기준을 단일 문서로 합의하는 것이다.
이 내용은 절대로 메뉴 시스템엔 영향을 끼치지 않는다.
핵심은 게임 내에서 원본이 출력하고 잇는 모든 텍스트를 마찬가지로 "네이티브에 가깝게" 한글 렌더로 하는 것이다. (등장,소멸타이밍 타이밍/위치/크기/펄스/무펄스나 맥동 등 효과 동일)
이 게임의 메뉴/오브젝티브/비디오자막/인게임 자막은 모두 별도이며 그것들은 사실상 완성상태이므로 이것이 영향을 끼치거나 불필요한 데이터 누수가 발생해서는 안된다.
일부 계열의 텍스트이 경우에는 위치와특성을 고정한다.

---

## 1) Terms (Do Not Mix)

### A. Source (capture path)
- `HUD560` (`NATIVE_HUD_SOURCE_HUD560=1`)
- `ADDCMD` (`NATIVE_HUD_SOURCE_ADDCMD=2`)
- `HUDELEM` (`NATIVE_HUD_SOURCE_HUDELEM=3`)
- `BRIDGE` (`NATIVE_HUD_SOURCE_BRIDGE=4`)

Source는 "어디서 잡혔는가"만 의미한다.  
Source는 렌더 스타일을 직접 의미하지 않는다.

### B. Render family (player-facing meaning)
- `OBJHUDHINT` family: 작은 상호작용 문구 (예: 집라인 설치/탑승, 숨참기 계열)
- `HUDHINT` family: 중앙~하단 슬롯 기반 일반 힌트 (tutorial/prompt 포함)

Render family는 "어떻게 보일 것인가"를 의미한다.

### C. Tail-join
- `교체: (무기명)` 형태의 접미 텍스트 결합 정책.
- Tail-join은 분류 체계가 아니라, 특정 key에서만 쓰는 합성 렌더 정책이다.
- 현재 대상 key는 사실상 무기 교체/줍기 4종(`PLATFORM_SWAPWEAPONS`, `PLATFORM_SWAPWEAPONSGAMEPAD`, `PLATFORM_PICKUPNEWWEAPON`, `PLATFORM_PICKUPNEWWEAPONGAMEPAD`)으로 한정한다.
- `:` 뒤 tail은 overlap으로 겹치면 안 된다.
- compact join은 `:` 뒤의 공백을 크게 벌리지 않기 위한 정책일 뿐이며, 최종 렌더는 항상 `:`가 보이는 작은 양수 gap을 유지해야 한다.

---

## 2) Contract Requested by User (Target Behavior)

아래가 이번 작업의 최종 계약이다.

1. `HUD560/ADDCMD`로 들어온 상호작용 프롬프트는 `OBJHUDHINT` 계열로 취급한다.
2. `OBJHUDHINT` 계열은 아래를 고정한다.
- `atlas0`
- 작은 고정형 크기/알파
- 사실상 화면 정중앙 기준 정렬(Y축은 별도일 수 있음.)
- body text RGB는 family 소유이며 native RGB 누수를 허용하지 않는다.
3. 그 외 루트(`BRIDGE/HUDELEM` 등)는 전부 `HUDHINT` 계열로 취급한다.
4. `HUDHINT` 계열은 아래를 따른다.
- `atlas1`
- native 기반 크기/알파/위치 유지
- `OBJHUDHINT`보다 일반적으로 큼
- shadow/pulse/fade 정책 대상
- body text RGB는 family 소유이며, native 검은색/무효 RGB 때문에 검은 글씨가 나오면 안 된다.
- `HUDHINT`로 분류된 순간 shadow는 family 기본값으로 켜져 있어야 하며, slot/caller/key 예외 때문에 빠지면 안 된다.
- `HUDHINT` prompt/tutorial 계열은 family pulse 대상이다.
- 단, 동일 key에서 native alpha 0 샘플/상이한 geometry 샘플이 섞여 들어와 pulse가 flicker로 변질되면 pulse를 끄고 stable alpha/shadow/fade만 유지해야 한다.
- pulse suppression은 프레임 단위가 아니라 key lifecycle 단위로 유지해야 한다. mixed evidence가 한번 확인된 같은 prompt key에서 pulse가 다시 켜지면 안 된다.
- 동일 key의 mixed-caller prompt에서 visible sample이 잠깐 끊겨도, hidden writer가 계속 살아 있으면 마지막 stable geometry를 짧게 유지하여 flash-gap을 막아야 한다.
- 동일 key의 mixed-caller prompt는 key 단위 authority caller를 1개만 고정해야 한다. probe/hidden caller로 winner가 프레임마다 바뀌면 안 된다.
- authority caller live sample이 끊기면, probe caller로 갈아타지 말고 마지막 authority render state를 짧게 fade-out 해야 한다.
- fade-out hold는 authority live render와 동시에 겹쳐 그리면 안 된다. live render가 있는 동안에는 hold를 arm만 하고, 실제 fade draw는 authority가 끊긴 뒤에만 시작한다.

---

## 3) Current Code Snapshot (As-Is, 2026-03-10)

현재 라우팅은 아래 함수가 기준이다.
- `ClassifyNativeHudDisplayChannelDirect`  
  `KOR_PATCH/src/TextHook.HudHint.inl`
- `TextHook_GetHudRouteForNativeEntry`  
  `KOR_PATCH/src/TextHook.HudHint.inl`

현행 핵심 분기:
- `IsDirectAddCmdObjHudHintKey(key)`면 `HUD_ROUTE_OBJHUDHINT`
- 아니면 `slot > 0`이면 `HUD_ROUTE_HINT`

즉, 현재는 "모든 HUD560/ADDCMD"가 자동으로 `OBJHUDHINT`가 되지 않는다.  
이 지점이 실제 체감(사용자 계약)과 코드 계약이 어긋나는 핵심이다.

---

## 4) Do / Do Not Rules

### Do
- Source와 Render family를 반드시 분리해서 판단한다.
- 분류 이슈를 볼 때는 먼저 `HUD-NATIVE-ROUTE`와 최종 `*-RENDER` 로그를 함께 본다.
- `OBJHUDHINT`와 `HUDHINT`의 atlas/size/alpha policy를 절대 공유하지 않는다.
- family가 결정되면 `atlas / size / alpha / anchor / shadow / pulse / fade / body RGB`는 전부 해당 family 정책이 소유한다.
- `HUDHINT` duplicate sample은 final winner를 1개만 남겨야 한다. 같은 key를 두 위치에 번갈아 그리면 안 된다.
- `HUDHINT` duplicate sample winner 선택은 prompt에만 한정되지 않는다. 일반 힌트도 동일 key면 최종 winner 1개만 남겨야 한다.
- `HUDHINT` mixed prompt는 "아무 live source"가 아니라 "authority source 1개"만 렌더 근거로 삼는다.
- `HUDHINT` mixed prompt에서 `bestFamilyByKey`/최종 winner는 authority caller에 종속돼야 하며, support caller가 winner로 승격되면 안 된다.
- 단, mixed prompt lifecycle에서 visible source가 전혀 없고 authority caller만 남는 경우에는, authority caller 1개에 한해서만 family 색/합성 알파를 써서 HUDHINT를 유지할 수 있다. support caller에는 적용 금지.
- strict HUDHINT snapshot(`cfg -> key`) 승격은 objective reverse bias를 직접 신뢰하면 안 된다. 먼저 HUDHINT live prompt/bridge에서 수집한 전용 `cfg -> key` 캐시를 조회해야 한다.
- objective/timer/intro 시스템의 cfg bias 변화 때문에 HUDHINT strict source key가 바뀌면 안 된다. HUDHINT strict source 해석은 HUDHINT 계층 안에서 독립적으로 유지해야 한다.

### Do Not
- `promptLike` 같은 휴리스틱 플래그에 라우팅 의미를 과적재하지 않는다.
- `source=ADDCMD`라는 이유만으로 "무조건 HUDHINT" 혹은 "무조건 OBJHUDHINT"를 암묵 처리하지 않는다.
- Tail-join 로직으로 family 분류를 대체하지 않는다.
- family가 결정된 뒤 native RGB를 그대로 신뢰해서 검은 글씨/무효 색을 렌더하지 않는다.
- `HUDHINT` support source(특히 zero-alpha / zero-rgb hidden probe)를 visible render source로 승격하지 않는다.
- support source는 authority source의 좌표/알파/효과 판정 보조만 할 수 있고, 단독으로 텍스트를 보이게 만들어서는 안 된다.
- zero-alpha hidden prompt probe를 family white/synth alpha로 억지 가시화하는 실험은 2026-03-12 ghost HUD 회귀를 일으켰으므로 금지한다. 2026-03-17 일반 active zero-alpha synth 재시도에서도 동일 회귀 확인 — 범용 synth는 금지하고 **명시적 키 화이트리스트만** 허용한다.
- `menuContext` 판정에서 `IsMenuActive()`를 단독 OR 조건으로 사용하지 않는다. 인게임 텍스트("Defend", "Follow" 등)가 raw 엔진 플래그 노이즈로 메뉴 처리를 받아 center band에서 atlas 1로 플립되는 문제가 발생하므로 (2026-03-17 수정).

---

## 4-1) Zero-Alpha Synth Whitelist (2026-03-17)

일부 키는 게임 엔진이 렌더하지만 native HudElem probe에서 alpha=0으로 읽힌다. 이 키들은 명시적 화이트리스트로 synth 렌더를 허용한다.

| Key | 조건 | fontScale 소스 |
|-----|------|---------------|
| `SATFARM_INITIALIZING` | 한글 번역 존재 | ObjRev cfg fontScale (fs=2.0) |
| `SATFARM_LEAVING_COMBAT` | 한글 번역 존재 | ObjRev cfg fontScale (fs=2.0) |

- ghost HUD 방지: 일반 alpha=0은 synth 금지, 화이트리스트만 허용.
- fontScale: `TextHook_GetNativeCfgFontScale(key)`로 ObjRev 캡처값 조회. 없으면 entry nativeYScale > 1.01 유지, 최종 fallback 1.0.
- off-screen 패널티: `hintAuthorityRank`에서 nativeY < -50 또는 > 530이면 rank -32. 같은 키의 off-screen 프로브가 on-screen 엔트리를 밀어내지 않도록.

---

## 4-2) Native Cfg FontScale Pipeline (2026-03-17)

ObjRev 스캐너가 native HudElem의 fontScale(+0x14)을 캡처하면 `g_NativeCfgFontScaleByKey[key]`에 저장. HUDHINT synth 렌더에서 `TextHook_GetNativeCfgFontScale(key)`로 조회.

```
ObjRev scanner (texthook.cpp) → HUD-NATIVE-HINT-CFG 로그 시점
  → g_NativeCfgFontScaleByKey[resolvedKey] = efs
  → DedicatedHint.inl synth: cfgFs = TextHook_GetNativeCfgFontScale(key)
```

코드 위치:
- 저장: `texthook.cpp` HUD-NATIVE-HINT-CFG 로깅 직후
- 조회: `D3D11Hook.Process.DedicatedHint.inl` synth scale 결정

---

## 5) Diagnostic Checklist (Per Key)

문제가 생긴 key마다 아래 6개를 기록한다.

1. source token (`tok`)  
2. route (`HUD_ROUTE_HINT / HUD_ROUTE_OBJHUDHINT / OBJECTIVE`)  
3. final renderer (`[HUDHINT-RENDER]` or `[OBJHUDHINT-RENDER]`)  
4. atlas (`0/1`)  
5. final screen pos (`pos=(x,y)`)  
6. final size/alpha (`h`, `a`)
7. final body RGB (`rgb=(r,g,b)`)
8. final family shadow/pulse state (`sh`, `pulse`)

필수 로그 태그:
- `[HUD-NATIVE-ROUTE]`
- `[HUDHINT-RENDER]`
- `[OBJHUDHINT-RENDER]`
- 필요 시 `[HUD560]`, `[NHUD-SNAP]`

---

## 6) Example Expectations

### A. `CORNERED_DEPLOY_ZIPLINE_TURRET`
- 기대 family: `OBJHUDHINT`
- 기대 스타일: `atlas0`, 작은 고정형, 정중앙 기준

### B. `CLOCKWORK_PROMPT_STAB`, `SCRIPT_NIGHTVISION_*`
- 기대 family: `HUDHINT`
- 기대 스타일: `atlas1`, native 크기/알파, shadow/pulse/fade 적용, body text는 family white 계열
- pulse가 native instability 때문에 flicker를 만들면, pulse보다 stable HUDHINT 출력이 우선이다.

### C. `PLATFORM_SWAPWEAPONS` (tail-join)
- family는 라우팅 결과를 따르되,
- tail 텍스트 결합은 별도 정책으로만 적용
- tail 정책이 family/atlas를 바꾸면 안 됨
- `:` 문자는 항상 보여야 하며, tail은 prefix 위로 겹쳐 쓰면 안 된다.
- gap은 "넓은 스페이스"가 아니라 얇은 양수 간격 1칸 수준을 목표로 한다.

### D. `FLOOD_LAUNCHER_MELEE`
- 기대 family: `HUDHINT`
- 기대 스타일: `atlas1`, native lower-slot 위치/크기 유지, shadow/pulse/fade 적용
- same-key duplicate(center probe + lower prompt)가 동시에 들어와도 최종 렌더는 1개만 허용

### E. `SCRIPT_INVULERABLE_BULLETS`
- 기대 family: `HUDHINT`
- source가 `BRIDGE`여도 family 규칙을 반드시 적용한다.
- 검은 body text로 나오면 family 적용 실패로 간주한다.
- 동일 key 다중 native sample이 있더라도 final winner는 1개만 남겨야 하며, 임의 샘플 순서에 따라 크기/위치가 바뀌면 안 된다.

### F. `CLOCKWORK_HINT_DRILL` / `CLOCKWORK_HINT_DRILL_PICKUP`
- 기대 family: `HUDHINT`
- **CG_DrawHudElem 좌표 캡처 (2026-03-18 신규)**: 이 키들은 기존의 ObjRev bridge(+0x1F087D) SEH 호출이 발사되지 않아 블라인드 레지스터 프로빙으로 잘못된 좌표를 받았다. `CgDrawHudElemCapture` 시스템으로 해결:
  - **DRILL**: `Detour_CG_DrawHudElem`에서 실제 hudelem_s 포인터를 저장 → CG_DrawHudElem 내부의 **두 번째 SEH 호출**(+0x1F1762, label 경로)에서 `g_CGDrawHudElemPtr != 0`이면 좌표 캡처 → alignment normalization 적용
  - **DRILL_PICKUP**: +0x1F1762도 발사되지 않음 → Detour에서 모든 elemPtr의 좌표를 캐시 → SEH 훅에서 key 확인 후 HudElem 배열 스캔(text/label cfg→SLC 매칭)으로 elemPtr 찾기 → 캐시된 좌표 사용
- **실제 HudElem 데이터 (DRILL 기준)**: `x=0.00, y=-40.00, alignScreen=0x22(CENTER×CENTER), fontScale=1.600, alpha=0.80` → screen=(960, 450) 정확한 중앙
- **이전 규칙 폐기 (2026-03-18)**:
  - ~~DRILL authority caller +0x204816 우선 규칙~~ → CG_DrawHudElem 캡처가 우선
  - ~~hasRealNativeScale 게이트 면제~~ → CG_DrawHudElem 캡처의 fontScale이 정확하므로 불필요
  - ~~s_drillBaselineScale~~ → 이미 폐기 상태 유지
- `DedicatedHint.inl`에서 `TextHook_GetCgDrawCapture(key)` 호출 시 alignment normalization 자동 적용. 기존 nativeCoordToScreen 대비 정확한 screen anchor 사용.

---

## 7) Migration Guidance (Code Work Order)

향후 수정은 아래 순서로 한다.

1. 분류 함수 1곳에서 family를 결정한다.  
2. renderer는 family 결과만 소비한다.  
3. atlas/size/alpha/anchor/shadow/body RGB를 family별로 완전 분리한다.  
4. 휴리스틱(`promptLike`)은 "보조 우선순위"로만 쓰고 분류 계약을 대체하지 않는다.
5. strict HUDHINT source 승격은 `HUDHINT live cfg cache -> ObjRev fallback` 순서를 지킨다. objective reverse 해석이 HUDHINT visible source를 오염시키면 안 된다.

---

## 8) QTE Visual Pulse

### 목적
게임 엔진이 QTE(Quick Time Event) 장면에서 키 바인딩 텍스트(예: "F")를 커졌다 작아지면서 연타/조작을 유도하는 효과를 한글 렌더러에서도 재현한다.

### 적용 조건
QTE 시각 펄스는 **HUDHINT** family 프롬프트에서 아래 조건을 **모두** 만족할 때 적용된다:

1. `isPromptFamilyKey == true` (center prompt / LUI hint slot 계열)
2. `StripColorCodesSimple(displayText).length()` 가 **1~5 바이트** (= 짧은 키 바인딩 텍스트)

즉, 렌더링할 한국어 텍스트가 사실상 키 이름만(`[F]`, `[E]` 등)으로 구성된 경우에만 해당한다.
긴 설명문 힌트(예: "^3[F]^7 버튼을 길게 눌러 드릴링하십시오")에는 적용되지 않는다.

### 적용 대상 예시

| Key | displayText (색상코드 제거 후) | 길이 | 펄스 |
|-----|-------------------------------|------|------|
| `SKYWAY_HINT_RELOAD` | `[F]` | 3 | **적용** |
| `CLOCKWORK_HINT_DRILL` | `왼쪽 마우스 버튼을 길게 눌러 드릴링하십시오` | 30+ | 미적용 |
| `SKYWAY_HINT_BRIDGE` | `[F] 버튼을 길게 눌러 다리를 내리십시오` | 20+ | 미적용 |
| `PLATFORM_SWAPWEAPONS` | `교체: AK-12` 등 | 10+ | 미적용 |

### 파라미터

| 항목 | 값 | 비고 |
|------|-----|------|
| 주파수 | ~2.86 Hz (주기 350ms) | 연타 리듬에 대응 |
| 진폭 | ±18% | `scale * (1.0 + 0.18 * sin(phase))` → 0.82x ~ 1.18x |
| 적용 대상 | `scale` + `fontHeight` | 센터 정렬 재계산 포함 |

### 렌더링 동작
- 펄스는 **폭 측정 및 센터 정렬 직전**에 적용되므로, 텍스트가 중심축 기준으로 대칭 펄싱된다.
- `promptPulseEligible`(알파 펄스)과 독립적으로 동작한다. 알파 펄스는 투명도 진동, QTE 펄스는 크기 진동이다.
- fade-hold 캐시에는 마지막 펄스 상태의 scale/position이 저장되며, fade-out 중에는 고정 크기로 페이드된다.

### rawNativeScale과의 관계
- DRILL 베이스라인 오버라이드(`s_drillBaselineScale`) 적용 **전** 네이티브 스케일을 `rawNativeScale`로 보존한다.
- `qteScaleActive` 감지는 `rawNativeScale`을 사용하여 베이스라인 오버라이드가 네이티브 애니메이션 감지를 가리지 않도록 한다.
- `qteScaleActive`는 QTE 시각 펄스의 트리거가 아닌, 레이아웃 캐싱 로직(stable/authority layout에서 animated scale을 보존/격리)에서만 사용된다.

### 진단 로그
- `[HUDHINT-RENDER]` 태그에 `qte=1/0` 플래그가 추가되어 QTE 펄스 적용 여부를 확인할 수 있다.

### Do / Do Not

**Do:**
- QTE 펄스는 `isPromptFamilyKey`이면서 짧은 키 바인딩 텍스트에만 적용한다.
- 신규 QTE 키가 발견되면 localize_kr.json에서 해당 키의 한국어 번역이 키 바인딩만 포함하는지 확인한다.

**Do Not:**
- 긴 설명문 힌트에 QTE 펄스를 적용하지 않는다.
- `qteScaleActive`를 시각 펄스 트리거로 사용하지 않는다 (레이아웃 캐싱 전용).
- 펄스 주파수/진폭을 과도하게 올려서 가독성을 해치지 않는다.

### 코드 위치
- QTE 시각 펄스: `D3D11Hook.Process.DedicatedHint.inl` (폭 측정 직전 블록)
- `rawNativeScale` 선언: 같은 파일, `scale` 할당 직후
- QTE 스케일 감지: 같은 파일, `s_qteScale` 맵 / `qteScaleActive` 플래그

---

## 9) CG_DrawHudElem Coordinate Capture (2026-03-18)

### 발견
CG_DrawHudElem(RVA 0x1F06B0)은 SEH_StringEd_GetString을 **두 번** 호출한다:
- **+0x1F087D** (offset 461): `text` 필드 해석 — 대부분의 HUDHINT 키가 이 경로
- **+0x1F1762** (offset 4274): `label` 필드 해석 — DRILL 등 text=0인 키가 이 경로

### 시스템 구조

```
Detour_CG_DrawHudElem
  ├─ 모든 elemPtr의 좌표를 elemPtr 기준 캐시 (StoreCgDrawElemPtrCapture)
  ├─ g_CGDrawHudElem_ElemPtr = elemPtr
  └─ Original_CG_DrawHudElem 호출
       ├─ SEH at +0x1F087D (text) → key 해석 → StoreCgDrawCapture(key, 좌표)
       └─ SEH at +0x1F1762 (label) → key 해석 → StoreCgDrawCapture(key, 좌표)

SEH at +0x204816 (CG_DrawHudElem 밖)
  └─ g_CGDrawHudElemPtr == 0
  └─ Fallback: HudElem 배열 스캔 → text/label cfg→SLC 매칭
     → elemPtr 특정 → GetCgDrawElemPtrCapture → StoreCgDrawCapture

DedicatedHint.inl 렌더링
  └─ GetCgDrawCapture(key) → alignment normalization → screen 좌표
```

### 진단 로그
- `[CG-DRAW-CAPTURE]`: SEH 내부에서 캡처 성공. `[SCAN]` 접미사는 배열 스캔 fallback 사용
- `[CG-DRAW-OVERRIDE]`: DedicatedHint에서 캡처 데이터로 좌표 덮어쓰기 적용

---

## 10) Source References

- `KOR_PATCH/src/TextHook.h` — CgDrawHudElemCapture 구조체/API 선언
- `KOR_PATCH/src/texthook.cpp` — CgDrawCapture 저장/조회 구현
- `KOR_PATCH/src/TextHook.HudHint.inl`
- `KOR_PATCH/src/TextHook.HookInstallAndDetours.inl` — Detour + SEH 캡처 로직
- `KOR_PATCH/src/D3D11Hook.Process.DedicatedHint.inl` — CG-DRAW-OVERRIDE 적용
- `KOR_PATCH/src/D3D11Hook.Process.DedicatedObjHudHint.inl`
- `KOR_PATCH/src/D3D11Hook.Process.HudOverlayPrelude.inl`
- `KOR_PATCH/SSOT_CG_DRAWHUDELEM_HOOK_2026-03-15.md` — CG_DrawHudElem 훅 상세
