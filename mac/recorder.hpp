// Records the video window: the picture inside it plus Live's sound, as an
// H.264/AAC MP4 in ~/Movies/MidiHands (ScreenCaptureKit + AVAssetWriter).
// macOS asks once whether Live may record the screen and its audio.
#pragma once

#include <functional>
#include <string>

namespace mh {

class Recorder {
 public:
  static Recorder& shared();

  // Main thread. Records the frontmost window whose title starts with
  // `title`, the rectangle x, y, w, h in points inside its content area
  // (y down, as the page measures it). `done(ok, message)` runs later on
  // another thread: ok with the file path, or not ok with the reason.
  void start(const std::string& title, double x, double y, double w, double h,
             std::function<void(bool, const std::string&)> started);
  void stop(std::function<void(bool, const std::string&)> finished);
  bool recording() const;
  std::string lastFile() const;
};

}  // namespace mh
