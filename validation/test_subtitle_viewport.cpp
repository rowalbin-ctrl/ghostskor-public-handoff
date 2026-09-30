#include "SubtitleViewport.h"
#include <cassert>
#include <limits>
#include <cstdio>
using namespace SubtitleViewport;
static void near(float a, float b) { assert(std::fabs(a - b) < .01f); }
int main() {
  // Actual live snapshots from the same process before/after a window resize.
  NativeInputs input{{0,0,3840,2160},3840,2160,1.0f,1.777777791f};
  auto a = Select(3840,2160,&input);
  assert(a.nativeDialogue && a.nativeVideo);
  near(a.dialogue.height,2160); near(a.video.height,2160);
  input = {{0,0,1600,1200},1600,1200,1.333333254f,1.777777791f};
  a = Select(1600,1200,&input);
  assert(a.nativeDialogue && a.nativeVideo);
  near(a.dialogue.offsetY,0); near(a.dialogue.height,1200);
  near(a.video.offsetY,0); near(a.video.height,1200);
  // Same pixel resolution, DIFFERENT engine aspect setting; dialogue remains
  // full height while the actual cinematic renderer now letterboxes the image.
  input.pixelAspect = 1;
  a = Select(1600,1200,&input);
  near(a.dialogue.height,1200); near(a.video.height,900); near(a.video.offsetY,150);
  // Confirm no invented side bars on ultrawide. Native movie helper uses full W.
  input = {{0,0,3440,1440},3440,1440,1,1.777777791f};
  a = Select(3440,1440,&input);
  near(a.video.width,3440); near(a.video.offsetX,0); near(a.dialogue.height,1440);
  // Native offset viewport is distinct from the full-surface movie rectangle.
  input = {{100,50,1720,980},1920,1080,1,1.777777791f};
  a = Select(1920,1080,&input);
  near(a.dialogue.offsetX,100); near(a.dialogue.offsetY,50);
  near(a.video.width,1920); near(a.video.offsetY,0);
  // Transitions in BOTH directions must reject a stale placement, even if it fits.
  a = Select(3840,2160,&input);
  assert(!a.nativeDialogue && !a.nativeVideo); near(a.dialogue.width,3840);
  a = Select(1280,720,&input);
  assert(!a.nativeDialogue && !a.nativeVideo); near(a.video.height,720);
  input.viewport.width = std::numeric_limits<float>::quiet_NaN();
  input.pixelAspect = 0;
  a = Select(1920,1080,&input);
  assert(!a.nativeDialogue && !a.nativeVideo); near(a.dialogue.height,1080);
  a = Select(0,1080,nullptr); near(a.video.width,0);
  a = Select(1920,std::numeric_limits<float>::infinity(),nullptr);
  near(a.dialogue.width,0);
  // Preserve the established 16:9 layout widths exactly.
  near(WrapWidth(1920,1080,false),1190.4f);
  near(WrapWidth(1920,1080,true),1075.2f);
  const float sizes[][2] = {{1280,720},{1920,1080},{1600,1200},{1920,1200},
    {1280,1024},{2560,1080},{3440,1440},{3840,2160},{7680,4320},{1080,1920},{160,90}};
  for (auto &size : sizes) for (bool video : {false,true}) {
    const float wrap = WrapWidth(size[0],size[1],video);
    assert(std::isfinite(wrap) && wrap > 0 && wrap < size[0]);
  }
  puts("PASS subtitle viewports: live snapshots, aspect settings, resize, invalid inputs, 11 sizes");
}
