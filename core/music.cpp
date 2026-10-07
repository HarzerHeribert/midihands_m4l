#include "music.hpp"

#include <algorithm>

namespace mh {

namespace {
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

void Scale::set(int rootNote, const std::vector<int>& steps) {
  root = ((rootNote % 12) + 12) % 12;
  size = 0;
  for (int step : steps) {
    if (size < 12 && step >= 0 && step < 12) intervals[size++] = step;
  }
  if (size == 0) {
    for (int i = 0; i < 12; ++i) intervals[i] = i;
    size = 12;
  }
}

int degreeToNote(int degree, const Scale& scale, int octave) {
  const int oct = floorDiv(degree, scale.size);
  const int idx = degree - oct * scale.size;
  const int note = 60 + scale.root + scale.intervals[idx] + 12 * (oct + octave);
  return std::clamp(note, 0, 127);
}

std::vector<int> diatonicChord(int degree, const Scale& scale, int octave, int size) {
  std::vector<int> notes;
  for (int i = 0; i < size; ++i) {
    const int n = degreeToNote(degree + 2 * i, scale, octave);
    if (std::find(notes.begin(), notes.end(), n) == notes.end()) notes.push_back(n);
  }
  return notes;
}

void NoteBook::claim(int owner, int note, int velocity, int channel, double, std::vector<MidiEvent>& out) {
  if (note < 0 || note > 127) return;
  const uint8_t ch = static_cast<uint8_t>(channel & 15);
  if (sounding(note)) {
    // Re-strike so the new finger is heard, without stacking note-ons.
    out.push_back({static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(note), 0});
  }
  pendingOff_[note] = -1.0;
  owners_[note] |= 1u << owner;
  out.push_back({static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(note),
                 static_cast<uint8_t>(std::clamp(velocity, 1, 127))});
}

void NoteBook::release(int owner, int note, double now, double graceSec) {
  if (note < 0 || note > 127 || !(owners_[note] & (1u << owner))) return;
  owners_[note] &= ~(1u << owner);
  if (owners_[note] == 0) pendingOff_[note] = now + graceSec;
}

void NoteBook::flush(double now, int channel, std::vector<MidiEvent>& out) {
  for (int n = 0; n < 128; ++n) {
    if (pendingOff_[n] >= 0.0 && now >= pendingOff_[n]) {
      pendingOff_[n] = -1.0;
      out.push_back({static_cast<uint8_t>(0x80 | (channel & 15)), static_cast<uint8_t>(n), 0});
    }
  }
}

void NoteBook::releaseAll(int channel, std::vector<MidiEvent>& out) {
  for (int n = 0; n < 128; ++n) {
    if (sounding(n)) out.push_back({static_cast<uint8_t>(0x80 | (channel & 15)), static_cast<uint8_t>(n), 0});
    owners_[n] = 0;
    pendingOff_[n] = -1.0;
  }
}

}  // namespace mh
