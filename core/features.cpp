#include "features.hpp"

#include <algorithm>
#include <cmath>

namespace mh {

namespace {

float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

// Distance in height-normalized units so x and y are on the same scale.
float dist(Point a, Point b, float aspect) {
  const float dx = (a.x - b.x) * aspect;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

float fingerCurl(Point mcp, Point pip, Point tip, float aspect) {
  // Angle term catches the actual bend at the PIP joint.
  const float ax = (pip.x - mcp.x) * aspect, ay = pip.y - mcp.y;
  const float bx = (tip.x - pip.x) * aspect, by = tip.y - pip.y;
  const float la = std::sqrt(ax * ax + ay * ay) + 1e-7f;
  const float lb = std::sqrt(bx * bx + by * by) + 1e-7f;
  const float cosAngle = (ax * bx + ay * by) / (la * lb);
  const float angleCurl = clamp01(1.f - (cosAngle + 1.f) * 0.5f);

  // Compactness term catches the fingertip moving back toward the palm.
  const float path = dist(mcp, pip, aspect) + dist(pip, tip, aspect) + 1e-7f;
  const float compactness = clamp01(1.f - dist(mcp, tip, aspect) / path);

  // Expand the mid-range so curl does not feel like a late switch.
  return clamp01(std::pow(angleCurl * 0.7f + compactness * 0.3f, 0.55f));
}

HandFeatures computeFeatures(const Hand& hand, Side side, float aspect) {
  HandFeatures f;
  if (!hand.present) return f;
  f.present = true;
  const auto& lm = hand.lm;

  for (int i = 0; i < kFingers; ++i) {
    f.curl[i] = fingerCurl(lm[kMcp[i]], lm[kPip[i]], lm[kTip[i]], aspect);
    // Thumb open/closed is too unstable to play notes with.
    f.upright[i] = i != Thumb && lm[kTip[i]].y < lm[kPip[i]].y - 0.02f;
  }

  // Wrist + middle MCP is a stable palm proxy.
  const Point wrist = lm[kWrist], palm = lm[9];
  f.x = clamp01((wrist.x + palm.x) * 0.5f);
  f.height = clamp01(1.f - (wrist.y + palm.y) * 0.5f);

  // Pinch is measured relative to palm length so it works at any distance
  // from the camera: ~0.2 palm lengths when touching, ~1 when open.
  const float palmLen = std::max(dist(wrist, palm, aspect), 1e-4f);
  const float pinchSpan = dist(lm[kThumbTip], lm[kTip[Index]], aspect) / palmLen;
  f.pinch = clamp01((1.f - pinchSpan) / 0.8f);

  // Nearest index or middle finger joint (and the ring finger's base, for a
  // thumb tucked across the palm).
  float near = 1e9f;
  for (int i : {5, 6, 7, 9, 10, 11, 13}) near = std::min(near, dist(lm[kThumbTip], lm[i], aspect));
  f.thumbSpan = near / palmLen;

  float sum = 0.f, lo = 1.f;
  for (int i = Index; i <= Pinky; ++i) {
    sum += f.curl[i];
    lo = std::min(lo, f.curl[i]);
  }
  f.fist = clamp01((0.65f * sum / 4.f + 0.35f * lo - 0.45f) / 0.4f);

  const Point a = lm[kMcp[Index]], b = lm[kMcp[Pinky]];
  float tilt = (std::atan2(b.y - a.y, (b.x - a.x) * aspect) + float(M_PI) * 0.5f) / float(M_PI);
  if (side == Left) tilt = 1.f - tilt;
  f.tilt = clamp01(tilt);
  return f;
}

}  // namespace mh
