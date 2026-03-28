#pragma once
#include <cstdint>

// =============================================================================
// IW6 Engine Function Offsets (from IW6x-client project)
// Single Player (SP) and Multiplayer (MP) offsets
// Base address: 0x140000000 (standard x64 ASLR base)
// =============================================================================

namespace IW6Offsets {

// Base addresses (will be added to module base at runtime)
// Format: {SP_offset, MP_offset}

// Localization Functions (from iw6x-client localized_strings.cpp)
// VERIFIED: From iw6x-client src/client/component/localized_strings.cpp line 61
constexpr uintptr_t SEH_StringEd_GetString_SP = 0x3F42D0;  // SELECT_VALUE(0x1403F42D0, ...)
constexpr uintptr_t SEH_StringEd_GetString_MP = 0x4A5F60;

// UI Localization
constexpr uintptr_t UI_LocalizeMapname_SP = 0;
constexpr uintptr_t UI_LocalizeMapname_MP = 0x4B96D0;
constexpr uintptr_t UI_LocalizeGametype_MP = 0x4B90F0;

// String Functions
constexpr uintptr_t SL_ConvertToString_SP = 0x3D6870;
constexpr uintptr_t SL_ConvertToString_MP = 0x4317F0;

// Command Buffer Functions (from iw6x-client symbols.hpp)
// Used to intercept console commands: loadgame_continue, fast_restart, etc.
constexpr uintptr_t Cbuf_AddText_SP = 0x3B3050;   // void(int localClientNum, const char* text)
constexpr uintptr_t Cbuf_AddText_MP = 0x3F6B50;

// Rendering Functions
// NOTE: Offsets can vary across builds/patches. TextHook.cpp validates the
// prologue and falls back if needed.
constexpr uintptr_t R_AddCmdDrawText_SP = 0x533E40;
constexpr uintptr_t R_AddCmdDrawText_SP_FALLBACK = 0x402990;
constexpr uintptr_t R_AddCmdDrawText_MP = 0x601070;
constexpr uintptr_t R_AddCmdDrawTextWithCursor_SP = 0x534170;  // Text with cursor (input fields)
constexpr uintptr_t R_AddCmdDrawTextWithCursor_MP = 0x6013A0;
constexpr uintptr_t R_RegisterFont_SP = 0x5130B0;
constexpr uintptr_t R_RegisterFont_MP = 0x5DFAC0;
constexpr uintptr_t R_TextWidth_SP = 0x513390;
constexpr uintptr_t R_TextWidth_SP_FALLBACK = 0x403C40;
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
constexpr uintptr_t CG_GameMessage_SP = 0x1F2E20;
constexpr uintptr_t CG_GameMessage_MP = 0x271320;

// Database Functions
constexpr uintptr_t DB_FindXAssetHeader_SP        = 0x272300;
constexpr uintptr_t DB_FindXAssetHeader_MP        = 0x31F3A0;
constexpr uintptr_t DB_EnumXAssets_Internal_SP    = 0x271FC0; // SP; enumerate all assets of a type
constexpr uintptr_t DB_IsLocalized_SP = 0x273210;
constexpr uintptr_t DB_IsLocalized_MP = 0x320360;

// Dvar Functions
constexpr uintptr_t Dvar_FindVar_SP = 0x429E70;
constexpr uintptr_t Dvar_FindVar_MP = 0x4ECB60;
constexpr uintptr_t Dvar_GetBool_SP = 0x429FC0;
constexpr uintptr_t Dvar_GetInt_SP = 0x42A0A0;
constexpr uintptr_t Dvar_GetVariantStringWithDefault_SP = 0x42A240;

// Screen Placement (for proper positioning)
constexpr uintptr_t ScrPlace_GetViewPlacement_SP = 0x24D150;
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
constexpr uintptr_t HudElem_Array_SP = 0x147EDAC;
constexpr int HudElem_Array_Stride_SP = 0xA8;
// Configstring->stringId resolver used by objective/hint reverse path.
// Reversed from runtime callsites around +0x328E30/+0x3298B0:
//   lea ecx, [cfg + 0xAC] / call +0x4915A0 / mov ecx,eax / call SL_ConvertToString
constexpr uintptr_t ConfigString_IndexToSlc_SP = 0x4915A0;
// String id type-check path used immediately before objective SL_ConvertToString
// callsite (+0x34E25E). Returns string type in EAX (cmp eax,3 in caller).
constexpr uintptr_t SL_StringTypeCheck_SP = 0x3DE320;
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
    constexpr uintptr_t g_entities = 0x3C91600;  // 0x143C91600 - base
    
    // Zone data (for asset loading context)
    constexpr uintptr_t g_zones_0 = 0x34892D8;   // 0x1434892D8 - base
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

// Helper to get actual address
inline void *GetAddress(uintptr_t moduleBase, uintptr_t offset) {
  if (offset == 0)
    return nullptr;
  return reinterpret_cast<void *>(moduleBase + offset);
}

} // namespace IW6Offsets
