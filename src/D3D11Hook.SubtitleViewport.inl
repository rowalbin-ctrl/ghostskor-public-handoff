// Read-only access, with no new render hooks or changes to native state.
// Steam 24723416: ScrPlace_GetViewPlacement returns viewport position +0x18,
// size +0x20. R_CinematicRect computes its image rectangle from four RIP loads.
static bool ReadSubtitleViewportSEH(ScrPlace_GetViewPlacement_t placement,
    const unsigned char *cinematic, SubtitleViewport::NativeInputs *out) {
  __try {
    if (!placement || !cinematic) return false;
    // Verify each instruction before interpreting its relative operand. A moved
    // function can resolve by signature; a changed body must fail safely.
    if (memcmp(cinematic + 0x0B, "\x8B\x05", 2) ||
        memcmp(cinematic + 0x45, "\x8B\x05", 2) ||
        memcmp(cinematic + 0x7D, "\xF3\x0F\x59\x05", 4) ||
        memcmp(cinematic + 0x9E, "\xF3\x0F\x5E\x05", 4)) return false;
    const auto width = reinterpret_cast<const uint32_t*>(cinematic + 0x11 +
        *reinterpret_cast<const int32_t*>(cinematic + 0x0D));
    const auto height = reinterpret_cast<const uint32_t*>(cinematic + 0x4B +
        *reinterpret_cast<const int32_t*>(cinematic + 0x47));
    const auto pixelAspect = reinterpret_cast<const float*>(cinematic + 0x85 +
        *reinterpret_cast<const int32_t*>(cinematic + 0x81));
    const auto movieAspect = reinterpret_cast<const float*>(cinematic + 0xA6 +
        *reinterpret_cast<const int32_t*>(cinematic + 0xA2));
    const IW6ScreenPlacement *sp = placement();
    if (!sp) return false;
    out->canvasWidth = *width;
    out->canvasHeight = *height;
    out->pixelAspect = *pixelAspect;
    out->movieAspect = *movieAspect;
    memcpy(&out->viewport, sp->realViewportPosition, sizeof(out->viewport));
    const auto position = reinterpret_cast<const volatile float*>(sp->realViewportPosition);
    const auto size = reinterpret_cast<const volatile float*>(sp->realViewportSize);
    // Reject a resize in progress instead of publishing a mixed snapshot.
    return out->canvasWidth == *reinterpret_cast<const volatile uint32_t*>(width) &&
        out->canvasHeight == *reinterpret_cast<const volatile uint32_t*>(height) &&
        out->pixelAspect == *reinterpret_cast<const volatile float*>(pixelAspect) &&
        out->viewport.offsetX == position[0] && out->viewport.offsetY == position[1] &&
        out->viewport.width == size[0] && out->viewport.height == size[1];
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static SubtitleViewport::Areas ReadSubtitleAreas(float w, float h) {
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  static ScrPlace_GetViewPlacement_t placement = nullptr;
  static const unsigned char *cinematic = nullptr;
  if (!placement) placement = reinterpret_cast<ScrPlace_GetViewPlacement_t>(
      IW6Offsets::GetAddress(base, IW6Offsets::ScrPlace_GetViewPlacement_SP));
  if (!cinematic) cinematic = static_cast<const unsigned char*>(
      IW6Offsets::GetAddress(base, IW6Offsets::R_CinematicRect_SP));
  SubtitleViewport::NativeInputs input;
  const bool valid = ReadSubtitleViewportSEH(placement, cinematic, &input);
  return SubtitleViewport::Select(w, h, valid ? &input : nullptr);
}
