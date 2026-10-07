#include "assign.hpp"

#include <cmath>

namespace mh {

namespace {

constexpr float kContinuityRadius = 0.12f;

float wristDistance(const std::array<Point, kLandmarks>& a, const std::array<Point, kLandmarks>& b) {
  return std::hypot(a[kWrist].x - b[kWrist].x, a[kWrist].y - b[kWrist].y);
}

float palmX(const std::array<Point, kLandmarks>& lm) { return (lm[kWrist].x + lm[9].x) * 0.5f; }

void put(Frame& f, Side side, const Detection& d) {
  f.hands[side].present = true;
  f.hands[side].confidence = d.confidence;
  f.hands[side].lm = d.lm;
}

}  // namespace

Frame HandAssigner::assign(const std::vector<Detection>& detections, double time, float aspect) {
  Frame f;
  f.time = time;
  f.aspect = aspect;

  if (detections.size() >= 2) {
    const Detection& a = detections[0];
    const Detection& b = detections[1];
    const bool aIsLeft = palmX(a.lm) <= palmX(b.lm);
    const Detection& byPosLeft = aIsLeft ? a : b;
    const Detection& byPosRight = aIsLeft ? b : a;
    // Crossed hands: only believe it when the detector agrees on both.
    const bool crossed = byPosLeft.chirality == Right && byPosRight.chirality == Left;
    put(f, crossed ? Right : Left, byPosLeft);
    put(f, crossed ? Left : Right, byPosRight);
  } else if (detections.size() == 1) {
    const Detection& d = detections[0];
    int side = -1;
    for (int s = 0; s < kSides && side < 0; ++s) {
      if (last_[s].present && wristDistance(last_[s].lm, d.lm) < kContinuityRadius) side = s;
    }
    if (side < 0 && d.chirality >= 0) side = d.chirality;
    if (side < 0) side = palmX(d.lm) < 0.5f ? Left : Right;
    put(f, static_cast<Side>(side), d);
  }

  last_ = f.hands;
  return f;
}

}  // namespace mh
