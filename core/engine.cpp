#include "engine.hpp"

#include <algorithm>
#include <cmath>

namespace mh {

const char* const kExprNames[kExpr] = {"L Height", "L X", "L Pinch", "L Fist", "L Tilt",
                                       "R Height", "R X", "R Pinch", "R Fist", "R Tilt"};

namespace {

constexpr float kGateWidth = 0.12f;

}  // namespace

int keyPosition(Side side, int finger) {
  return side == Left ? Pinky - finger : 4 + (finger - Index);
}

std::array<FingerSlot, kKeys> presetSlots(int layout) {
  std::array<FingerSlot, kKeys> slots{};
  for (int k = 0; k < kKeys; ++k) {
    switch (layout) {
      case Keys:
        slots[k] = {FingerNote, k, 0};
        break;
      case Chords:
        slots[k] = {FingerChord, k, -1};
        break;
      case Split:
      default: {
        // Left hand: I, IV, V, vi chords (pinky to index).
        // Right hand: root, 2nd, 3rd, 5th melody notes (index to pinky).
        static constexpr int chordDegrees[4] = {0, 3, 4, 5};
        static constexpr int melodyDegrees[4] = {0, 1, 2, 4};
        slots[k] = k < 4 ? FingerSlot{FingerChord, chordDegrees[k], -1} : FingerSlot{FingerNote, melodyDegrees[k - 4], 0};
      }
    }
  }
  return slots;
}

Engine::Engine() {
  lastSeen_.fill(-1e9);
  lastCc_.fill(-1);
  expr_.fill(0.f);
  for (int s = 0; s < kSides; ++s) {
    expr_[s * 5 + 0] = 0.5f;  // height
    expr_[s * 5 + 1] = 0.5f;  // x
    expr_[s * 5 + 4] = 0.5f;  // tilt
  }
}

std::vector<int> Engine::notesForKey(int key) const {
  if (key < 0 || key >= kKeys) return {};
  const FingerSlot& slot = p_.fingers[key];
  const int octave = p_.octave + slot.octave;
  switch (slot.mode) {
    case FingerNote:
      return {degreeToNote(slot.degree, p_.scale, octave)};
    case FingerChord:
      return diatonicChord(slot.degree, p_.scale, octave);
    default:
      return {};
  }
}

int Engine::velocityFor(const HandFeatures& f, const FingerState& fs) const {
  switch (p_.velocityMode) {
    case VelocityHeight:
      return static_cast<int>(std::lround(20 + f.height * 107));
    case VelocitySpeed:
      // Extension speed in units per second; a quick flick is ~4.
      return static_cast<int>(std::lround(30 + 97 * std::clamp(fs.speed / 4.f, 0.f, 1.f)));
    case VelocityFixed:
    default:
      return p_.velocity;
  }
}

void Engine::releaseFinger(int owner, FingerState& fs, double now) {
  for (int n : fs.notes) book_.release(owner, n, now, p_.graceMs / 1000.0);
  fs.notes.clear();
  fs.on = false;
  fs.pendingOff = false;
}

void Engine::resetFingers() {
  for (auto& side : fingers_)
    for (auto& fs : side) fs = FingerState{};
}

void Engine::setParams(const Params& p, std::vector<MidiEvent>& out) {
  const bool repitch = p.fingers != p_.fingers || !(p.scale == p_.scale) || p.octave != p_.octave ||
                       p.channel != p_.channel || p.notes != p_.notes || p.hands != p_.hands;
  if (repitch) panic(out);
  if (p.ccOut != p_.ccOut || p.ccBase != p_.ccBase) lastCc_.fill(-1);
  for (auto& s : smooth_) s.timeConstantMs = p.smoothingMs;
  filter_.configure(p.landmarkCutoff, p.landmarkBeta);
  p_ = p;
}

void Engine::panic(std::vector<MidiEvent>& out) {
  book_.releaseAll(p_.channel, out);
  resetFingers();
}

Output Engine::process(const Frame& raw) {
  Output out;
  const double now = raw.time;
  const double dt = lastTime_ < 0.0 ? 0.0 : std::max(0.0, now - lastTime_);
  lastTime_ = now;

  out.frame = filter_.apply(raw);
  const float threshold = 0.75f - 0.5f * std::clamp(p_.sensitivity, 0.f, 1.f);

  for (int s = 0; s < kSides; ++s) {
    const Side side = static_cast<Side>(s);
    const Hand& hand = out.frame.hands[s];
    out.handPresent[s] = hand.present;

    HandFeatures f;
    bool usable = false;
    if (hand.present) {
      f = computeFeatures(hand, side, raw.aspect);
      lastFeatures_[s] = f;
      lastSeen_[s] = now;
      usable = true;
    } else if ((now - lastSeen_[s]) * 1000.0 <= p_.holdMs) {
      f = lastFeatures_[s];  // brief dropout: keep the last pose
      usable = true;
    }

    for (int finger = Index; finger <= Pinky; ++finger) {
      FingerState& fs = fingers_[s][finger];
      const int owner = s * kFingers + finger;
      bool gate = false;
      if (usable) {
        const float ext = extension(f, finger);
        if (hand.present && dt > 0.0) {
          const float inst = static_cast<float>((ext - fs.lastExt) / dt);
          fs.speed += 0.5f * (inst - fs.speed);
        }
        if (hand.present) fs.lastExt = ext;
        const bool sideOn = p_.hands == BothHands || (p_.hands == LeftHandOnly) == (side == Left);
        gate = fs.gate.apply(ext, threshold, kGateWidth) && p_.notes && sideOn;
      } else {
        fs.gate.reset();
        fs.speed = 0.f;
      }

      if (gate && !fs.on) {
        fs.on = true;
        fs.onAt = now;
        fs.pendingOff = false;
        fs.notes = notesForKey(keyPosition(side, finger));
        const int velocity = velocityFor(f, fs);
        for (int n : fs.notes) book_.claim(owner, n, velocity, p_.channel, now, out.midi);
      } else if (gate && fs.on) {
        fs.pendingOff = false;
      } else if (!gate && fs.on) {
        if ((now - fs.onAt) * 1000.0 >= p_.minNoteMs) releaseFinger(owner, fs, now);
        else fs.pendingOff = true;
      }
      out.fingerOn[s][finger] = fs.on;
    }

    if (hand.present) {
      const float values[5] = {f.height, f.x, f.pinch, f.fist, f.tilt};
      for (int i = 0; i < 5; ++i) expr_[s * 5 + i] = smooth_[s * 5 + i].apply(values[i], dt);
    }
  }

  book_.flush(now, p_.channel, out.midi);

  out.expr = expr_;
  if (p_.ccOut) {
    for (int i = 0; i < kExpr; ++i) {
      const int v = static_cast<int>(std::lround(expr_[i] * 127.f));
      if (v != lastCc_[i]) {
        lastCc_[i] = v;
        out.midi.push_back({static_cast<uint8_t>(0xB0 | (p_.channel & 15)),
                            static_cast<uint8_t>(std::clamp(p_.ccBase + i, 0, 127)), static_cast<uint8_t>(v)});
      }
    }
  }
  return out;
}

}  // namespace mh
