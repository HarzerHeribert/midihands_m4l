// Camera capture + hand pose: AVFoundation + Apple Vision on macOS
// (mac/tracker.mm), Media Foundation + MediaPipe hand models on Windows
// (win/tracker.cpp). Everything downstream is core/.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/hands.hpp"
#include "../core/preview.hpp"

namespace mh {

struct CameraInfo {
  std::string name;
  std::string uid;
  int width = 0;
  int height = 0;
  double fps = 0.0;  // best frame rate the tracker will use
};

std::vector<CameraInfo> listCameras();

enum class CameraAccess { Granted, Denied, Undetermined };
CameraAccess cameraAccess();
// Shows the system prompt if needed; the callback runs on an arbitrary thread.
void requestCameraAccess(std::function<void(bool granted)> done);

struct TrackerStats {
  double fps = 0.0;        // measured camera frame rate
  double detectMs = 0.0;   // Vision time for the last frame
  double latencyMs = 0.0;  // frame timestamp to landmarks ready
};

// `luma` points into the camera frame and is only valid during the call.
using DetectionCallback = std::function<void(const std::vector<Detection>& hands, double time, float aspect,
                                             const TrackerStats& stats, const LumaView& luma)>;

class Tracker {
 public:
  Tracker();
  ~Tracker();
  Tracker(const Tracker&) = delete;
  Tracker& operator=(const Tracker&) = delete;

  // camera: uid or name; empty picks the first camera. The callback runs on
  // the capture thread. Returns false and fills error on failure.
  bool start(const std::string& camera, DetectionCallback callback, std::string* error);
  // Synchronous: no callback runs after this returns. Never call it from
  // inside the callback.
  void stop();
  bool running() const;
  CameraInfo camera() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Runs detection on every frame of a movie file (tests and benchmarks).
// Frames are processed as fast as possible; time is the movie timestamp.
bool processMovie(
    const std::string& path,
    const std::function<void(const std::vector<Detection>&, double time, float aspect, double detectMs, const LumaView&)>& cb,
    std::string* error);

}  // namespace mh
