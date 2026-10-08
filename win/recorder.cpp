// Windows: recording is not there yet (macOS records with ScreenCaptureKit).
#include "../platform/recorder.hpp"

namespace mh {

Recorder& Recorder::shared() {
  static Recorder* r = new Recorder();
  return *r;
}

void Recorder::start(const std::string&, double, double, double, double, std::function<void(bool, const std::string&)> started) {
  started(false, "recording is not available on Windows yet");
}

void Recorder::stop(std::function<void(bool, const std::string&)> finished) { finished(false, "not recording"); }
bool Recorder::recording() const { return false; }
std::string Recorder::lastFile() const { return ""; }

}  // namespace mh
