// midihands command-line tool: exercise the tracker and engine without Max.
//
//   mh cameras                         list cameras and the format midihands picks
//   mh replay <clip.mp4>... [--phases corpus.yaml] [--layout keys|chords|split]
//                                      run clips through tracking + engine
//   mh live [camera] [--seconds N] [--instances N]
//                                      run the live camera through the shared backend
//   mh snapshot <clip.mp4> <frame> <out.png>
//                                      render the editor's camera picture for one frame
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "../core/assign.hpp"
#include "../core/engine.hpp"
#include "../core/preview.hpp"
#include "../mac/camera_hub.hpp"
#include "../mac/tracker.hpp"

using namespace mh;

namespace {

struct Phase {
  std::string id;
  int start = 0, end = 0, hands = 0;
  std::vector<std::string> sides;
};

// Reads the "phases" entries of a recorder corpus.yaml (all scenes share them).
std::vector<Phase> readPhases(const std::string& path) {
  std::vector<Phase> phases;
  std::ifstream in(path);
  std::string line;
  const std::regex re(
      R"(phase_id:\s*([\w-]+),\s*start_frame:\s*(\d+),\s*end_frame:\s*(\d+),\s*expected_hand_count:\s*(\d+),\s*expected_handedness:\s*\[([^\]]*)\])");
  while (std::getline(in, line)) {
    std::smatch m;
    if (!std::regex_search(line, m, re)) continue;
    Phase p{m[1], std::stoi(m[2]), std::stoi(m[3]), std::stoi(m[4]), {}};
    std::string sides = m[5];
    if (sides.find("LEFT") != std::string::npos) p.sides.push_back("LEFT");
    if (sides.find("RIGHT") != std::string::npos) p.sides.push_back("RIGHT");
    phases.push_back(p);
    if (p.id == "empty-after") break;
  }
  return phases;
}

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(q * v.size()))];
}

int cmdCameras() {
  for (const CameraInfo& c : listCameras())
    std::printf("%-28s %4dx%-4d @ %5.1f fps  %s\n", c.name.c_str(), c.width, c.height, c.fps, c.uid.c_str());
  return 0;
}

int cmdReplay(const std::vector<std::string>& clips, const std::string& phasesPath, int layout) {
  const std::vector<Phase> phases = phasesPath.empty() ? std::vector<Phase>{} : readPhases(phasesPath);
  std::vector<double> detectMs;
  long frames = 0, countOk = 0, sideOk = 0, sideTotal = 0, noteOns = 0, noteOffs = 0;
  std::map<std::string, std::pair<long, long>> perPhase;  // id -> (count ok, frames)

  for (const std::string& clip : clips) {
    HandAssigner assigner;
    Engine engine;
    Params params;
    params.layout = layout;
    std::vector<MidiEvent> ignored;
    engine.setParams(params, ignored);
    long index = 0, clipOns = 0;
    std::string error;
    const bool ok = processMovie(
        clip,
        [&](const std::vector<Detection>& dets, double time, float aspect, double ms, const LumaView&) {
          detectMs.push_back(ms);
          const Frame frame = assigner.assign(dets, time, aspect);
          const Output out = engine.process(frame);
          for (const MidiEvent& e : out.midi) {
            if ((e.status & 0xF0) == 0x90) ++noteOns, ++clipOns;
            if ((e.status & 0xF0) == 0x80) ++noteOffs;
          }
          for (const Phase& p : phases) {
            if (index < p.start || index >= p.end) continue;
            const int count = int(frame.hands[Left].present) + int(frame.hands[Right].present);
            const bool good = count == p.hands;
            countOk += good;
            perPhase[p.id].first += good;
            perPhase[p.id].second += 1;
            if (good) {
              for (const std::string& side : p.sides) {
                ++sideTotal;
                sideOk += frame.hands[side == "LEFT" ? Left : Right].present;
              }
            }
          }
          ++index;
        },
        &error);
    if (!ok) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    frames += index;
    std::printf("%-40s frames=%ld note-ons=%ld\n", clip.substr(clip.find_last_of('/') + 1).c_str(), index, clipOns);
  }

  std::printf("\nframes %ld | vision mean %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f\n", frames,
              detectMs.empty() ? 0.0 : [&] { double s = 0; for (double v : detectMs) s += v; return s / detectMs.size(); }(),
              percentile(detectMs, 0.5), percentile(detectMs, 0.95), percentile(detectMs, 0.99));
  std::printf("notes: %ld on, %ld off\n", noteOns, noteOffs);
  if (!phases.empty()) {
    std::printf("exact hand count: %.1f%% | correct side when count is right: %.1f%%\n",
                100.0 * countOk / std::max(1L, frames), 100.0 * sideOk / std::max(1L, sideTotal));
    for (const Phase& p : phases)
      std::printf("  %-22s %5.1f%%\n", p.id.c_str(), 100.0 * perPhase[p.id].first / std::max(1L, perPhase[p.id].second));
  }
  return 0;
}

