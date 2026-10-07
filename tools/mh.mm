// midihands command-line tool: exercise the tracker and engine without Max.
//
//   mh cameras                         list cameras and the format midihands picks
//   mh replay <clip.mp4>... [--phases corpus.yaml] [--layout keys|chords|split]
//                                      run clips through tracking + engine
//   mh live [camera] [--seconds N]     run the live camera and report timing
#import <Foundation/Foundation.h>

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
        [&](const std::vector<Detection>& dets, double time, float aspect, double ms) {
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

int cmdLive(const std::string& camera, double seconds) {
  if (cameraAccess() == CameraAccess::Undetermined) {
    std::atomic<int> answer{-1};
    requestCameraAccess([&](bool granted) { answer = granted; });
    while (answer < 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (cameraAccess() != CameraAccess::Granted) {
    std::fprintf(stderr, "camera access denied (System Settings > Privacy & Security > Camera)\n");
    return 1;
  }
  HandAssigner assigner;
  Engine engine;
  std::vector<double> detect, latency;
  std::atomic<long> frames{0}, notes{0};
  std::mutex mutex;
  Tracker tracker;
  std::string error;
  const bool ok = tracker.start(
      camera,
      [&](const std::vector<Detection>& dets, double time, float aspect, const TrackerStats& stats) {
        const Output out = engine.process(assigner.assign(dets, time, aspect));
        for (const MidiEvent& e : out.midi) notes += (e.status & 0xF0) == 0x90;
        std::lock_guard<std::mutex> lock(mutex);
        detect.push_back(stats.detectMs);
        latency.push_back(stats.latencyMs);
        if (++frames % 30 == 0)
          std::printf("fps %5.1f | vision %5.2f ms | frame->landmarks %6.2f ms | hands L%d R%d | notes %ld\n", stats.fps,
                      stats.detectMs, stats.latencyMs, out.handPresent[Left], out.handPresent[Right], notes.load());
      },
      &error);
  if (!ok) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const CameraInfo info = tracker.camera();
  std::printf("camera: %s %dx%d @ %.0f fps\n", info.name.c_str(), info.width, info.height, info.fps);
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  tracker.stop();
  std::lock_guard<std::mutex> lock(mutex);
  std::printf("\n%ld frames | vision p50 %.2f p95 %.2f ms | frame->landmarks p50 %.2f p95 %.2f ms\n", frames.load(),
              percentile(detect, 0.5), percentile(detect, 0.95), percentile(latency, 0.5), percentile(latency, 0.95));
  return 0;
}

}  // namespace

int main(int argc, const char** argv) {
  @autoreleasepool {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
      std::fprintf(stderr, "usage: mh cameras | mh replay <clip>... [--phases corpus.yaml] [--layout keys|chords|split] | mh live [camera] [--seconds N]\n");
      return 2;
    }
    const std::string cmd = args[0];
    std::vector<std::string> positional;
    std::string phases, camera;
    double seconds = 10.0;
    int layout = Split;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--phases" && i + 1 < args.size()) phases = args[++i];
      else if (args[i] == "--seconds" && i + 1 < args.size()) seconds = std::stod(args[++i]);
      else if (args[i] == "--layout" && i + 1 < args.size()) {
        const std::string l = args[++i];
        layout = l == "keys" ? Keys : l == "chords" ? Chords : Split;
      } else positional.push_back(args[i]);
    }
    if (cmd == "cameras") return cmdCameras();
    if (cmd == "replay") return cmdReplay(positional, phases, layout);
    if (cmd == "live") return cmdLive(positional.empty() ? "" : positional[0], seconds);
    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 2;
  }
}
