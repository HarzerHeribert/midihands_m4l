#include "clutch.hpp"

namespace mh {

bool engaged(int engage, const Gestures& g) {
  if (engage <= kEngageAlways || engage > kClutches) return true;
  const int i = engage - 1;
  return g[i / kGestures][i % kGestures];
}

void HandClutches::reset() {
  raw_.fill(false);
  state_.fill(false);
}

const std::array<bool, kGestures>& HandClutches::apply(const HandFeatures& f, bool usable, double now) {
  if (!usable) {
    reset();
    return state_;
  }
  const float others = (f.curl[Index] + f.curl[Middle] + f.curl[Ring]) / 3.f;
  const float values[kGestures] = {f.thumbSpan, f.thumbSpan, f.fist, others - f.curl[Pinky]};
  for (int g = 0; g < kGestures; ++g) {
    const Threshold t = kThresholds[g];
    const bool above = t.on > t.off;
    const float v = values[g];
    bool raw = raw_[g];
    if (!raw && (above ? v > t.on : v < t.on)) raw = true;
    else if (raw && (above ? v < t.off : v > t.off)) raw = false;
    if (raw != raw_[g]) {
      raw_[g] = raw;
      since_[g] = now;
    }
    if (raw_[g] != state_[g] && now - since_[g] >= kSettleSeconds - 1e-9) state_[g] = raw_[g];
  }
  return state_;
}

}  // namespace mh
