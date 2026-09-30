#include "TextIntakePolicy.h"
#include "NativeHudContext.h"
#include <cassert>
#include <thread>
#include <cstdio>
int main() {
  using namespace TextIntakePolicy;
  assert(!ShouldInspectSlc("CREDITS_STAFF",true,true));
  assert(!ShouldInspectSlc("Synthetic Staff Member",true,false));
  assert(!ShouldInspectSlc("ui_play_credits",false,false));
  assert(ShouldInspectSlc("GAME_GET_TO_COVER",false,false));
  assert(ShouldInspectSlc("PLATFORM_PRESS_TO_USE",false,false));
  assert(ShouldInspectSlc("DEER_HUNT_LASER_HINT",true,false));
  assert(ShouldInspectSlc("DEER_HUNT_LASER_HINT",false,true));
  assert(ShouldInspectSlc("Press E to interact",true,false));
  assert(!ShouldInspectSlc("Press E to interact",false,false));
  assert(ShouldInspectSlc("Objectives Updated",false,true));
  assert(IsLocalizationKey("@INTROSCREEN_SKYWAY"));
  assert(!IsLocalizationKey("CREDITS"));
  assert(!IsLocalizationKey("a_path/file.ff"));
  assert(ShouldInspectNativeConfig("SKYWAY_OBJ_FINDBOSS"));
  assert(ShouldInspectNativeConfig("OILROCKS_FIND_RORKE"));
  assert(ShouldInspectNativeConfig("EXE_GAMESAVED"));
  assert(ShouldInspectNativeConfig("Find Rorke."));
  assert(ShouldInspectNativeConfig("Game Saved"));
  assert(ShouldInspectNativeConfig("\x1e" "SKYWAY_OBJ_FINDBOSS"));
  assert(ShouldInspectNativeConfig("\x1f" "Game Saved"));
  assert(ShouldInspectNativeConfig("\x1e" "EXE_GAMESAVED\x16" "1"));
  assert(ShouldInspectNativeConfig("\x1e" "CREDITS_STAFF\x1e" "EXE_GAMESAVED"));
  assert(!ShouldInspectNativeConfig(""));
  assert(!ShouldInspectNativeConfig("CREDITS_STAFF"));
  assert(!ShouldInspectNativeConfig("@CREDITS_STAFF"));
  assert(!ShouldInspectNativeConfig("\x1e" "CREDITS_STAFF"));
  assert(!ShouldInspectNativeConfig("CREDITS_STAFF\x1e" "CREDITS_DEVELOPER"));
  assert(!ShouldInspectNativeConfig("\x1e\x1f\x1e"));
  // This permissive rule applies only to the native config path. Arbitrary
  // raw SLC traffic still cannot turn staff names into gameplay events.
  assert(!ShouldInspectSlc("Synthetic Staff Member", true, false));
  assert(NativeHudContext::Get()==0);
  {
    NativeHudContext::Scope first(123);
    assert(NativeHudContext::Get()==123);
    try { NativeHudContext::Scope nested(456); throw 1; } catch(int) {}
    assert(NativeHudContext::Get()==123);
    std::thread thread([]{ assert(NativeHudContext::Get()==0); NativeHudContext::Scope own(789); assert(NativeHudContext::Get()==789); });
    thread.join();
    assert(NativeHudContext::Get()==123);
  }
  assert(NativeHudContext::Get()==0);
  std::puts("Text intake and nested/thread-local native HUD context: PASS");
}
