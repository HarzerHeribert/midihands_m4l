// Windows: camera frames from Media Foundation, hands from the MediaPipe
// models (track/hand_pipeline) on ONNX Runtime. Same contract as
// mac/tracker.mm: callbacks on the capture thread, landmarks mirrored.
#include "../platform/tracker.hpp"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "../track/hand_pipeline.hpp"
#include "paths.hpp"

namespace mh {
namespace {

template <typename T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// COM and Media Foundation for the calling thread / the process.
struct MfScope {
  bool com = false;
  MfScope() {
    // The Max main thread may already be single-threaded COM; that is fine too.
    com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); });
  }
  ~MfScope() {
    if (com) CoUninitialize();
  }
};

double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string attributeString(IMFActivate* device, const GUID& key) {
  wchar_t* value = nullptr;
  UINT32 length = 0;
  std::string out;
  if (SUCCEEDED(device->GetAllocatedString(key, &value, &length)) && value) {
    out = narrow(std::wstring(value, length));
    CoTaskMemFree(value);
  }
  return out;
}

// All video capture devices; the caller releases each and frees the array.
std::vector<IMFActivate*> captureDevices() {
  std::vector<IMFActivate*> out;
  IMFAttributes* attributes = nullptr;
  if (FAILED(MFCreateAttributes(&attributes, 1))) return out;
  attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  if (SUCCEEDED(MFEnumDeviceSources(attributes, &devices, &count))) {
    for (UINT32 i = 0; i < count; ++i) out.push_back(devices[i]);
    CoTaskMemFree(devices);
  }
  release(attributes);
  return out;
}

CameraInfo describe(IMFActivate* device) {
  CameraInfo info;
  info.name = attributeString(device, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
  info.uid = attributeString(device, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
  if (info.uid.empty()) info.uid = info.name;
  return info;
}

// Highest frame rate first (it bounds latency), then the size closest to
// 1280x720, as on the Mac.
IMFMediaType* bestFormat(IMFSourceReader* reader, CameraInfo* info) {
  IMFMediaType* best = nullptr;
  double bestFps = 0.0, bestCost = 1e18;
  for (DWORD i = 0;; ++i) {
    IMFMediaType* type = nullptr;
    if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type))) break;
    UINT32 w = 0, h = 0, num = 0, den = 1;
    MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
    const double fps = den ? double(num) / den : 0.0;
    const double cost = std::fabs(double(w) * h - 1280.0 * 720.0);
    if (w && h && (fps > bestFps + 0.5 || (std::fabs(fps - bestFps) <= 0.5 && cost < bestCost))) {
      release(best);
      best = type;
      bestFps = fps;
      bestCost = cost;
      info->width = int(w);
      info->height = int(h);
      info->fps = fps;
    } else {
      release(type);
    }
  }
  return best;
}

}  // namespace

std::vector<CameraInfo> listCameras() {
  MfScope mf;
  std::vector<CameraInfo> out;
  for (IMFActivate* device : captureDevices()) {
    out.push_back(describe(device));
    device->Release();
  }
  return out;
}

// Windows has no per-app prompt for desktop apps: access is a system setting,
// and opening the camera fails with "access denied" when it is off.
CameraAccess cameraAccess() { return CameraAccess::Granted; }
void requestCameraAccess(std::function<void(bool)> done) {
  if (done) done(true);
}

struct Tracker::Impl {
  std::thread thread;
  std::atomic<bool> running{false};
  CameraInfo camera;
  mutable std::mutex mutex;
  std::unique_ptr<HandPipeline> pipeline;  // kept across restarts: loading the models takes a moment
};

Tracker::Tracker() : impl_(std::make_unique<Impl>()) {}
Tracker::~Tracker() { stop(); }

