#include "camera_hub.hpp"

namespace mh {

struct CameraHub::Camera {
  CameraInfo info;
  Tracker tracker;
  HandAssigner assigner;  // only touched on the capture thread
  std::mutex dispatch;    // guards everything below; held while calling subscribers
  std::map<long, Callback> subscribers;
  std::map<long, bool> wantsPreview;
};

CameraHub& CameraHub::shared() {
  static CameraHub* hub = new CameraHub();  // never destroyed: outlives every instance
  return *hub;
}

long CameraHub::subscribe(const std::string& camera, Callback callback, std::string* error, CameraInfo* info) {
  std::string uid;
  CameraInfo found;
  for (const CameraInfo& c : listCameras()) {
    if (camera.empty() || camera == c.uid || camera == c.name) {
      uid = c.uid;
      found = c;
      break;
    }
  }
  if (uid.empty()) {
    if (error) *error = camera.empty() ? "no camera found" : "camera not found: " + camera;
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const long id = nextId_++;
  auto it = cameras_.find(uid);
  if (it == cameras_.end()) {
    auto cam = std::make_shared<Camera>();
    cam->info = found;
    Camera* raw = cam.get();
    const bool ok = cam->tracker.start(
        uid,
        [this, raw](const std::vector<Detection>& dets, double time, float aspect, const TrackerStats& stats,
                    const LumaView& luma) { onFrame(*raw, dets, time, aspect, stats, luma); },
        error);
    if (!ok) return 0;
    cam->info = cam->tracker.camera();
    it = cameras_.emplace(uid, std::move(cam)).first;
  }
  {
    std::lock_guard<std::mutex> d(it->second->dispatch);
    it->second->subscribers[id] = std::move(callback);
  }
  owners_[id] = uid;
  if (info) *info = it->second->info;
  return id;
}

void CameraHub::unsubscribe(long id) {
  std::shared_ptr<Camera> stopping;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto owner = owners_.find(id);
    if (owner == owners_.end()) return;
    auto it = cameras_.find(owner->second);
    owners_.erase(owner);
    if (it == cameras_.end()) return;
    bool empty;
    {
      // Waits for an in-flight frame, so the callback cannot run after this.
      std::lock_guard<std::mutex> d(it->second->dispatch);
      it->second->subscribers.erase(id);
      it->second->wantsPreview.erase(id);
      empty = it->second->subscribers.empty();
    }
    if (empty) {
      stopping = it->second;
      cameras_.erase(it);
    }
  }
  // Outside the locks: stop() waits for the capture thread, which may be
  // waiting for `dispatch`.
  if (stopping) stopping->tracker.stop();
}

void CameraHub::setPreview(long id, bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto owner = owners_.find(id);
  if (owner == owners_.end()) return;
  auto it = cameras_.find(owner->second);
  if (it == cameras_.end()) return;
  std::lock_guard<std::mutex> d(it->second->dispatch);
  it->second->wantsPreview[id] = enabled;
}

std::vector<std::pair<std::string, int>> CameraHub::activeCameras() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<std::string, int>> out;
  for (const auto& [uid, cam] : cameras_) {
    std::lock_guard<std::mutex> d(cam->dispatch);
    out.emplace_back(cam->info.name, int(cam->subscribers.size()));
  }
  return out;
}

void CameraHub::onFrame(Camera& cam, const std::vector<Detection>& dets, double time, float aspect,
                        const TrackerStats& stats, const LumaView& luma) {
  HubFrame hf;
  hf.frame = cam.assigner.assign(dets, time, aspect);
  hf.stats = stats;
  std::lock_guard<std::mutex> d(cam.dispatch);
  bool preview = false;
  for (const auto& [id, on] : cam.wantsPreview) preview |= on;
  if (preview) {
    const int w = kPreviewWidth;
    const int h = std::max(1, int(w / std::max(0.1f, aspect)));
    hf.preview = std::make_shared<GrayImage>(mirroredThumbnail(luma, w, h));
  }
  for (const auto& [id, callback] : cam.subscribers) callback(hf);
}

}  // namespace mh
