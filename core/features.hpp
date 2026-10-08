// Per-hand features derived from landmarks. Ported from the original
// midihands finger_features.py so the thresholds keep their meaning.
#pragma once

#include <array>

#include "hands.hpp"

namespace mh {

struct HandFeatures {
  bool present = false;
  // 0 = straight, 1 = fully bent. Index 0 is the thumb.
  std::array<float, kFingers> curl{};
  // Fingertip clearly above its PIP joint (the original "is_extended").
  std::array<bool, kFingers> upright{};
  float x = 0.f;       // palm position, 0 = performer's far left
  float height = 0.f;  // palm height, 0 = bottom of the image
  float pinch = 0.f;   // thumb-index closeness, 1 = touching
  float fist = 0.f;    // 1 = closed fist
  float tilt = 0.f;    // hand rotation, 0.5 = level
  // Thumb tip's distance from the index and middle finger, in palm lengths:
  // ~0.1 tucked in, ~0.6 relaxed, over 1 stuck out (see HandClutches).
  float thumbSpan = 0.f;
};

float fingerCurl(Point mcp, Point pip, Point tip, float aspect);
HandFeatures computeFeatures(const Hand& hand, Side side, float aspect);

}  // namespace mh
