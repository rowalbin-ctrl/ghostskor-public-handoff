#include "MenuClip.h"
#include "MenuFrameBuffer.h"
#include <cassert>
#include <iostream>
#include <string>
bool near(float a,float b) { return std::fabs(a-b)<0.0001f; }
int main() {
  for (auto size : {std::pair<int,int>{1280,720}, {1920,1080}, {1600,1200},
                    {1280,1024}, {1920,1200}, {3440,1440}, {3840,2160}}) {
    const float w=(float)size.first,h=(float)size.second;
    // Native title above the old 18% band must remain visible at every aspect.
    MenuClipRect title{true,.08f*w,.04f*h,.6f*w,.12f*h};
    MenuQuad glyph{.10f*w,.06f*h,.12f*w,.09f*h,0,0,1,1};
    assert(ClipMenuQuad(glyph,title));
    MenuClipRect scroll{true,0,.15f*h,.48f*w,.85f*h};
    MenuQuad hidden{.1f*w,.10f*h,.15f*w,.14f*h,0,0,1,1};
    assert(!ClipMenuQuad(hidden,scroll));
    MenuQuad partial{.1f*w,.10f*h,.15f*w,.20f*h,0,0,1,1};
    assert(ClipMenuQuad(partial,scroll));
    assert(near(partial.top,.15f*h) && near(partial.v0,.5f));
    MenuQuad footer{.08f*w,.92f*h,.2f*w,.96f*h,0,0,1,1};
    assert(ClipMenuQuad(footer,{})); // reset must restore the footer immediately
  }
  MenuQuad q{0,0,100,100,.2f,.3f,.8f,.9f};
  assert(ClipMenuQuad(q,{true,25,25,75,75}));
  assert(near(q.u0,.35f)&&near(q.u1,.65f)&&near(q.v0,.45f)&&near(q.v1,.75f));
  assert(!ClipMenuQuad(q,{true,10,10,10,20})); // enabled zero width hides all
  MenuFrameBuffer<std::string> frames;
  frames.Begin(); frames.Add("title");
  assert(!frames.Available() && frames.Current().empty()); // Present mid-build
  frames.Add("resolution"); frames.End();
  assert(frames.Current().size()==2);
  frames.Begin(); frames.Add("advanced video");
  assert(frames.Current().size()==2 && frames.Current()[0]=="title");
  frames.End();
  assert(frames.Current().size()==1 && frames.Current()[0]=="advanced video");
  frames.Begin(); frames.End();
  assert(frames.Available() && frames.Current().empty()); // native menu hidden
  frames.Begin(); frames.Add("old resolution"); frames.Clear();
  assert(!frames.Available() && frames.Current().empty()); // graphics recreation
  std::cout<<"PASS: 7 aspect/resolution cases, partial UV clipping, reset, empty rectangles, complete-frame publication and empty-frame removal\n";
}
