// Scales, chords and note bookkeeping.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace mh {

struct Scale {
  int root = 0;  // pitch class, 0 = C
  int size = 7;
  std::array<int, 12> intervals = {0, 2, 4, 5, 7, 9, 11};  // major

  // Live 12 exposes the Set's scale as a list of intervals; this accepts
  // the same shape. Falls back to chromatic when the list is empty.
  void set(int rootNote, const std::vector<int>& steps);
  bool operator==(const Scale& o) const {
    if (root != o.root || size != o.size) return false;
    for (int i = 0; i < size; ++i)
      if (intervals[i] != o.intervals[i]) return false;
    return true;
  }
};

// Degree 0 is the root at middle C's octave (C4 = 60 when root is C).
// Negative and large degrees wrap into lower and higher octaves.
int degreeToNote(int degree, const Scale& scale, int octave);

// Stacked diatonic chord (root, third, fifth, ...) built on a degree.
std::vector<int> diatonicChord(int degree, const Scale& scale, int octave, int size = 3);

struct MidiEvent {
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
};

// Reference-counted note ownership. Several fingers can hold the same pitch;
// it only stops when the last owner lets go, and the note-off waits a short
// grace period so chord changes do not leave audible gaps.
class NoteBook {
 public:
  void claim(int owner, int note, int velocity, int channel, double now, std::vector<MidiEvent>& out);
  void release(int owner, int note, double now, double graceSec);
  void flush(double now, int channel, std::vector<MidiEvent>& out);
  void releaseAll(int channel, std::vector<MidiEvent>& out);
  bool sounding(int note) const { return owners_[note] != 0 || pendingOff_[note] >= 0.0; }

 private:
  std::array<uint32_t, 128> owners_{};
  std::array<double, 128> pendingOff_ = makePending();
  static std::array<double, 128> makePending() {
    std::array<double, 128> a{};
    a.fill(-1.0);
    return a;
  }
};

}  // namespace mh
