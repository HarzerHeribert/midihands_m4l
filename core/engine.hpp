// The instrument: landmarks in, MIDI and expression values out.
// Portable C++17 so the same engine can serve a future Windows tracker.
#pragma once

#include <array>
#include <vector>

#include "features.hpp"
#include "filters.hpp"
#include "hands.hpp"
#include "music.hpp"

namespace mh {

enum Layout { Keys = 0, Chords = 1, Split = 2 };
enum VelocityMode { VelocityFixed = 0, VelocityHeight = 1, VelocitySpeed = 2 };
enum HandsUsed { BothHands = 0, LeftHandOnly = 1, RightHandOnly = 2 };

// Continuous controls, in this order, for mapping to Live parameters.
enum Expr { LHeight = 0, LX, LPinch, LFist, LTilt, RHeight, RX, RPinch, RFist, RTilt, kExpr };
extern const char* const kExprNames[kExpr];

struct Params {
  bool notes = true;            // play notes from fingers
  int hands = BothHands;        // which hands play notes on this instance
  int layout = Split;
  Scale scale;
  int octave = 0;
  int velocityMode = VelocityFixed;
  int velocity = 100;
  float sensitivity = 0.5f;     // 0..1, higher = fingers trigger with less extension
  float minNoteMs = 180.f;      // shortest note, hides one-frame flicker
  float holdMs = 300.f;         // keep notes through brief tracking dropouts
  float graceMs = 40.f;         // note-off delay for gapless chord changes
  int channel = 0;              // 0..15
  bool ccOut = false;           // also send expressions as CC ccBase..ccBase+9
  int ccBase = 20;
  float smoothingMs = 50.f;     // expression smoothing
};

struct Output {
  std::vector<MidiEvent> midi;
  std::array<float, kExpr> expr{};
  std::array<bool, kSides> handPresent{};
  std::array<std::array<bool, kFingers>, kSides> fingerOn{};
  Frame frame;  // filtered landmarks, for display
};

class Engine {
 public:
  Engine();
  // Applies new settings. Changes that alter pitches release sounding notes
  // first so nothing hangs.
  void setParams(const Params& p, std::vector<MidiEvent>& out);
  const Params& params() const { return p_; }

  Output process(const Frame& raw);
  void panic(std::vector<MidiEvent>& out);

  // Finger extension (1 = straight) that the note gates compare against.
  static float extension(const HandFeatures& f, int finger) { return 1.f - f.curl[finger]; }

 private:
  struct FingerState {
    Hysteresis gate;
    bool on = false;
    bool pendingOff = false;
    double onAt = 0.0;
    float lastExt = 0.f;
    float speed = 0.f;
    std::vector<int> notes;
  };

  std::vector<int> notesFor(Side side, int finger) const;
  int velocityFor(const HandFeatures& f, const FingerState& fs) const;
  void releaseFinger(int owner, FingerState& fs, double now);
  void resetFingers();

  Params p_;
  LandmarkFilter filter_;
  NoteBook book_;
  std::array<std::array<FingerState, kFingers>, kSides> fingers_{};
  std::array<double, kSides> lastSeen_{};
  std::array<HandFeatures, kSides> lastFeatures_{};
  std::array<Smoother, kExpr> smooth_{};
  std::array<float, kExpr> expr_{};
  std::array<int, kExpr> lastCc_{};
  double lastTime_ = -1.0;
};

}  // namespace mh
