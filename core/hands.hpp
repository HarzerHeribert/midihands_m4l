// Shared hand data types. Portable: no platform or Max dependencies.
#pragma once

#include <array>
#include <cstdint>

namespace mh {

// Landmark order follows MediaPipe so ported thresholds keep their meaning:
// 0 wrist, 1-4 thumb (CMC, MCP, IP, TIP), then MCP, PIP, DIP, TIP for
// index (5-8), middle (9-12), ring (13-16) and pinky (17-20).
constexpr int kLandmarks = 21;
constexpr int kWrist = 0;
constexpr int kThumbTip = 4;
constexpr std::array<int, 5> kMcp = {2, 5, 9, 13, 17};
constexpr std::array<int, 5> kPip = {3, 6, 10, 14, 18};
constexpr std::array<int, 5> kTip = {4, 8, 12, 16, 20};

enum Finger { Thumb = 0, Index, Middle, Ring, Pinky, kFingers };
enum Side { Left = 0, Right = 1, kSides };

// Normalized image coordinates in the mirrored (selfie) view:
// x grows to the performer's right, y grows downward, both in [0, 1].
struct Point {
  float x = 0.f;
  float y = 0.f;
};

struct Hand {
  bool present = false;
  float confidence = 0.f;
  std::array<Point, kLandmarks> lm{};
};

// One camera frame worth of tracking, already sorted into the performer's
// left and right hand.
struct Frame {
  double time = 0.0;    // seconds, monotonic
  float aspect = 1.f;   // image width / height, for undistorted distances
  std::array<Hand, kSides> hands{};
};

// Raw detector output before hands are assigned to sides.
struct Detection {
  float confidence = 0.f;
  int chirality = -1;  // Left, Right, or -1 when the detector cannot tell
  std::array<Point, kLandmarks> lm{};
};

}  // namespace mh
