// Clutch gestures: hand poses that engage links and effect slots while held.
// Each link chooses its own: thumb stuck out or tucked in, a fist, or a
// straightened pinky, of either hand. So a pinch (which needs the thumb) can
// be engaged by the other hand's fist, and a fist movement by a thumb.
// Pinky means the pinky up on its own: an open hand, all fingers straight,
// is not a gesture.
#pragma once

#include <array>

#include "features.hpp"
#include "hands.hpp"

namespace mh {

enum Gesture { GestureThumbOut = 0, GestureThumbIn, GestureFist, GesturePinky, kGestures };
using Gestures = std::array<std::array<bool, kGestures>, kSides>;

// What engages a link or an effect slot: 0 = always, else 1 + side * kGestures
// + gesture (the device's menu order; Sets store the index).
constexpr int kEngageAlways = 0;
constexpr int kClutches = kSides * kGestures;
constexpr int clutchFor(Side side, Gesture g) { return 1 + side * kGestures + g; }
bool engaged(int engage, const Gestures& g);

// Hand features to debounced gestures. Each gesture switches on and off at
// two thresholds, so it does not flicker, and a change must hold for a few
// frames before it counts. Thumb span measured on recorded hands: tucked in
// ~0.1, relaxed open hands 0.33-0.84 (1st-99th percentile), widely spread
// ~0.97, thumbs-up ~1.1.
class HandClutches {
 public:
  static constexpr double kSettleSeconds = 0.06;
  struct Threshold { float on, off; };  // on > off: switches on above; on < off: below
  static constexpr Threshold kThresholds[kGestures] = {
      {1.0f, 0.88f},   // thumb out: thumb span
      {0.25f, 0.35f},  // thumb in: thumb span
      {0.75f, 0.55f},  // fist
      {0.35f, 0.2f},   // pinky: its extension above the other three fingers' mean
  };

  // usable: the hand is seen (or within the dropout hold). A lost hand lets go at once.
  const std::array<bool, kGestures>& apply(const HandFeatures& f, bool usable, double now);
  void reset();

 private:
  std::array<bool, kGestures> raw_{};    // with thresholds, not yet settled
  std::array<bool, kGestures> state_{};  // what counts
  std::array<double, kGestures> since_{};
};

}  // namespace mh
