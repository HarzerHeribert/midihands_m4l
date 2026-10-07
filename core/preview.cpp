#include "preview.hpp"

#include <algorithm>
#include <cmath>

namespace mh {

namespace {

struct Rgb {
  uint8_t r, g, b;
};

constexpr Rgb kBone = {200, 200, 200};
constexpr Rgb kLeft = {90, 200, 255};     // left hand tint
constexpr Rgb kRight = {255, 160, 40};    // right hand tint
constexpr Rgb kPlaying = {255, 230, 90};

constexpr int kBones[][2] = {{0, 1},   {1, 2},   {2, 3},   {3, 4},   {0, 5},   {5, 6},   {6, 7},
                             {7, 8},   {9, 10},  {10, 11}, {11, 12}, {13, 14}, {14, 15}, {15, 16},
                             {0, 17},  {17, 18}, {18, 19}, {19, 20}, {5, 9},   {9, 13},  {13, 17}};

class Canvas {
 public:
  Canvas(uint8_t* argb, int w, int h, int rowBytes) : p_(argb), w_(w), h_(h), row_(rowBytes) {}

  void dot(int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    uint8_t* px = p_ + y * row_ + x * 4;
    px[0] = 255;
    px[1] = c.r;
    px[2] = c.g;
    px[3] = c.b;
  }

  void disc(float cx, float cy, float r, Rgb c) {
    const int x0 = int(std::floor(cx - r)), x1 = int(std::ceil(cx + r));
    const int y0 = int(std::floor(cy - r)), y1 = int(std::ceil(cy + r));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) dot(x, y, c);
  }

  // Thick line drawn as overlapping discs; plenty fast at preview sizes.
  void line(float ax, float ay, float bx, float by, float width, Rgb c) {
    const float len = std::hypot(bx - ax, by - ay);
    const int steps = std::max(1, int(len / std::max(0.5f, width * 0.5f)));
    for (int i = 0; i <= steps; ++i) {
      const float t = float(i) / steps;
      disc(ax + (bx - ax) * t, ay + (by - ay) * t, width * 0.5f, c);
    }
  }

 private:
  uint8_t* p_;
  int w_, h_, row_;
};

int fingerOf(int landmark) { return landmark == 0 ? -1 : (landmark - 1) / 4; }

}  // namespace

GrayImage mirroredThumbnail(const LumaView& luma, int width, int height) {
  GrayImage out;
  if (!luma.data || luma.width <= 0 || luma.height <= 0 || width <= 0 || height <= 0) return out;
  out.width = width;
  out.height = height;
  out.pixels.resize(size_t(width) * height);
  const float sx = float(luma.width) / width, sy = float(luma.height) / height;
  for (int y = 0; y < height; ++y) {
    const int y0 = int(y * sy), y1 = std::max(y0 + 1, int((y + 1) * sy));
    for (int x = 0; x < width; ++x) {
      // Mirror: output column x reads from the opposite side of the source.
      const int srcX = width - 1 - x;
      const int x0 = int(srcX * sx), x1 = std::max(x0 + 1, int((srcX + 1) * sx));
      unsigned sum = 0, n = 0;
      for (int yy = y0; yy < y1 && yy < luma.height; yy += 2)
        for (int xx = x0; xx < x1 && xx < luma.width; xx += 2, ++n) sum += luma.data[yy * luma.stride + xx];
      out.pixels[size_t(y) * width + x] = uint8_t(n ? sum / n : 0);
    }
  }
  return out;
}

void renderPreview(const GrayImage& gray, const Frame& frame,
                   const std::array<std::array<bool, kFingers>, kSides>& fingerOn, uint8_t* argb,
                   int rowBytes) {
  const int w = gray.width, h = gray.height;
  for (int y = 0; y < h; ++y) {
    uint8_t* row = argb + y * rowBytes;
    for (int x = 0; x < w; ++x) {
      const uint8_t v = uint8_t(gray.pixels[size_t(y) * w + x] * 0.6f);  // dimmed so hands stand out
      row[x * 4 + 0] = 255;
      row[x * 4 + 1] = v;
      row[x * 4 + 2] = v;
      row[x * 4 + 3] = v;
    }
  }
  Canvas canvas(argb, w, h, rowBytes);
  const float thick = std::max(2.f, w / 220.f);
  for (int s = 0; s < kSides; ++s) {
    const Hand& hand = frame.hands[s];
    if (!hand.present) continue;
    const Rgb tint = s == Left ? kLeft : kRight;
    auto P = [&](int i) { return std::array<float, 2>{hand.lm[i].x * w, hand.lm[i].y * h}; };
    for (const auto& bone : kBones) {
      const int f = fingerOf(bone[1]);
      const bool playing = f > Thumb && fingerOn[s][f];
      const auto a = P(bone[0]), b = P(bone[1]);
      canvas.line(a[0], a[1], b[0], b[1], playing ? thick * 1.6f : thick, playing ? kPlaying : kBone);
    }
    for (int f = Thumb; f <= Pinky; ++f) {
      const auto tip = P(kTip[f]);
      const bool playing = f > Thumb && fingerOn[s][f];
      canvas.disc(tip[0], tip[1], playing ? thick * 3.f : thick * 1.8f, playing ? kPlaying : tint);
    }
    const auto wrist = P(kWrist);
    canvas.disc(wrist[0], wrist[1], thick * 2.2f, tint);
  }
}

}  // namespace mh
