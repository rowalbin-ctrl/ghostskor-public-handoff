#pragma once
#include <vector>
struct DrawCommand;

namespace NativeMenuDraw {
using Producer = void(__fastcall *)(const char*, int, void*, float, float,
                                    float, float, float, float*, int);
bool Initialize();
void BeginFrame();
bool IsCapturing();
void FlushNativeGeometry();
// Called under the renderer lock. The batch belongs to this exact AddCmd call.
bool Queue(DrawCommand &command);

class TextScope {
  friend bool Queue(DrawCommand &);
  TextScope *previous = nullptr;
  Producer producer;
  void *font;
  float x, y, sx, sy, rotation;
  int style;
  bool active = false;
  std::vector<DrawCommand> commands;
public:
  TextScope(Producer original, void *font, float x, float y, float sx, float sy,
            float rotation, int style, bool nativeLayout);
  ~TextScope();
};
}
