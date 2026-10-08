// MediaPipe's hand landmarker, rebuilt on ONNX Runtime (see hand_pipeline.hpp).
// A port of MediaPipe Tasks' HandLandmarker graphs (hand_detector_graph.cc,
// hand_landmarks_detector_graph.cc, hand_landmarker_graph.cc and their
// calculators), checked against the official runtime: the detector's SSD
// anchors and decoding, weighted NMS, palm-to-crop (scale 2.6, shifted towards
// the fingers), corner-to-corner bilinear crops, and crop tracking from 12
// palm landmarks (scale 2.0).
//
// The models see the camera image as it is (MediaPipe's handedness refers to
// the unmirrored image); only the final landmarks are mirrored into the selfie
// view the rest of midihands uses, as on the Mac.
#include "hand_pipeline.hpp"

#include <algorithm>
#include <cmath>

#include "onnx.hpp"

namespace mh {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kDetectorSize = 192;
constexpr int kLandmarkSize = 224;
constexpr float kDetectScore = 0.5f;  // min_hand_detection_confidence
constexpr float kNmsOverlap = 0.3f;   // weighted NMS, IoU
constexpr float kPresence = 0.5f;     // min_hand_presence_confidence
constexpr float kNewHandOverlap = 0.5f;  // a detection overlapping a followed hand is that hand
constexpr int kMaxHands = 2;
// MediaPipe Tasks sets the palm's target angle with set_rotation_vector_target_angle(90),
// a field in radians: 90 rad, not 90 degrees. Kept for parity with the official runtime.
constexpr double kPalmTargetAngle = 90.0;

float sigmoid(float x) { return 1.f / (1.f + std::exp(-std::clamp(x, -100.f, 100.f))); }
double wrapAngle(double a) { return a - 2 * kPi * std::floor((a + kPi) / (2 * kPi)); }

struct Anchor {
  float x, y;
};

// SSD anchors of the full-range palm detector: 4 layers with strides 8, 16, 16, 16
// on 192 x 192, 2 anchors per layer and cell; layers with equal strides share a grid
// -> 24*24*2 + 12*12*6 = 2016, in y, x, anchor order.
std::vector<Anchor> palmAnchors() {
  std::vector<Anchor> anchors;
  for (const auto [stride, perCell] : {std::pair<int, int>{8, 2}, {16, 6}}) {
    const int n = kDetectorSize / stride;
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        for (int k = 0; k < perCell; ++k) anchors.push_back({(x + 0.5f) / n, (y + 0.5f) / n});
  }
  return anchors;
}

// A rotated rectangle in camera-image pixels (y down, rotation clockwise on screen).
struct Rect {
  double cx = 0, cy = 0, w = 0, h = 0, rotation = 0;
};

struct Palm {
  float score = 0;
  float box[4] = {};     // x0 y0 x1 y1, detector-normalized
  float kp[7][2] = {};   // 0 wrist centre, 2 middle-finger knuckle
};

float iou(const float* a, const float* b) {
  const float ix = std::max(0.f, std::min(a[2], b[2]) - std::max(a[0], b[0]));
  const float iy = std::max(0.f, std::min(a[3], b[3]) - std::max(a[1], b[1]));
  const float inter = ix * iy;
  const float uni = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter;
  return uni > 0 ? inter / uni : 0.f;
}

// RectTransformation with shift_x 0 and square_long.
Rect transform(double cx, double cy, double w, double h, double rotation, double scale, double shiftY) {
  Rect r;
  r.cx = cx - h * shiftY * std::sin(rotation);
  r.cy = cy + h * shiftY * std::cos(rotation);
  r.w = r.h = std::max(w, h) * scale;
  r.rotation = rotation;
  return r;
}

// The tracking crop for the next frame: from the 12 palm and lower-finger landmarks,
// upright from the wrist towards the middle of the knuckles.
Rect handRect(const double (&p)[kLandmarks][2]) {
  static const int kPartial[] = {0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};
  const double tx = ((p[5][0] + p[13][0]) / 2 + p[9][0]) / 2, ty = ((p[5][1] + p[13][1]) / 2 + p[9][1]) / 2;
  const double rotation = wrapAngle(kPi / 2 - std::atan2(-(ty - p[0][1]), tx - p[0][0]));
  double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
  for (int k : kPartial) {
    minX = std::min(minX, p[k][0]); maxX = std::max(maxX, p[k][0]);
    minY = std::min(minY, p[k][1]); maxY = std::max(maxY, p[k][1]);
  }
  const double ax = (minX + maxX) / 2, ay = (minY + maxY) / 2;  // un-rotate around the axis-aligned centre
  const double cr = std::cos(-rotation), sr = std::sin(-rotation);
  double qx0 = 1e18, qy0 = 1e18, qx1 = -1e18, qy1 = -1e18;
  for (int k : kPartial) {
    const double dx = p[k][0] - ax, dy = p[k][1] - ay;
    const double qx = dx * cr - dy * sr, qy = dx * sr + dy * cr;
    qx0 = std::min(qx0, qx); qx1 = std::max(qx1, qx); qy0 = std::min(qy0, qy); qy1 = std::max(qy1, qy);
  }
  const double qcx = (qx0 + qx1) / 2, qcy = (qy0 + qy1) / 2;
  const double cx = qcx * std::cos(rotation) - qcy * std::sin(rotation) + ax;
  const double cy = qcx * std::sin(rotation) + qcy * std::cos(rotation) + ay;
  return transform(cx, cy, qx1 - qx0, qy1 - qy0, rotation, 2.0, -0.1);
}

// Axis-aligned bounds of a crop (as MediaPipe compares new and followed hands).
void bounds(const Rect& r, float* b) {
  b[0] = float(r.cx - r.w / 2); b[1] = float(r.cy - r.h / 2); b[2] = float(r.cx + r.w / 2); b[3] = float(r.cy + r.h / 2);
}

}  // namespace

