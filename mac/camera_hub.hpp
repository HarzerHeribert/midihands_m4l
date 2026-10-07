// The shared tracking backend. Every midihands instance in the process
// subscribes here; each camera that at least one instance uses runs exactly
// one capture + detection pipeline, and its frames go to all subscribers.
// A camera stops when its last subscriber leaves.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/assign.hpp"
#include "../core/preview.hpp"
#include "tracker.hpp"

namespace mh {

struct HubFrame {
  Frame frame;  // hands sorted into left/right, unfiltered
  TrackerStats stats;
};

class CameraHub {
 public:
  using Callback = std::function<void(const HubFrame&)>;
  static constexpr int kPreviewWidth = 640;  // camera picture streamed to editors

  static CameraHub& shared();

  // camera: uid or name, empty for the first camera. Returns 0 on failure.
  // The callback runs on that camera's capture thread.
  long subscribe(const std::string& camera, Callback callback, std::string* error, CameraInfo* info);
  // After this returns the subscriber's callback is not running and never
  // will again. Stops the camera if nobody else uses it.
  void unsubscribe(long id);
  // Address of the subscriber's camera picture (MJPEG over loopback), or "".
  std::string pictureUrl(long id) const;
  // Number of cameras currently running, and instances on each (for status).
  std::vector<std::pair<std::string, int>> activeCameras() const;

 private:
  struct Camera;
  void onFrame(Camera& cam, const std::vector<Detection>& dets, double time, float aspect,
               const TrackerStats& stats, const LumaView& luma);

  mutable std::mutex mutex_;  // guards cameras_ and owners_
  std::map<std::string, std::shared_ptr<Camera>> cameras_;  // by camera uid
  std::map<long, std::string> owners_;                       // subscriber -> camera uid
  long nextId_ = 1;
};

}  // namespace mh