int cmdLive(const std::string& camera, double seconds, int instances) {
  if (cameraAccess() == CameraAccess::Undetermined) {
    std::atomic<int> answer{-1};
    requestCameraAccess([&](bool granted) { answer = granted; });
    while (answer < 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (cameraAccess() != CameraAccess::Granted) {
    std::fprintf(stderr, "camera access denied (System Settings > Privacy & Security > Camera)\n");
    return 1;
  }
  // Several engines on one shared camera, like several devices in a Live Set.
  struct Instance {
    Engine engine;
    std::atomic<long> frames{0};
    long id = 0;
  };
  std::vector<std::unique_ptr<Instance>> all;
  std::vector<double> detect, latency;
  std::mutex mutex;
  CameraInfo info;
  for (int i = 0; i < std::max(1, instances); ++i) {
    auto inst = std::make_unique<Instance>();
    Instance* raw = inst.get();
    const bool first = i == 0;
    std::string error;
    inst->id = CameraHub::shared().subscribe(
        camera,
        [&, raw, first](const HubFrame& hf) {
          raw->engine.process(hf.frame);
          if (++raw->frames % 30 == 0 && first)
            std::printf("fps %5.1f | vision %5.2f ms | frame->landmarks %6.2f ms | hands L%d R%d\n", hf.stats.fps,
                        hf.stats.detectMs, hf.stats.latencyMs, hf.frame.hands[Left].present,
                        hf.frame.hands[Right].present);
          if (first) {
            std::lock_guard<std::mutex> lock(mutex);
            detect.push_back(hf.stats.detectMs);
            latency.push_back(hf.stats.latencyMs);
          }
        },
        &error, &info);
    if (!inst->id) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    all.push_back(std::move(inst));
  }
  std::printf("camera: %s %dx%d @ %.0f fps, %d instance(s), %zu camera pipeline(s)\npicture: %s\n", info.name.c_str(),
              info.width, info.height, info.fps, instances, CameraHub::shared().activeCameras().size(),
              CameraHub::shared().pictureUrl(all[0]->id).c_str());
  std::fflush(stdout);
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  for (auto& inst : all) CameraHub::shared().unsubscribe(inst->id);
  std::lock_guard<std::mutex> lock(mutex);
  std::printf("\nframes per instance:");
  for (auto& inst : all) std::printf(" %ld", inst->frames.load());
  std::printf("\nvision p50 %.2f p95 %.2f ms | frame->landmarks p50 %.2f p95 %.2f ms | pipelines left running: %zu\n",
              percentile(detect, 0.5), percentile(detect, 0.95), percentile(latency, 0.5), percentile(latency, 0.95),
              CameraHub::shared().activeCameras().size());
  return 0;
}

int cmdSnapshot(const std::string& clip, long target, const std::string& outPath) {
  HandAssigner assigner;
  Engine engine;
  long index = 0;
  bool written = false;
  std::string error;
  processMovie(
      clip,
      [&](const std::vector<Detection>& dets, double time, float aspect, double, const LumaView& luma) {
        const Output out = engine.process(assigner.assign(dets, time, aspect));
        if (index++ != target || written) return;
        const int w = CameraHub::kPreviewWidth, h = int(w / aspect);
        const GrayImage gray = mirroredThumbnail(luma, w, h);
        std::vector<uint8_t> argb(size_t(w) * h * 4);
        renderPreview(gray, out.frame, out.fingerOn, argb.data(), w * 4);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(argb.data(), w, h, 8, w * 4, cs,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Big);
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:outPath.c_str()]];
        CGImageDestinationRef dst =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nil);
        CGImageDestinationAddImage(dst, img, nil);
        written = CGImageDestinationFinalize(dst);
        CFRelease(dst);
        CGImageRelease(img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
      },
      &error);
  std::printf(written ? "wrote %s\n" : "frame not found: %s\n", outPath.c_str());
  return written ? 0 : 1;
}

}  // namespace

int main(int argc, const char** argv) {
  @autoreleasepool {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
      std::fprintf(stderr, "usage: mh cameras | mh replay <clip>... [--phases corpus.yaml] [--layout keys|chords|split] | mh live [camera] [--seconds N] [--instances N] | mh snapshot <clip> <frame> <out.png>\n");
      return 2;
    }
    const std::string cmd = args[0];
    std::vector<std::string> positional;
    std::string phases, camera;
    double seconds = 10.0;
    int layout = Split, instances = 1;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--phases" && i + 1 < args.size()) phases = args[++i];
      else if (args[i] == "--seconds" && i + 1 < args.size()) seconds = std::stod(args[++i]);
      else if (args[i] == "--instances" && i + 1 < args.size()) instances = std::stoi(args[++i]);
      else if (args[i] == "--layout" && i + 1 < args.size()) {
        const std::string l = args[++i];
        layout = l == "keys" ? Keys : l == "chords" ? Chords : Split;
      } else positional.push_back(args[i]);
    }
    if (cmd == "cameras") return cmdCameras();
    if (cmd == "replay") return cmdReplay(positional, phases, layout);
    if (cmd == "live") return cmdLive(positional.empty() ? "" : positional[0], seconds, instances);
    if (cmd == "snapshot" && positional.size() == 3) return cmdSnapshot(positional[0], std::stol(positional[1]), positional[2]);
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 2;
  }
}