bool Tracker::start(const std::string& camera, DetectionCallback callback, std::string* error) {
  stop();
  if (!impl_->pipeline) {
    const std::string root = packageFolder();
    impl_->pipeline = HandPipeline::load(root + "\\support\\onnxruntime.dll", root + "\\models", error);
    if (!impl_->pipeline) return false;
  }
  // Open the camera on the calling thread, so errors come back from start().
  MfScope mf;
  IMFMediaSource* source = nullptr;
  CameraInfo info;
  for (IMFActivate* device : captureDevices()) {
    const CameraInfo d = describe(device);
    if (!source && (camera.empty() || camera == d.uid || camera == d.name)) {
      const HRESULT hr = device->ActivateObject(IID_PPV_ARGS(&source));
      if (FAILED(hr) && error) {
        *error = hr == E_ACCESSDENIED
                     ? "camera access is off: Windows Settings > Privacy & security > Camera > allow desktop apps"
                     : "cannot open " + d.name + ": " + errorText(hr);
      }
      info = d;
    }
    device->Release();
  }
  if (!source) {
    if (error && error->empty()) *error = camera.empty() ? "no camera found" : "camera not found: " + camera;
    return false;
  }
  IMFAttributes* attributes = nullptr;
  MFCreateAttributes(&attributes, 3);
  attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // any camera format -> RGB32
  attributes->SetUINT32(MF_LOW_LATENCY, TRUE);                           // never queue stale frames
  IMFSourceReader* reader = nullptr;
  HRESULT hr = MFCreateSourceReaderFromMediaSource(source, attributes, &reader);
  release(attributes);
  IMFMediaType* native = SUCCEEDED(hr) ? bestFormat(reader, &info) : nullptr;
  if (native) hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, native);
  release(native);
  IMFMediaType* rgb = nullptr;
  if (SUCCEEDED(hr)) hr = MFCreateMediaType(&rgb);
  if (SUCCEEDED(hr)) {
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(rgb, MF_MT_FRAME_SIZE, UINT32(info.width), UINT32(info.height));
    hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, rgb);
  }
  release(rgb);
  if (FAILED(hr) || !info.width) {
    if (error) *error = "cannot read frames from " + info.name + ": " + errorText(hr);
    release(reader);
    source->Shutdown();
    release(source);
    return false;
  }
  // Rows of a plain (not 2D) buffer: the stride the output format declares, else
  // Media Foundation's default for RGB32 (negative: bottom-up).
  LONG defaultStride = 0;
  IMFMediaType* current = nullptr;
  if (SUCCEEDED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current))) {
    UINT32 stride = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) defaultStride = LONG(INT32(stride));
    release(current);
  }
  if (defaultStride == 0 && FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, UINT32(info.width), &defaultStride)))
    defaultStride = -info.width * 4;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->camera = info;
  }
  impl_->running = true;
  HandPipeline* pipeline = impl_->pipeline.get();
  std::atomic<bool>* running = &impl_->running;
  impl_->thread = std::thread([reader, source, pipeline, running, callback = std::move(callback), info, defaultStride]() mutable {
    MfScope threadMf;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    double lastFrame = 0.0, fps = 0.0;
    while (running->load()) {
      DWORD stream = 0, flags = 0;
      LONGLONG stamp = 0;
      IMFSample* sample = nullptr;
      const HRESULT read = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &stamp, &sample);
      if (FAILED(read) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
        release(sample);
        break;
      }
      if (!sample) continue;
      const double frameTime = nowSeconds();
      IMFMediaBuffer* buffer = nullptr;
      IMF2DBuffer* buffer2d = nullptr;
      BYTE* scan0 = nullptr;
      LONG pitch = 0;
      bool locked = false;
      if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer))) {
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d)))) {
          locked = SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
        } else {
          BYTE* data = nullptr;
          DWORD length = 0;
          const DWORD rowBytes = DWORD(std::abs(defaultStride));
          if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            locked = length >= rowBytes * DWORD(info.height);
            if (!locked) buffer->Unlock();
            pitch = defaultStride;
            scan0 = pitch < 0 ? data + size_t(rowBytes) * (info.height - 1) : data;  // the top row
          }
        }
      }
      {
        if (locked) {
          const BgraImage image{scan0, info.width, info.height, int(pitch)};
          const double t0 = nowSeconds();
          std::vector<Detection> hands = pipeline->process(image);
          const double t1 = nowSeconds();
          if (lastFrame > 0.0 && frameTime > lastFrame) {
            const double instant = 1.0 / (frameTime - lastFrame);
            fps = fps > 0.0 ? fps + 0.1 * (instant - fps) : instant;
          }
          lastFrame = frameTime;
          TrackerStats stats;
          stats.fps = fps;
          stats.detectMs = (t1 - t0) * 1000.0;
          stats.latencyMs = (t1 - frameTime) * 1000.0;
          LumaView luma;
          luma.width = info.width;
          luma.height = info.height;
          luma.native = &image;  // the picture stream encodes it in color
          callback(hands, frameTime, float(info.width) / float(std::max(1, info.height)), stats, luma);
          if (buffer2d) buffer2d->Unlock2D();
          else buffer->Unlock();
        }
      }
      release(buffer2d);
      release(buffer);
      release(sample);
    }
    release(reader);
    source->Shutdown();
    release(source);
    running->store(false);  // also when the camera went away
  });
  return true;
}

void Tracker::stop() {
  impl_->running = false;
  if (impl_->thread.joinable()) impl_->thread.join();  // ReadSample returns within a frame
}

bool Tracker::running() const { return impl_->running.load(); }

CameraInfo Tracker::camera() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->camera;
}

}  // namespace mh
