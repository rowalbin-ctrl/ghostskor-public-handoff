#pragma once
#include <algorithm>
#include <cmath>

// Pixel coordinates from the engine's scissor command, after ScreenPlacement.
// Disabled means the whole render target; an enabled empty rect hides all text.
struct MenuClipRect {
  bool enabled = false;
  float left = 0, top = 0, right = 0, bottom = 0;
};

struct MenuQuad {
  float left, top, right, bottom;
  float u0, v0, u1, v1;
};

inline bool ClipMenuQuad(MenuQuad &q, const MenuClipRect &clip) {
  if (!(q.right > q.left && q.bottom > q.top)) return false;
  if (!clip.enabled) return true;
  const float l = (std::max)(q.left, clip.left);
  const float t = (std::max)(q.top, clip.top);
  const float r = (std::min)(q.right, clip.right);
  const float b = (std::min)(q.bottom, clip.bottom);
  if (!(r > l && b > t)) return false;
  const float du = (q.u1 - q.u0) / (q.right - q.left);
  const float dv = (q.v1 - q.v0) / (q.bottom - q.top);
  q.u1 = q.u0 + (r - q.left) * du;
  q.v1 = q.v0 + (b - q.top) * dv;
  q.u0 += (l - q.left) * du;
  q.v0 += (t - q.top) * dv;
  q.left = l; q.top = t; q.right = r; q.bottom = b;
  return true;
}
