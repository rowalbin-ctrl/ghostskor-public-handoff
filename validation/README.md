# Validation harnesses

From an x64 Visual Studio developer prompt in the archive root:

```
cl /nologo /std:c++17 /EHsc /utf-8 /I src validation/test_dynamic_light_policy.cpp /Fe:lighting_policy_test.exe
lighting_policy_test.exe
cl /nologo /std:c++17 /EHsc /utf-8 /I src validation/regression.cpp /Fe:hud_timer_test.exe
hud_timer_test.exe
```

The additional test_matcher.cpp and test_publication.cpp exercise the actual
production matcher/resolver. Compile with /I src, and for test_publication.cpp
also compile src/GameAddresses.cpp with /EHa. They take local code-section
RVA/file pairs (publication test also takes stock/missing/duplicate/moved/unreadable
as its first argument). Proprietary code captures are not included.

These policy/renderer-boundary checks do not execute the game's lighting
command queue or validate full-campaign behavior. Stock game captures are
intentionally excluded. Read MAINTENANCE_R5.md for scope and build details.

## Native menu and DXGI integration

From the same x64 developer prompt:

```
cl /nologo /O2 /EHa /std:c++17 /utf-8 /I src validation/test_menu_geometry.cpp /Fe:menu_geometry_test.exe
menu_geometry_test.exe
cl /nologo /O2 /EHa /std:c++17 /utf-8 /DGHOSTSKOR_LOGGING=0 /I src validation/test_native_text.cpp src/NativeMenuCapture.cpp src/KoreanAtlas.cpp /Fe:native_text_test.exe
native_text_test.exe src/font_metrics.bin src/font_metrics2.bin
cl /nologo /O2 /EHa /std:c++17 /utf-8 /DGHOSTSKOR_TESTING /DGHOSTSKOR_LOGGING=0 /I src validation/test_native_menu_draw.cpp src/NativeMenuDraw.cpp /Fe:native_menu_draw_test.exe
native_menu_draw_test.exe
cl /nologo /O2 /EHa /std:c++17 /utf-8 /I src validation/test_dxgi_lifecycle.cpp src/DXGIWrapper.cpp src/DXGIProxy.cpp /Fe:dxgi_lifecycle_test.exe d3d11.lib dxgi.lib dxguid.lib user32.lib
dxgi_lifecycle_test.exe
```

These exercise complete/empty menu frames, partial UV clipping at seven aspect/
resolution combinations, the six production hooks and all 13 LUI text arguments,
exact fresh/cached alignment and wrapping, Latin cap calibration in both supplied
atlases with a fixed Hangul centre, and all three shipping DXGI exports using
D3D11 WARP. The DXGI test checks nine recreations, 90 repeated wrapping attempts,
27 Presents/resizes, COM lifetime and vertex-shader buffer restoration.

The ordered menu draw test uses the production command handler. It checks
native geometry flush before Korean text, text ordering across popup batches,
cached command replay, expired markers, bounded marker parsing and nested
capture restoration. It does not reproduce the game's complete command list.

For live regression checks, include credits playback, its pause menu and its
quit confirmation, then return to the campaign menu. All native menu hooks must
remain active throughout. The former credits workaround disabled hooks globally;
that suppressed English while also disabling the Korean command handler, leaving
blank labels and dialogs. A passing standalone renderer test cannot detect a
separate runtime path disabling these hooks.

Build normal candidates without `-MenuTrace`. `-MenuTrace` is a diagnostic option
with an exported ring buffer and stack capture overhead, not a release setting.
These harnesses do not establish full campaign or all-menu visual correctness.

## Native gameplay glyph integration

```
cl /nologo /O2 /EHa /std:c++17 /utf-8 /DGHOSTSKOR_LOGGING=0 /I src validation/test_native_hud_caption.cpp /Fe:native_hud_caption_test.exe
native_hud_caption_test.exe
cl /nologo /O2 /EHa /std:c++17 /utf-8 /DGHOSTSKOR_LOGGING=0 /I src validation/test_native_glyph_bridge.cpp src/KoreanAtlas.cpp /Fe:native_glyph_bridge_test.exe
native_glyph_bridge_test.exe src/font_metrics.bin
cl /nologo /O2 /EHa /std:c++17 /utf-8 /DGHOSTSKOR_LOGGING=0 /I src validation/test_asset_localization.cpp /Fe:asset_localization_test.exe
asset_localization_test.exe
```

These invoke the production native text and glyph adapters. The glyph check
uses the supplied atlas and checks all 11,172 Hangul syllables against the native
Latin cap centre, including the opposite signs of atlas bearingY and IW6 y0.
The asset check ensures repeated intro/status/hint lookups preserve native values
and only register content. Callback tests preserve the 17 effect arguments and
the backend's glyph geometry/color/order, but do not execute all native effect
machine code or replace live visual testing of fade, wrapping and animations.
They also exercise native scaled width only within the HUD preparation scope,
preserve the resulting draw coordinates, distinguish an identical dialogue/HUD
sentence by its actual owner, and keep a native label's runtime timer value.

## Text intake and native HUD ownership

```
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /I src validation/test_text_intake.cpp /Fe:text_intake_test.exe
text_intake_test.exe
```

This checks per-string exclusions, supported HUD key/prompt paths, and nested,
exception-unwound and thread-local native HUD ownership. It does not measure
game FPS. Credits must never disable translation hooks globally.

## Dialogue and cinematic viewports

```
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /I src validation/test_subtitle_viewport.cpp /Fe:subtitle_viewport_test.exe
subtitle_viewport_test.exe
```

Uses live 3840x2160 and 1600x1200 ScreenPlacement snapshots, native cinematic
pixel-aspect inputs, resize mismatches, invalid values, and 11 output sizes.
Checks that the native movie rectangle can differ at the same resolution when
the game's aspect setting changes. Existing 16:9 wrap widths remain unchanged.
These are geometry checks, not visual certification of every video/display mode.

`build.ps1 -TextPerf` is an optional diagnostic build with exported Present/text
counters and an access-violation observer. Omit both `-TextPerf` and `-MenuTrace`
for release builds. A synthetic benchmark or visible translated credits menu
does not establish credits frame-time performance.
