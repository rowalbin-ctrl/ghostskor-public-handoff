# GhostsKor — Call of Duty: Ghosts 한글패치

Steam **싱글플레이어 빌드 24723416**용 커뮤니티 한글패치의 유지보수 소스입니다.
현재 개정은 **2026.09.08 R5**입니다. 8월 EXE 대응, HUD 크기와 타이머,
시작 메뉴 초기화, 조명 설정 및 함수 주소 검증을 보완했습니다.

## 사용 및 지원 범위

배포 ZIP의 `dxgi.dll`, `localize.json`, `localize_kr.json` 세 파일을
`iw6sp64_ship.exe` 옆에 복사합니다. [설치 안내](docs/INSTALL.ko.txt)와
[변경 내역](docs/CHANGELOG.ko.txt)을 먼저 확인해주세요.

지원 EXE SHA-256:
`e9f62f2b780d2dce8180f4b5fd9f0022e10c9f2596cc8066128c21169b74f110`

멀티플레이, 구 EXE, Microsoft Store/Game Pass판은 지원하지 않습니다.
패턴 스캐너가 있어도 미래 EXE의 자동 호환을 보장하지 않습니다.
알려진 파일·코드·주소 프로필 검증을 통과해야 게임 훅을 설치합니다.
[버전 지원 정책](VERSION_SUPPORT.md)에 업데이트 대응 범위를 설명했습니다.

## 확인된 범위

R2의 일부 불러오기 장면에서 사용자가 한글·타이머 표시와 성능 개선을
확인했습니다. R5는 x64 빌드와 자동 검사를 통과했지만, R5 실게임 시작,
조명 값 전환·에이잭스 장면, 4K 시각 검토, 반복 실행 메뉴 정렬 및
전체 캠페인 검증은 아직 완료하지 않았습니다.

`releases/2026.09.08-R5.json`은 배포 DLL/ZIP의 해시와 검증 범위를 기록합니다.
이 저장소의 Git 파일에는 배포 DLL과 게임 실행 파일을 포함하지 않습니다.

## 빌드

Windows, Visual Studio 2022 C++ x64 도구와 Windows SDK가 필요합니다.
확인한 환경은 MSVC 14.38 / SDK 10.0.19041, C++17, `/MT /O2 /EHa /utf-8`입니다.
저장소 루트의 PowerShell에서 실행합니다.

```powershell
powershell -NoProfile -File .\src\build.ps1 -NoDeploy
```

결과는 `src/build/dxgi.dll`입니다. 독립 도구 모음의 환경 설정 BAT/CMD는
`-ToolchainSetup`으로 지정할 수 있습니다. 빌드에는 인코딩/주소 검사도 포함됩니다.

**반드시 `-NoDeploy`를 사용하세요.** 기존 자동 배포 기능은 소스 폴더의 상위
경로에서 게임 폴더를 추정하고 `dinput8.dll`/`version.dll`을 정리합니다.
일반적인 clone 위치에서는 올바른 설치 경로가 아니므로, 빌드 뒤 DLL만
직접 게임 폴더에 복사하세요.

## 유지보수 자료

- [R2: EXE 이식, HUD·타이머](MAINTENANCE_R2.md)
- [R3: 조명 설정](MAINTENANCE_R3.md)
- [R4: 조명 재시도 제한](MAINTENANCE_R4.md)
- [R5: 함수 검색·주소 프로필](MAINTENANCE_R5.md)
- [기존 스캐너와 8월 충돌 원인](SCANNER_AUDIT.md)
- [검증 소스와 실행 방법](validation/README.md)
- [프로젝트 인수인계](MAINTAINER_HANDOFF.md)

R2~R4 문서는 각 개정 당시 기록입니다. 현재 동작과 지원 범위는 이 README,
R5 문서와 버전 지원 정책을 우선하세요. 기존 SSOT 문서도 이전 구현의 기록입니다.

## 프로젝트 배경과 포함 파일

전문 프로그래머가 아닌 배포자가 AI의 도움으로 구현·디버깅하고 실제 게임에서
시험해 온 커뮤니티 프로젝트입니다. 구현과 휴리스틱은 후속 유지보수와
실게임 검증이 필요한 부분을 포함합니다.

소스, 빌드 스크립트, 기존 생성 글꼴 아틀라스/메트릭과 vendored 의존성 및
라이선스 고지를 포함합니다. 게임 EXE, 메모리 캡처, 세이브, 로컬 설정,
원본 TTF, 빌드 도구와 빌드 산출물은 포함하지 않습니다.
글꼴 리소스는 `src/resources.rc`에 내장되며 원본 TTF 없이 현재 소스를
빌드할 수 있습니다. 글꼴을 교체할 때는 배포 권한을 별도로 확인하세요.
