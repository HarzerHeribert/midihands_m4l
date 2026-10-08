// The instrument: landmarks in, MIDI and expression values out.
// Portable C++17 so the same engine can serve a future Windows tracker.
#pragma once

#include <array>
#include <vector>

#include "clutch.hpp"
#include "features.hpp"
#include "filters.hpp"
#include "hands.hpp"
#include "music.hpp"

namespace mh {

enum Layout { Keys = 0, Chords = 1, Split = 2, Custom = 3 };
enum FingerMode { FingerOff = 0, FingerNote = 1, FingerChord = 2 };

// What one finger plays. Degree counts scale steps from the root (0 = root,
// 7 = root an octave up in a 7-note scale); octave shifts on top of that.
struct FingerSlot {
  int mode = FingerNote;
  int degree = 0;
  int octave = 0;
  bool operator==(const FingerSlot& o) const { return mode == o.mode && degree == o.degree && octave == o.octave; }
  bool operator!=(const FingerSlot& o) const { return !(*this == o); }
};

// The eight playing fingers as keys, left to right in the mirrored view:
// 0-3 = left pinky, ring, middle, index; 4-7 = right index, middle, ring, pinky.
constexpr int kKeys = 8;
int keyPosition(Side side, int finger);
std::array<FingerSlot, kKeys> presetSlots(int layout);
enum VelocityMode { VelocityFixed = 0, VelocityHeight = 1, VelocitySpeed = 2 };
enum HandsUsed { BothHands = 0, LeftHandOnly = 1, RightHandOnly = 2 };

// Continuous controls, in this order, for mapping to Live parameters.
enum Expr { LHeight = 0, LX, LPinch, LFist, LTilt, RHeight, RX, RPinch, RFist, RTilt, kExpr };
extern const char* const kExprNames[kExpr];

struct Params {
  bool notes = true;            // play notes from fingers
  int hands = BothHands;        // which hands play notes on this instance
  int layout = Split;           // which preset filled `fingers`, or Custom
  std::array<FingerSlot, kKeys> fingers = presetSlots(Split);
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
  float landmarkCutoff = 1.7f;  // One Euro on landmarks: min cutoff Hz (<= 0: off)
  float landmarkBeta = 20.f;    //   and speed coefficient, per normalized unit/s
};

struct Output {
  std::vector<MidiEvent> midi;
  std::array<float, kExpr> expr{};
  std::array<bool, kSides> handPresent{};
  Gestures gestures{};  // clutch gestures held, per hand
  std::array<float, kSides> thumbSpan{};  // smoothed, what thumb in/out compare (0: no hand)
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

  // Notes a key (see keyPosition) plays with the current settings.
  std::vector<int> notesForKey(int key) const;

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

  int velocityFor(const HandFeatures& f, const FingerState& fs) const;
  void releaseFinger(int owner, FingerState& fs, double now);
  void resetFingers();

  Params p_;
  LandmarkFilter filter_;
  NoteBook book_;
  std::array<std::array<FingerState, kFingers>, kSides> fingers_{};
  std::array<double, kSides> lastSeen_{};
  std::array<HandFeatures, kSides> lastFeatures_{};
  std::array<HandClutches, kSides> clutch_{};
  std::array<Smoother, kExpr> smooth_{};
  std::array<float, kExpr> expr_{};
  std::array<int, kExpr> lastCc_{};
  double lastTime_ = -1.0;
};

}  // namespace mh
