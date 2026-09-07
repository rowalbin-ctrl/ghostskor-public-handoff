#pragma once
#include <cstdint>

// =============================================================================
// IW6 Engine Function Offsets (from IW6x-client project)
// SP: stock Steam build 24723416, guarded by GameBuild.cpp.
// MP constants are historical reference only; MP initialization is blocked.
// Base address: 0x140000000 (standard x64 ASLR base)
// =============================================================================

namespace IW6Offsets {

// Function values below are reference IDs. Resolve them through GetAddress;
// they must never be added to the module base to create a callable pointer.
// Format: {SP_offset, MP_offset}

// Localization Functions (from iw6x-client localized_strings.cpp)
// VERIFIED: From iw6x-client src/client/component/localized_strings.cpp line 61
constexpr uintptr_t SEH_StringEd_GetString_SP = 0x436A20;  // SELECT_VALUE(0x1403F42D0, ...)
constexpr uintptr_t SEH_StringEd_GetString_MP = 0x4A5F60;

// UI Localization
constexpr uintptr_t UI_LocalizeMapname_SP = 0;
constexpr uintptr_t UI_LocalizeMapname_MP = 0x4B96D0;
constexpr uintptr_t UI_LocalizeGametype_MP = 0x4B90F0;

// String Functions
constexpr uintptr_t SL_ConvertToString_SP = 0x417230;
constexpr uintptr_t SL_ConvertToString_MP = 0x4317F0;

// Command Buffer Functions (from iw6x-client symbols.hpp)
// Used to intercept console commands: loadgame_continue, fast_restart, etc.
constexpr uintptr_t Cbuf_AddText_SP = 0x3F3C00;   // void(int localClientNum, const char* text)
constexpr uintptr_t Cbuf_AddText_MP = 0x3F6B50;

// Rendering Functions
// NOTE: Offsets can vary across builds/patches. TextHook.cpp validates the
// prologue and falls back if needed.
constexpr uintptr_t R_AddCmdDrawText_SP = 0x57DA90;
constexpr uintptr_t R_AddCmdDrawText_SP_FALLBACK = 0; // No unverified fallback.
constexpr uintptr_t R_AddCmdDrawText_MP = 0x601070;
constexpr uintptr_t R_AddCmdDrawTextWithCursor_SP = 0x57DC10;  // Text with cursor (input fields)
constexpr uintptr_t R_AddCmdDrawTextWithCursor_MP = 0x6013A0;
constexpr uintptr_t R_RegisterFont_SP = 0x55C050;
constexpr uintptr_t R_RegisterFont_MP = 0x5DFAC0;
constexpr uintptr_t R_TextWidth_SP = 0x55C320;
constexpr uintptr_t R_TextWidth_SP_FALLBACK = 0;
constexpr uintptr_t R_TextWidth_MP = 0x5DFDB0;
constexpr uintptr_t R_EndFrame_SP = 0x534860;
constexpr uintptr_t R_EndFrame_MP = 0x601AA0;

// LUI (Lua UI) Functions
constexpr uintptr_t LUI_OpenMenu_SP = 0x3FD460;
constexpr uintptr_t LUI_OpenMenu_MP = 0x4B3610;
constexpr uintptr_t LUI_EnterCriticalSection_SP = 0x1AE940;
constexpr uintptr_t LUI_EnterCriticalSection_MP = 0x1CD040;
constexpr uintptr_t LUI_LeaveCriticalSection_SP = 0x1B0AA0;
constexpr uintptr_t LUI_LeaveCriticalSection_MP = 0x1CF1A0;

// HUD Functions (potential subtitle/briefing rendering)
constexpr uintptr_t CG_GameMessage_SP = 0x233C90;
constexpr uintptr_t CG_GameMessage_MP = 0x271320;

// Database Functions
constexpr uintptr_t DB_FindXAssetHeader_SP        = 0x2B0D70;
constexpr uintptr_t DB_FindXAssetHeader_MP        = 0x31F3A0;
constexpr uintptr_t DB_EnumXAssets_Internal_SP    = 0x2B0AD0; // SP; enumerate all assets of a type
constexpr uintptr_t DB_IsLocalized_SP = 0x273210;
constexpr uintptr_t DB_IsLocalized_MP = 0x320360;

// Dvar Functions
constexpr uintptr_t Dvar_FindVar_SP = 0x470630;
constexpr uintptr_t Dvar_FindVar_MP = 0x4ECB60;
constexpr uintptr_t Dvar_GetBool_SP = 0x4707D0;
constexpr uintptr_t Dvar_GetInt_SP = 0x4708B0;
constexpr uintptr_t Dvar_GetVariantStringWithDefault_SP = 0x470C40;

// Screen Placement (for proper positioning)
constexpr uintptr_t ScrPlace_GetViewPlacement_SP = 0x28C4C0;
constexpr uintptr_t ScrPlace_GetViewPlacement_MP = 0x2F6D40;

// =============================================================================
// NEW OFFSETS (from iw6x-client symbols.hpp)
// Added 2026-02-01 for HUD/subtitle animation sync testing
// =============================================================================

// HUD Element Functions (for native subtitle state capture)
// HudElem_Alloc: Allocates HUD elements - can help find HudElem array base
constexpr uintptr_t HudElem_Alloc_SP = 0;  // Not available in SP
constexpr uintptr_t HudElem_Alloc_MP = 0x3997E0;
// Reversed SP HudElem array candidate used by live polling fast-path.
// Relocation of the historical search anchor 0x147EDAC. This is a search
// region, not proof that every scanned slot belongs to the native HUD array.
// Keep the historical fallback coverage until mission-by-mission validation.
constexpr uintptr_t HudElem_Array_SP = 0x160865C;
constexpr int HudElem_Array_Stride_SP = 0xA8;
// A separate native draw loop covers 256 entries at 0x160922C. It must NOT
// be substituted for the recovery-search region above without coverage tests.
constexpr uintptr_t HudElem_RenderArray_SP = 0x160922C;
constexpr int HudElem_RenderArray_Count_SP = 256;
// Native countdown clock as well as color/font interpolation. 0x231081 stores
// it at renderCtx+0x238; 0x2316E0/0x2317A1 pass it to 0x231E80 (end - now).
constexpr uintptr_t ClientGameTime_SP = 0x178CCA0;
// Configstring->stringId resolver used by objective/hint reverse path.
// Reversed from runtime callsites around +0x328E30/+0x3298B0:
//   lea ecx, [cfg + 0xAC] / call +0x4915A0 / mov ecx,eax / call SL_ConvertToString
constexpr uintptr_t ConfigString_IndexToSlc_SP = 0x4DA960;
// String id type-check path used immediately before objective SL_ConvertToString
// callsite (+0x34E25E). Returns string type in EAX (cmp eax,3 in caller).
constexpr uintptr_t SL_StringTypeCheck_SP = 0x41EE30;
// Legacy (incorrect for cfg indexes) kept for compatibility.
constexpr uintptr_t ConfigString_Resolve_SP = SL_ConvertToString_SP;

// Video/Cinematic Functions (for loading screen subtitles)
// These are less useful but may help trace video subtitle rendering
constexpr uintptr_t R_AddCmdDrawStretchPic_SP = 0x234460;
constexpr uintptr_t R_AddCmdDrawStretchPic_MP = 0x600BE0;

// Menu Functions (for context detection)
constexpr uintptr_t Menu_IsMenuOpenAndVisible_SP = 0;
constexpr uintptr_t Menu_IsMenuOpenAndVisible_MP = 0x4B38A0;

// String List Functions (additional localization)
constexpr uintptr_t SL_GetString_SP = 0x3D6CD0;
constexpr uintptr_t SL_GetString_MP = 0x431C70;
constexpr uintptr_t SL_FindString_SP = 0x3D6AF0;
constexpr uintptr_t SL_FindString_MP = 0x431A90;

// =============================================================================
// GLOBAL VARIABLE OFFSETS (from iw6x-client)
// These are data addresses, not function addresses
// =============================================================================

// SP-specific global data
namespace SPGlobals {
    // gentity_s array base (from symbols.hpp: sp::g_entities)
    constexpr uintptr_t g_entities = 0x3E39D00;  // 0x143C91600 - base