struct HandPipeline::Impl {
  std::unique_ptr<OnnxModel> detector, landmarks;
  std::vector<Anchor> anchors = palmAnchors();
  std::vector<float> input;
  std::vector<OnnxModel::Tensor> out;
  std::vector<Rect> tracked;  // hands followed from the previous frame
  int frame = 0;
  // Output positions by name (Identity: landmarks, _1: presence, _2: handedness).
  int lmPoints = 0, lmPresence = 1, lmHanded = 2;

  // ImageToTensor: the rect's corners map to the tensor's corners, sampled at integer
  // tensor pixels, bilinear; RGB 0..1, NHWC. Outside the image: black, or the nearest
  // edge pixel (`replicate`).
  void crop(const BgraImage& img, const Rect& r, int size, bool replicate) {
    input.resize(size_t(size) * size * 3);
    const double c = std::cos(r.rotation), s = std::sin(r.rotation), sx = r.w / size, sy = r.h / size;
    const double ox = r.cx - c * r.w / 2 + s * r.h / 2, oy = r.cy - s * r.w / 2 - c * r.h / 2;
    float* o = input.data();
    for (int ty = 0; ty < size; ++ty) {
      for (int tx = 0; tx < size; ++tx, o += 3) {
        double x = c * sx * tx - s * sy * ty + ox, y = s * sx * tx + c * sy * ty + oy;
        const int xi = int(std::floor(x)), yi = int(std::floor(y));
        const float fx = float(x - xi), fy = float(y - yi);
        float acc[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k) {
          int px = xi + (k & 1), py = yi + (k >> 1);
          const float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
          if (px < 0 || py < 0 || px >= img.width || py >= img.height) {
            if (!replicate) continue;
            px = std::clamp(px, 0, img.width - 1);
            py = std::clamp(py, 0, img.height - 1);
          }
          const uint8_t* q = img.data + int64_t(py) * img.stride + px * 4;
          acc[0] += wgt * q[2]; acc[1] += wgt * q[1]; acc[2] += wgt * q[0];  // BGRx -> RGB
        }
        o[0] = acc[0] / 255.f; o[1] = acc[1] / 255.f; o[2] = acc[2] / 255.f;
      }
    }
  }

