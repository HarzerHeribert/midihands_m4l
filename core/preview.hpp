// Camera picture for the editor: a small mirrored grayscale frame with the
// tracked hands drawn on top. Portable so every tracker backend can use it.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "hands.hpp"

namespace mh {

// 8-bit luma plane of a camera frame, valid only during the frame callback.
struct LumaView {
  const uint8_t* data = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;  // bytes per row
};

struct GrayImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;  // width * height
};

// Box-filtered downscale to width x height, flipped horizontally so it
// matches the mirrored landmark coordinates.
GrayImage mirroredThumbnail(const LumaView& luma, int width, int height);

// Renders `gray` dimmed into an ARGB buffer (rowBytes per row) and draws
// both hands: bones, fingertips, and fingers that are playing highlighted.
void renderPreview(const GrayImage& gray, const Frame& frame,
                   const std::array<std::array<bool, kFingers>, kSides>& fingerOn,
                   uint8_t* argb, int rowBytes);

}  // namespace mh
