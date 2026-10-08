#include "filters.hpp"

#include <algorithm>
#include <cmath>

namespace mh {

namespace {
float alpha(float cutoff, double dt) {
  const double tau = 1.0 / (2.0 * M_PI * std::max(cutoff, 1e-6f));
  return static_cast<float>(1.0 / (1.0 + tau / dt));
}
}  // namespace

float OneEuro::apply(float value, double time) {
  if (!primed_ || time <= time_) {
    primed_ = true;
    value_ = raw_ = value;
    deriv_ = 0.f;
    time_ = time;
    return value_;
  }
  const double dt = time - time_;
  const float d = static_cast<float>((value - raw_) / dt);
  deriv_ += alpha(dCutoff, dt) * (d - deriv_);
  const float cutoff = minCutoff + beta * std::fabs(deriv_);
  value_ += alpha(cutoff, dt) * (value - value_);
  raw_ = value;
  time_ = time;
  return value_;
}

void LandmarkFilter::configure(float minCutoff, float beta) {
  for (auto& side : f_)
    for (auto& lm : side)
      for (auto& axis : lm) {
        axis.minCutoff = minCutoff;
        axis.beta = beta;
      }
}

Frame LandmarkFilter::apply(const Frame& in) {
  Frame out = in;
  if (f_[0][0][0].minCutoff <= 0.f) return out;
  for (int s = 0; s < kSides; ++s) {
    if (!in.hands[s].present) {
      for (auto& lm : f_[s])
        for (auto& axis : lm) axis.reset();
      continue;
    }
    for (int i = 0; i < kLandmarks; ++i) {
      out.hands[s].lm[i].x = f_[s][i][0].apply(in.hands[s].lm[i].x, in.time);
      out.hands[s].lm[i].y = f_[s][i][1].apply(in.hands[s].lm[i].y, in.time);
    }
  }
  return out;
}

void LandmarkFilter::reset() {
  for (auto& side : f_)
    for (auto& lm : side)
      for (auto& axis : lm) axis.reset();
}

float Smoother::apply(float value, double dt) {
  if (!primed_ || timeConstantMs <= 0.f || dt <= 0.0) {
    primed_ = true;
    value_ = value;
    return value_;
  }
  const double tau = timeConstantMs / 1000.0;
  value_ += static_cast<float>(dt / (tau + dt)) * (value - value_);
  return value_;
}

}  // namespace mh