  // Palms in the whole frame, letterboxed into the detector's square; best first.
  std::vector<Rect> detectPalms(const BgraImage& img) {
    const double side = std::max(img.width, img.height);
    crop(img, Rect{img.width / 2.0, img.height / 2.0, side, side, 0}, kDetectorSize, false);
    std::string error;
    if (!detector->run(input.data(), {1, kDetectorSize, kDetectorSize, 3}, &out, &error) || out.size() < 2) return {};
    const OnnxModel::Tensor* reg = &out[0];  // Identity: [1, 2016, 18]
    const OnnxModel::Tensor* score = &out[1];  // Identity_1: [1, 2016, 1] logits
    if (reg->data.size() < score->data.size()) std::swap(reg, score);
    const size_t n = std::min(anchors.size(), score->data.size());
    if (reg->data.size() < n * 18) return {};
    std::vector<Palm> palms;
    for (size_t i = 0; i < n; ++i) {
      const float p = sigmoid(score->data[i]);
      if (p < kDetectScore) continue;
      const float* r = &reg->data[i * 18];
      const float cx = r[0] / kDetectorSize + anchors[i].x, cy = r[1] / kDetectorSize + anchors[i].y;
      const float w = r[2] / kDetectorSize, h = r[3] / kDetectorSize;
      if (w < 0 || h < 0) continue;
      Palm palm;
      palm.score = p;
      palm.box[0] = cx - w / 2; palm.box[1] = cy - h / 2; palm.box[2] = cx + w / 2; palm.box[3] = cy + h / 2;
      for (int k = 0; k < 7; ++k) {
        palm.kp[k][0] = r[4 + k * 2] / kDetectorSize + anchors[i].x;
        palm.kp[k][1] = r[5 + k * 2] / kDetectorSize + anchors[i].y;
      }
      palms.push_back(palm);
    }
    // Weighted NMS: each pick averages everything overlapping it, weighted by score.
    std::stable_sort(palms.begin(), palms.end(), [](const Palm& a, const Palm& b) { return a.score > b.score; });
    std::vector<Rect> rects;
    std::vector<bool> used(palms.size(), false);
    const double x0 = img.width / 2.0 - side / 2, y0 = img.height / 2.0 - side / 2;
    for (size_t i = 0; i < palms.size() && rects.size() < size_t(kMaxHands); ++i) {
      if (used[i]) continue;
      Palm avg{};
      float total = 0;
      for (size_t j = i; j < palms.size(); ++j) {
        if (used[j] || iou(palms[i].box, palms[j].box) <= kNmsOverlap) continue;
        used[j] = true;
        const float wgt = palms[j].score;
        total += wgt;
        for (int k = 0; k < 4; ++k) avg.box[k] += palms[j].box[k] * wgt;
        for (int k = 0; k < 7; ++k) { avg.kp[k][0] += palms[j].kp[k][0] * wgt; avg.kp[k][1] += palms[j].kp[k][1] * wgt; }
      }
      used[i] = true;
      if (total <= 0) continue;
      // To image pixels, then the hand crop: upright from wrist to middle knuckle.
      double b[4], k0[2], k2[2];
      for (int k = 0; k < 4; ++k) b[k] = avg.box[k] / total * side + (k % 2 ? y0 : x0);
      for (int k = 0; k < 2; ++k) {
        k0[k] = avg.kp[0][k] / total * side + (k ? y0 : x0);
        k2[k] = avg.kp[2][k] / total * side + (k ? y0 : x0);
      }
      const double rotation = wrapAngle(kPalmTargetAngle - std::atan2(-(k2[1] - k0[1]), k2[0] - k0[0]));
      rects.push_back(transform((b[0] + b[2]) / 2, (b[1] + b[3]) / 2, b[2] - b[0], b[3] - b[1], rotation, 2.6, -0.5));
    }
    return rects;
  }

