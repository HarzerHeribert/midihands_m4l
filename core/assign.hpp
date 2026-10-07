// Sorts raw detections into the performer's left and right hand.
#pragma once

#include <vector>

#include "hands.hpp"

namespace mh {

// The detector's left/right guess flickers, especially for a single hand,
// so it is combined with position and continuity:
//   - two hands: the one further left in the mirrored image is the left hand
//     (unless the detector confidently says they are crossed);
//   - one hand: stay on the side it was on last frame if the wrist is close,
//     otherwise trust the detector, otherwise use the image half.
class HandAssigner {
 public:
  Frame assign(const std::vector<Detection>& detections, double time, float aspect);
  void reset() { last_ = {}; }

 private:
  std::array<Hand, kSides> last_{};
};

}  // namespace mh
