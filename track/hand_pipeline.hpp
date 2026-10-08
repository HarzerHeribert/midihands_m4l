// MediaPipe hand landmarks on ONNX Runtime: a palm detector finds hands, a
// landmark model reads 21 points from a rotated crop around each, and the
// crops follow the hands from frame to frame (the detector only runs while
// fewer than two hands are tracked). Portable C++: the Windows tracker uses
// it, and on a Mac `mh replay --tracker mediapipe` compares it with Vision.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/hands.hpp"
#include "../core/preview.hpp"

namespace mh {

class HandPipeline {
 public:
  // `runtime`: the ONNX Runtime library; `models`: folder with
  // hand_detector.onnx and hand_landmarks.onnx.
  static std::unique_ptr<HandPipeline> load(const std::string& runtime, const std::string& models, std::string* error);
  ~HandPipeline();

  // Up to two hands, landmarks in the mirrored (selfie) view like the Mac
  // tracker: x to the performer's right, y down, both 0..1.
  std::vector<Detection> process(const BgraImage& image);

  struct Impl;

 private:
  HandPipeline() = default;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mh
