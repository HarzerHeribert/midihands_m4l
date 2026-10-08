#include "clutch.hpp"

#include <algorithm>
#include <cmath>

namespace mh {

bool engaged(int engage, const Gestures& g) {
  if (engage <= kEngageAlways || engage > kClutches) return true;
  const int i = engage - 1;
  return g[i / kGestures][i % kGestures];
}

void HandClutches::reset() {
  raw_.fill(false);
  state_.fill(false);
  span_ = -1.f;
}

const std::array<bool, kGestures>& HandClutches::apply(const HandFeatures& f, bool usable, double now) {
  if (!usable) {
    reset();
    return state_;
  }
  const double dt = span_ < 0.f ? 0.0 : std::max(0.0, now - last_);
  last_ = now;
  span_ = span_ < 0.f ? f.thumbSpan
                      : span_ + float(1.0 - std::exp(-dt / kSpanSmoothSeconds)) * (f.thumbSpan - span_);
  const float values[kGestures] = {span_, span_, f.fist, 1.f - f.curl[Pinky]};
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

void SlotSwitches::setGesture(int s, int gesture) {
  if (s < 0 || s >= kSlots) return;
  gesture_[s] = gesture;
  held_[s] = false;  // a new gesture starts let go
}

void SlotSwitches::setMode(int s, int mode) {
  if (s >= 0 && s < kSlots) mode_[s] = mode;
}

void SlotSwitches::setOn(int s, bool on) {
  if (s >= 0 && s < kSlots) on_[s] = on;
}

void SlotSwitches::update(const Gestures& g, std::vector<SlotSwitch>& out) {
  for (int s = 0; s < kSlots; ++s) {
    if (gesture_[s] <= kEngageAlways) continue;
    const bool held = engaged(gesture_[s], g);
    bool on = on_[s];
    if (mode_[s] == Toggle) {
      if (held && !held_[s]) on = !on;
    } else {
      on = held;
    }
    held_[s] = held;
    if (on != on_[s]) {
      on_[s] = on;
      out.push_back({s, on});
    }
  }
}

}  // namespace mh
