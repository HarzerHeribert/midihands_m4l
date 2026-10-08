// Clutch gestures: hand poses that engage links and effect slots while held.
// Each link chooses its own: thumb stuck out or tucked in, a fist, or a
// straightened pinky, of either hand. So a pinch (which needs the thumb) can
// be engaged by the other hand's fist, and a fist movement by a thumb.
// Pinky means the pinky straight, whatever the other fingers do (an open hand
// counts too), so it suits poses where the pinky is otherwise bent.
#pragma once

#include <array>
#include <vector>

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
// ~0.1, relaxed open hands 0.33-0.84 (1st-99th percentile, median 0.6),
// widely spread ~0.97, thumbs-up ~1.1. Thumb out at 1.0 needed a full stretch
// (owner's hands, 2026-10-09), so it counts from 0.8. The span is smoothed a
// little first: thumb tips are the noisiest landmarks.
class HandClutches {
 public:
  static constexpr double kSettleSeconds = 0.06;
  static constexpr double kSpanSmoothSeconds = 0.05;
  struct Threshold { float on, off; };  // on > off: switches on above; on < off: below
  static constexpr Threshold kThresholds[kGestures] = {
      {0.8f, 0.7f},    // thumb out: thumb span
      {0.25f, 0.35f},  // thumb in: thumb span
      {0.75f, 0.55f},  // fist
      {0.56f, 0.44f},  // pinky: extension, the note gate's default
  };

  // usable: the hand is seen (or within the dropout hold). A lost hand lets go at once.
  const std::array<bool, kGestures>& apply(const HandFeatures& f, bool usable, double now);
  void reset();
  float span() const { return span_ < 0.f ? 0.f : span_; }  // smoothed thumb span, 0 without a hand

 private:
  std::array<bool, kGestures> raw_{};    // with thresholds, not yet settled
  std::array<bool, kGestures> state_{};  // what counts
  std::array<double, kGestures> since_{};
  float span_ = -1.f;
  double last_ = 0.0;
};

// Effect slots switched by a gesture: Hold = on while the gesture is held,
// Toggle = each new gesture flips the slot. The result goes to the slot's On
// parameter, so it shows, saves and can be recorded like any switch.
struct SlotSwitch {
  int slot;
  bool on;
};

class SlotSwitches {
 public:
  static constexpr int kSlots = 8;
  enum Mode { Hold = 0, Toggle = 1 };

  // The slot's settings as stored: gesture (0 = none, else clutchFor()), mode, and its On switch.
  void setGesture(int s, int gesture);
  void setMode(int s, int mode);
  void setOn(int s, bool on);
  void update(const Gestures& g, std::vector<SlotSwitch>& out);

 private:
  std::array<int, kSlots> gesture_{};
  std::array<int, kSlots> mode_{};
  std::array<bool, kSlots> on_{};
  std::array<bool, kSlots> held_{};
};

}  // namespace mh
