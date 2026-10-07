// Temporal filters for landmarks and control values.
#pragma once

#include <array>

#include "hands.hpp"

namespace mh {

// One Euro filter (Casiez et al.). Defaults are the values validated in the
// 2026 pipeline study (min_cutoff 1.7, beta 0.3, d_cutoff 1.0).
class OneEuro {
 public:
  void reset() { primed_ = false; }
  float apply(float value, double time);

  float minCutoff = 1.7f;
  float beta = 0.3f;
  float dCutoff = 1.0f;

 private:
  bool primed_ = false;
  float value_ = 0.f;
  float raw_ = 0.f;
  float deriv_ = 0.f;
  double time_ = 0.0;
};

// Filters every landmark of both hands; resets a side when its hand is lost.
class LandmarkFilter {
 public:
  Frame apply(const Frame& in);
  void reset();

 private:
  std::array<std::array<std::array<OneEuro, 2>, kLandmarks>, kSides> f_{};
};

// Exponential smoothing with a time constant, so behavior does not depend on
// the camera frame rate.
class Smoother {
 public:
  float apply(float value, double dt);
  void reset() { primed_ = false; }
  float value() const { return value_; }

  float timeConstantMs = 50.f;

 private:
  bool primed_ = false;
  float value_ = 0.f;
};

// Two-threshold switch: turns on above threshold + width/2 and off below
// threshold - width/2.
class Hysteresis {
 public:
  bool apply(float value, float threshold, float width) {
    if (!on_ && value >= threshold + width * 0.5f) on_ = true;
    else if (on_ && value <= threshold - width * 0.5f) on_ = false;
    return on_;
  }
  void reset() { on_ = false; }
  bool on() const { return on_; }

 private:
  bool on_ = false;
};

}  // namespace mh
