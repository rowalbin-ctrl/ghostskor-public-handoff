# 2026-09-08 maintenance revision R2

Base public handoff commit: `a32baaee2b1b535f95c40c68bb8c61baa221bb78`.

The release DLL targets the exact Steam SP build 24723416 EXE SHA-256:
`e9f62f2b780d2dce8180f4b5fd9f0022e10c9f2596cc8066128c21169b74f110`.

Unknown executables pass through to system DXGI without the game hooks. The
source is not a dual legacy/current build profile and does not support MP.

## Changes

- Reviewed August function/caller relocations and exact EXE/runtime checks.
- Excluded diagnostic engine calls from the release build.
- Synchronized initialization/publication of the text-width function.
- Separated logical font limits from output resolution. HUD pixel heights
  pass through `HudTextRenderer`; TimeScript logical heights use an explicit
  resolution multiplier. Width measurement and drawing use matching scales.
- Read native countdown clock RVA `0x178CCA0`. The parent loads it at
  `0x231081`, stores it at context `+0x238`, and the timer helper `0x231E80`
  subtracts it from HudElem deadline `+0x78`. Relevant code bytes are included
  in the runtime readiness checks.
- Preserve wide HUD recovery and slot reuse guards. Skip clock discovery
  when the native time value is available; publish the computed-number flag.
- Require a fresh numeric replacement before native timer offscreen
  suppression. Feed separated native numeric draws into the matching snapshot
  on every capture; use the existing combined overlay when no usable snapshot
  exists. Clamp expired computed remaining time to zero.

## Build

Use Visual Studio 2022 C++ x64 tools and a Windows SDK. The tested toolchain
was MSVC 14.38 with SDK 10.0.19041, C++17, /MT, /O2, /EHa, and UTF-8.

From `src` run `powershell -NoProfile -File .\build.ps1 -NoDeploy`.
For a standalone toolchain, supply `-ToolchainSetup` with the full path to
its x64 environment setup BAT/CMD. Use `-NoDeploy`: the historical default
deployment path is derived from the source directory hierarchy.

Generated atlases/metrics are embedded by resources.rc. Raw TTF files and
Microsoft build tools are not included. The original vendored source and
license notices are retained. Reproducible byte-identical builds have not
been established; compare source, compiler settings and behavior as well.

## Validation and limits

The x64 MSVC build passed without reported compiler warnings/errors. Native
clock data flow and 34 code signatures were verified against local stock
runtime captures. Those proprietary game captures are not distributed.

`validation/regression.cpp` is the generated test harness used for the actual
scale/gate function bodies and the HUD adapter. It checks 1080p/1440p/4K,
QTE scaling, queue/measurement consistency, and timer readiness conditions.
Rasterization is not exercised by its renderer-boundary stubs.

The user confirmed startup/Korean rendering and, from a loaded mission scene,
timer/Korean output and performance recovery. A whole-campaign replay,
repeated startup menu checks, and 4K visual review remain outstanding.
The Ajax workaround is config guidance, not an engine/DLL lighting patch.