    // Zone data (for asset loading context)
    constexpr uintptr_t g_zones_0 = 0x362DFC8;   // 0x1434892D8 - base
}

// MP-specific global data
namespace MPGlobals {
    // cg_s array (client game state, contains HUD info)
    constexpr uintptr_t cgArray = 0x176EC00;     // 0x14176EC00 - base

    // gentity_s array base
    constexpr uintptr_t g_entities = 0x427A0E0;  // 0x14427A0E0 - base

    // Game time (for animation sync)
    constexpr uintptr_t gameTime = 0x43F4B6C;    // 0x1443F4B6C - base
    constexpr uintptr_t serverTime = 0x647B280;  // 0x14647B280 - base
}

// SEH_StringEd_GetString Signature Patterns for fallback scanning
// Pattern: Start of function prologue
namespace Signatures {
    // SEH_StringEd_GetString typical prologue pattern
    // 48 89 5C 24 ?? 57 48 83 EC ?? (mov [rsp+?], rbx; push rdi; sub rsp, ?)
    constexpr const char* SEH_StringEd_GetString_Pattern = "48 89 5C 24 ? 57 48 83 EC";
}

// Additional reviewed function IDs used by direct hooks.
constexpr uintptr_t CG_DrawHudElem_SP = 0x2314F0;
constexpr uintptr_t IntroSubRender_SP = 0x231DE0;
constexpr uintptr_t IntroRender_SP = 0x232540;
constexpr uintptr_t HudTextRender_SP = 0x23C4E0;
constexpr uintptr_t ConfigStringWrapper_SP = 0x273760;
constexpr uintptr_t SubtitleWrapper_SP = 0x273EA0;
constexpr uintptr_t SubtitleEnqueue_SP = 0x274330;
constexpr uintptr_t SubtitlePrecheck_SP = 0x280750;
constexpr uintptr_t SubtitleBranch_SP = 0x2807A0;
constexpr uintptr_t IntroTextLayout_SP = 0x4364C0;
constexpr uintptr_t TextFxRender_SP = 0x436B80;
constexpr uintptr_t HUD_DrawText_SP = 0x444DC0;
constexpr uintptr_t IntroActualRender_SP = 0x45CA50;
constexpr uintptr_t Dvar_SetBool_SP = 0x472310;

namespace Profile {
constexpr uintptr_t SearchImageSpan = 0x10000000; // Existing wide recovery search bound.
// Version-bound caller/data/legacy probe locations, preserved from R4.
// These are NOT automatically portable. Unknown EXEs remain disabled.
constexpr uintptr_t Rva_183FEC = 0x183FEC; // texthook.cpp:2598
constexpr uintptr_t Rva_18423B = 0x18423B; // texthook.cpp:2598
constexpr uintptr_t Rva_1843DB = 0x1843DB; // texthook.cpp:2599
constexpr uintptr_t Rva_18465B = 0x18465B; // texthook.cpp:366
constexpr uintptr_t Rva_18477E = 0x18477E; // texthook.cpp:366
constexpr uintptr_t Rva_1F1B00 = 0x1F1B00; // D3D11Hook.Process.DedicatedHint.inl:1224
constexpr uintptr_t Rva_1F622C = 0x1F622C; // TextHook.HookInstallAndDetours.inl:9324
constexpr uintptr_t Rva_1F9400 = 0x1F9400; // TextHook.HookInstallAndDetours.inl:9289
constexpr uintptr_t Rva_1FC990 = 0x1FC990; // TextHook.HookInstallAndDetours.inl:9289
constexpr uintptr_t Rva_2006A0 = 0x2006A0; // D3D11Hook.ObjectiveSubsystem.inl:439
constexpr uintptr_t Rva_200780 = 0x200780; // D3D11Hook.ObjectiveSubsystem.inl:439
constexpr uintptr_t Rva_200B30 = 0x200B30; // D3D11Hook.ObjectiveSubsystem.inl:439
constexpr uintptr_t Rva_204C33 = 0x204C33; // TextHook.HookInstallAndDetours.inl:9329
constexpr uintptr_t Rva_2316DB = 0x2316DB; // D3D11Hook.Process.DedicatedHint.inl:210
constexpr uintptr_t Rva_232562 = 0x232562; // D3D11Hook.Process.DedicatedHint.inl:326
constexpr uintptr_t Rva_234180 = 0x234180; // D3D11Hook.ObjectiveSubsystem.inl:440
constexpr uintptr_t Rva_234FA0 = 0x234FA0; // D3D11Hook.ObjectiveSubsystem.inl:440
constexpr uintptr_t Rva_23E452 = 0x23E452; // D3D11Hook.ObjectiveSubsystem.inl:440
constexpr uintptr_t Rva_2454FC = 0x2454FC; // D3D11Hook.Process.DedicatedHint.inl:324
constexpr uintptr_t Rva_273F0A = 0x273F0A; // TextHook.SubtitleStringEd.inl:74
constexpr uintptr_t Rva_27DF17 = 0x27DF17; // TextHook.HookInstallAndDetours.inl:5323
constexpr uintptr_t Rva_2B1453 = 0x2B1453; // TextHook.HookInstallAndDetours.inl:447
constexpr uintptr_t Rva_367F60 = 0x367F60; // texthook.cpp:2594
constexpr uintptr_t Rva_3680B2 = 0x3680B2; // TextHook.HookInstallAndDetours.inl:3268
constexpr uintptr_t Rva_3682F7 = 0x3682F7; // texthook.cpp:2594
constexpr uintptr_t Rva_369002 = 0x369002; // TextHook.HookInstallAndDetours.inl:1031
constexpr uintptr_t Rva_3720F2 = 0x3720F2; // TextHook.HookInstallAndDetours.inl:2423
constexpr uintptr_t Rva_389BB5 = 0x389BB5; // TextHook.HookInstallAndDetours.inl:446
constexpr uintptr_t Rva_389BF7 = 0x389BF7; // texthook.cpp:366
constexpr uintptr_t Rva_3E85AB = 0x3E85AB; // TextHook.HookInstallAndDetours.inl:9326
constexpr uintptr_t Rva_4156EC = 0x4156EC; // texthook.cpp:367
constexpr uintptr_t Rva_4DAC50 = 0x4DAC50; // texthook.cpp:367
constexpr uintptr_t Rva_555EA0 = 0x555EA0; // TextHook.HookInstallAndDetours.inl:9891
constexpr uintptr_t Rva_580D53 = 0x580D53; // TextHook.HookInstallAndDetours.inl:9325
constexpr uintptr_t Rva_589C31 = 0x589C31; // TextHook.HookInstallAndDetours.inl:9327
constexpr uintptr_t Rva_5DB732 = 0x5DB732; // TextHook.HookInstallAndDetours.inl:569
constexpr uintptr_t Rva_5DBB01 = 0x5DBB01; // TextHook.HookInstallAndDetours.inl:569
constexpr uintptr_t Rva_5DC13F = 0x5DC13F; // TextHook.HookInstallAndDetours.inl:570
constexpr uintptr_t Rva_5DC266 = 0x5DC266; // TextHook.HookInstallAndDetours.inl:570
constexpr uintptr_t Rva_5DC637 = 0x5DC637; // TextHook.HookInstallAndDetours.inl:570
constexpr uintptr_t Rva_5DC950 = 0x5DC950; // TextHook.HookInstallAndDetours.inl:654
constexpr uintptr_t Rva_5DD45F = 0x5DD45F; // TextHook.HookInstallAndDetours.inl:569
constexpr uintptr_t Rva_1400000 = 0x1400000; // texthook.cpp:7768
constexpr uintptr_t Rva_1473520 = 0x1473520; // TextHook.TimeScript.inl:368
constexpr uintptr_t Rva_1473524 = 0x1473524; // TextHook.TimeScript.inl:368
constexpr uintptr_t Rva_1473528 = 0x1473528; // TextHook.TimeScript.inl:368
constexpr uintptr_t Rva_147352C = 0x147352C; // TextHook.TimeScript.inl:368
constexpr uintptr_t Rva_1478000 = 0x1478000; // texthook.cpp:7684
constexpr uintptr_t Rva_1478004 = 0x1478004; // texthook.cpp:7684
constexpr uintptr_t Rva_1478008 = 0x1478008; // texthook.cpp:7684
constexpr uintptr_t Rva_147ED00 = 0x147ED00; // texthook.cpp:7682
constexpr uintptr_t Rva_147ED04 = 0x147ED04; // texthook.cpp:7682
constexpr uintptr_t Rva_147ED08 = 0x147ED08; // texthook.cpp:7682
constexpr uintptr_t Rva_1550000 = 0x1550000; // texthook.cpp:7690
constexpr uintptr_t Rva_1550004 = 0x1550004; // texthook.cpp:7690
constexpr uintptr_t Rva_1560000 = 0x1560000; // texthook.cpp:7690
constexpr uintptr_t Rva_1580000 = 0x1580000; // texthook.cpp:7768
constexpr uintptr_t Rva_15B0000 = 0x15B0000; // texthook.cpp:7685
constexpr uintptr_t Rva_15B0004 = 0x15B0004; // texthook.cpp:7685
constexpr uintptr_t Rva_15B0008 = 0x15B0008; // texthook.cpp:7685
constexpr uintptr_t Rva_1640E38 = 0x1640E38; // D3D11Hook.ObjectiveSubsystem.inl:995
constexpr uintptr_t Rva_176EC00 = 0x176EC00; // texthook.cpp:7680
constexpr uintptr_t Rva_17BF4B8 = 0x17BF4B8; // D3D11Hook.Process.InGameSubtitles.inl:60
constexpr uintptr_t Rva_17BFC8C = 0x17BFC8C; // D3D11Hook.ObjectiveSubsystem.inl:996
constexpr uintptr_t Rva_17D97D0 = 0x17D97D0; // D3D11Hook.ObjectiveSubsystem.inl:459
constexpr uintptr_t Rva_17FC430 = 0x17FC430; // D3D11Hook.ObjectiveSubsystem.inl:505
constexpr uintptr_t Rva_3C80000 = 0x3C80000; // texthook.cpp:7755
constexpr uintptr_t Rva_3C914F8 = 0x3C914F8; // texthook.cpp:7688
constexpr uintptr_t Rva_3C914FC = 0x3C914FC; // texthook.cpp:7688
constexpr uintptr_t Rva_3C91500 = 0x3C91500; // texthook.cpp:7687
constexpr uintptr_t Rva_3C91504 = 0x3C91504; // texthook.cpp:7687
constexpr uintptr_t Rva_3C91508 = 0x3C91508; // texthook.cpp:7687
constexpr uintptr_t Rva_3F00000 = 0x3F00000; // texthook.cpp:7760
constexpr uintptr_t Rva_43F4B60 = 0x43F4B60; // texthook.cpp:7692
constexpr uintptr_t Rva_43F4B64 = 0x43F4B64; // texthook.cpp:7692
constexpr uintptr_t Rva_43F4B68 = 0x43F4B68; // texthook.cpp:7692
constexpr uintptr_t Rva_43F4B70 = 0x43F4B70; // texthook.cpp:7692
constexpr uintptr_t Rva_49B00A8 = 0x49B00A8; // TextHook.TimeScript.inl:1650
} // namespace Profile
} // namespace IW6Offsets
