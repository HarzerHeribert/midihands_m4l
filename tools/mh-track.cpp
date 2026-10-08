// Runs the MediaPipe hand pipeline (track/) on still images and prints what it finds:
//   mh-track <onnxruntime library> <models folder> <image> [frames]
// Each image is fed `frames` times (default 3) to exercise crop tracking too. Used to check
// the Windows build on a CI runner and to compare with MediaPipe's Python reference.
#include <cstdio>
#include <cstdlib>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

#include "../track/hand_pipeline.hpp"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: mh-track <onnxruntime library> <models folder> <image> [frames]\n");
    return 2;
  }
  std::string error;
  auto pipeline = mh::HandPipeline::load(argv[1], argv[2], &error);
  if (!pipeline) {
    std::fprintf(stderr, "cannot load: %s\n", error.c_str());
    return 1;
  }
  int w = 0, h = 0, n = 0;
  unsigned char* rgba = stbi_load(argv[3], &w, &h, &n, 4);
  if (!rgba) {
    std::fprintf(stderr, "cannot read %s\n", argv[3]);
    return 1;
  }
  for (int i = 0; i < w * h; ++i) std::swap(rgba[i * 4], rgba[i * 4 + 2]);  // RGBA -> BGRA, like a camera frame
  const mh::BgraImage image{rgba, w, h, w * 4};
  const int frames = argc > 4 ? std::atoi(argv[4]) : 3;
  std::vector<mh::Detection> hands;
  for (int f = 0; f < frames; ++f) hands = pipeline->process(image);
  std::printf("{\"image\": [%d, %d], \"hands\": [", w, h);
  for (size_t k = 0; k < hands.size(); ++k) {
    const mh::Detection& d = hands[k];
    std::printf("%s\n  {\"side\": \"%s\", \"confidence\": %.3f, \"landmarks\": [", k ? "," : "",
                d.chirality == mh::Right ? "right" : d.chirality == mh::Left ? "left" : "?", d.confidence);
    for (int i = 0; i < mh::kLandmarks; ++i) std::printf("%s[%.4f, %.4f]", i ? ", " : "", d.lm[i].x, d.lm[i].y);
    std::printf("]}");
  }
  std::printf("]}\n");
  stbi_image_free(rgba);
  return 0;
}