  // The landmark model on one crop. False when no hand is in it any more.
  bool readHand(const BgraImage& img, const Rect& r, Detection* d, Rect* next) {
    crop(img, r, kLandmarkSize, true);
    std::string error;
    if (!landmarks->run(input.data(), {1, kLandmarkSize, kLandmarkSize, 3}, &out, &error)) return false;
    if (out.size() < 3 || out[size_t(lmPoints)].data.size() < 63) return false;
    const float presence = out[size_t(lmPresence)].data[0];  // already a probability
    if (!(presence > kPresence)) return false;
    const float right = out[size_t(lmHanded)].data[0];  // P(anatomically right hand), unmirrored image
    const double c = std::cos(r.rotation), s = std::sin(r.rotation), sx = r.w / kLandmarkSize, sy = r.h / kLandmarkSize;
    const double ox = r.cx - c * r.w / 2 + s * r.h / 2, oy = r.cy - s * r.w / 2 - c * r.h / 2;
    double p[kLandmarks][2];
    const float* lm = out[size_t(lmPoints)].data.data();
    for (int k = 0; k < kLandmarks; ++k) {
      const double tx = lm[k * 3], ty = lm[k * 3 + 1];
      p[k][0] = c * sx * tx - s * sy * ty + ox;
      p[k][1] = s * sx * tx + c * sy * ty + oy;
      // Into the mirrored selfie view, normalized like Vision's points.
      d->lm[k] = {float(1.0 - (p[k][0] + 0.5) / img.width), float((p[k][1] + 0.5) / img.height)};
    }
    d->confidence = presence;
    d->chirality = right > 0.5f ? Right : Left;
    *next = handRect(p);
    return true;
  }
};

std::unique_ptr<HandPipeline> HandPipeline::load(const std::string& runtime, const std::string& models, std::string* error) {
  std::unique_ptr<HandPipeline> p(new HandPipeline());
  p->impl_ = std::make_unique<Impl>();
  const char sep = models.find('\\') != std::string::npos ? '\\' : '/';
  p->impl_->detector = OnnxModel::load(runtime, models + sep + "hand_detector.onnx", error);
  if (!p->impl_->detector) return nullptr;
  p->impl_->landmarks = OnnxModel::load(runtime, models + sep + "hand_landmarks.onnx", error);
  if (!p->impl_->landmarks) return nullptr;
  const auto& names = p->impl_->landmarks->outputNames();
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == "Identity") p->impl_->lmPoints = int(i);
    else if (names[i] == "Identity_1") p->impl_->lmPresence = int(i);
    else if (names[i] == "Identity_2") p->impl_->lmHanded = int(i);
  }
  return p;
}

HandPipeline::~HandPipeline() = default;

std::vector<Detection> HandPipeline::process(const BgraImage& image) {
  Impl& m = *impl_;
  ++m.frame;
  std::vector<Detection> hands;
  std::vector<Rect> next;
  for (const Rect& r : m.tracked) {
    Detection d;
    Rect follow;
    if (m.readHand(image, r, &d, &follow)) {
      hands.push_back(d);
      next.push_back(follow);
    }
  }
  // Look for (more) hands while fewer than two are followed: every frame with none,
  // every other frame with one (a detector pass costs about as much as a landmark pass).
  if (next.size() < size_t(kMaxHands) && (next.empty() || m.frame % 2 == 0)) {
    const std::vector<Rect> followed = next;
    for (const Rect& r : m.detectPalms(image)) {
      if (next.size() >= size_t(kMaxHands)) break;
      float a[4], b[4];
      bounds(r, a);
      bool known = false;
      for (const Rect& f : followed) {
        bounds(f, b);
        if (iou(a, b) > kNewHandOverlap) known = true;
      }
      if (known) continue;
      Detection d;
      Rect follow;
      if (m.readHand(image, r, &d, &follow)) {
        hands.push_back(d);
        next.push_back(follow);
      }
    }
  }
  m.tracked = next;
  return hands;
}

}  // namespace mh
