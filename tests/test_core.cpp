// Unit tests for core/. Run with `make test`.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../core/assign.hpp"
#include "../core/audio.hpp"
#include "../core/clutch.hpp"
#include "../core/engine.hpp"
#include "../core/features.hpp"
#include "../core/filters.hpp"
#include "../core/links.hpp"
#include "../core/music.hpp"
#include "../core/preview.hpp"
#include "../core/version.hpp"

using namespace mh;

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    ++checks;                                                             \
    if (!(cond)) {                                                        \
      ++failures;                                                         \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                     \
  } while (0)

std::vector<std::pair<std::string, std::function<void()>>>& registry() {
  static std::vector<std::pair<std::string, std::function<void()>>> r;
  return r;
}

struct Register {
  Register(const char* name, std::function<void()> fn) { registry().emplace_back(name, std::move(fn)); }
};

#define TEST(name)                          \
  void name();                              \
  Register reg_##name(#name, name);         \
  void name()

// Synthetic hand centred at cx, cy. Each finger points up when its entry in
// `straight` is true and folds back toward the palm otherwise.
std::array<Point, kLandmarks> makeHand(float cx, float cy, std::array<bool, kFingers> straight) {
  std::array<Point, kLandmarks> lm{};
  lm[kWrist] = {cx, cy + 0.15f};
  for (int f = 0; f < kFingers; ++f) {
    const float x = cx - 0.06f + 0.03f * f;
    const Point mcp = {x, cy};
    const Point pip = {x, cy - 0.05f};
    const Point tip = straight[f] ? Point{x, cy - 0.12f} : Point{x + 0.005f, cy - 0.01f};
    lm[kMcp[f]] = mcp;
    lm[kPip[f]] = pip;
    lm[kPip[f] + 1] = {(pip.x + tip.x) * 0.5f, (pip.y + tip.y) * 0.5f};
    lm[kTip[f]] = tip;
  }
  lm[1] = {cx - 0.05f, cy + 0.1f};
  return lm;
}

constexpr std::array<bool, kFingers> kOpen = {true, true, true, true, true};
constexpr std::array<bool, kFingers> kFist = {false, false, false, false, false};

Frame frameWith(double t, const std::array<Point, kLandmarks>* left, const std::array<Point, kLandmarks>* right) {
  Frame f;
  f.time = t;
  f.aspect = 1.f;
  if (left) f.hands[Left] = {true, 1.f, *left};
  if (right) f.hands[Right] = {true, 1.f, *right};
  return f;
}

int count(const std::vector<MidiEvent>& events, uint8_t type) {
  int n = 0;
  for (const auto& e : events) n += (e.status & 0xF0) == type;
  return n;
}

}  // namespace

TEST(versions_compare_numerically) {
  CHECK(compareVersions("0.1.0", "0.1.0") == 0);
  CHECK(compareVersions("v0.2.0", "0.1.9") == 1);
  CHECK(compareVersions("0.9.0", "0.10.0") == -1);
  CHECK(compareVersions("1.0", "1.0.0") == 0);
  CHECK(compareVersions("0.0.0-dev", "0.1.0") == -1);
  // Prereleases rank below their release, so beta testers are offered the final version.
  CHECK(compareVersions("0.3.0-beta.1", "0.3.0") == -1);
  CHECK(compareVersions("0.3.0", "0.3.0-beta.1") == 1);
  CHECK(compareVersions("0.3.0-beta.1", "0.2.0") == 1);
  CHECK(compareVersions("0.3.0-beta.2", "0.3.0-beta.10") == -1);
  CHECK(compareVersions("0.3.0-beta.1", "0.3.0-beta.1") == 0);
  CHECK(compareVersions("0.3.0-alpha", "0.3.0-beta") == -1);
}

TEST(landmark_filter_follows_motion_and_can_be_off) {
  // A fingertip moving 1 normalized unit/s at 30 fps: the filter used now
  // trails by well under a frame of motion, the old setting by several.
  const auto lagAfter = [](float beta) {
    OneEuro f;
    f.beta = beta;
    float out = 0.f;
    for (int i = 0; i <= 15; ++i) out = f.apply(i / 30.f, i / 30.0);
    return 15 / 30.f - out;
  };
  CHECK(lagAfter(20.f) < 0.01f);
  CHECK(lagAfter(0.3f) > 0.05f);

  LandmarkFilter off;
  off.configure(0.f, 0.f);
  Frame a, b;
  a.time = 0.0;
  b.time = 1.0 / 30.0;
  a.hands[0].present = b.hands[0].present = true;
  b.hands[0].lm[8].x = 0.5f;
  off.apply(a);
  CHECK(off.apply(b).hands[0].lm[8].x == 0.5f);
}

TEST(degrees_follow_scale_and_wrap_octaves) {
  Scale major;
  CHECK(degreeToNote(0, major, 0) == 60);
  CHECK(degreeToNote(2, major, 0) == 64);
  CHECK(degreeToNote(7, major, 0) == 72);
  CHECK(degreeToNote(-1, major, 0) == 59);
  CHECK(degreeToNote(0, major, -1) == 48);
  Scale dMinor;
  dMinor.set(2, {0, 2, 3, 5, 7, 8, 10});
  CHECK(degreeToNote(0, dMinor, 0) == 62);
  CHECK(degreeToNote(2, dMinor, 0) == 65);
}

TEST(diatonic_chords_stack_thirds) {
  Scale major;
  CHECK((diatonicChord(0, major, 0) == std::vector<int>{60, 64, 67}));  // C major
  CHECK((diatonicChord(1, major, 0) == std::vector<int>{62, 65, 69}));  // D minor
  CHECK((diatonicChord(6, major, 0) == std::vector<int>{71, 74, 77}));  // B diminished
}

TEST(empty_scale_falls_back_to_chromatic) {
  Scale s;
  s.set(0, {});
  CHECK(s.size == 12);
  CHECK(degreeToNote(1, s, 0) == 61);
}

TEST(shared_note_stops_only_after_last_owner_and_grace) {
  NoteBook book;
  std::vector<MidiEvent> out;
  book.claim(0, 60, 100, 0, 0.0, out);
  book.claim(1, 60, 100, 0, 0.0, out);
  CHECK(count(out, 0x90) == 2);
  CHECK(count(out, 0x80) == 1);  // re-strike for the second owner
  out.clear();
  book.release(0, 60, 1.0, 0.04);
  book.flush(2.0, 0, out);
  CHECK(out.empty());  // owner 1 still holds it
  book.release(1, 60, 2.0, 0.04);
  book.flush(2.01, 0, out);
  CHECK(out.empty());  // inside the grace period
  book.flush(2.05, 0, out);
  CHECK(count(out, 0x80) == 1);
}

TEST(reclaim_inside_grace_keeps_note_sounding) {
  NoteBook book;
  std::vector<MidiEvent> out;
  book.claim(0, 60, 100, 0, 0.0, out);
  book.release(0, 60, 1.0, 0.04);
  out.clear();
  book.claim(1, 60, 90, 0, 1.01, out);
  book.flush(1.1, 0, out);
  CHECK(count(out, 0x80) == 1);  // only the re-strike
  CHECK(count(out, 0x90) == 1);
  CHECK(book.sounding(60));
}

TEST(curl_separates_straight_and_folded_fingers) {
  const auto open = makeHand(0.5f, 0.5f, kOpen);
  const auto fist = makeHand(0.5f, 0.5f, kFist);
  const HandFeatures a = computeFeatures({true, 1.f, open}, Right, 1.f);
  const HandFeatures b = computeFeatures({true, 1.f, fist}, Right, 1.f);
  for (int f = Index; f <= Pinky; ++f) {
    CHECK(a.curl[f] < 0.2f);
    CHECK(b.curl[f] > 0.8f);
  }
  CHECK(a.fist < 0.1f);
  CHECK(b.fist > 0.9f);
}

TEST(open_hand_plays_and_fist_releases_after_min_length) {
  Engine engine;
  Params p;
  p.layout = Keys;
  p.fingers = presetSlots(Keys);
  std::vector<MidiEvent> ignored;
  engine.setParams(p, ignored);
  const auto open = makeHand(0.7f, 0.5f, kOpen);
  const auto fist = makeHand(0.7f, 0.5f, kFist);

  Output o = engine.process(frameWith(0.0, nullptr, &open));
  CHECK(count(o.midi, 0x90) == 4);  // index..pinky of the right hand
  o = engine.process(frameWith(0.05, nullptr, &fist));
  CHECK(count(o.midi, 0x80) == 0);  // shorter than minNoteMs
  o = engine.process(frameWith(0.25, nullptr, &fist));
  o = engine.process(frameWith(0.30, nullptr, &fist));
  CHECK(count(o.midi, 0x80) == 4);  // released after the grace period
}

TEST(brief_dropout_holds_notes_long_dropout_releases) {
  Engine engine;
  Params p;
  p.layout = Keys;
  p.fingers = presetSlots(Keys);
  std::vector<MidiEvent> ignored;
  engine.setParams(p, ignored);
  const auto open = makeHand(0.7f, 0.5f, kOpen);
  engine.process(frameWith(0.0, nullptr, &open));
  Output o = engine.process(frameWith(0.2, nullptr, nullptr));
  CHECK(count(o.midi, 0x80) == 0);
  o = engine.process(frameWith(0.25, nullptr, &open));
  CHECK(count(o.midi, 0x90) == 0);  // no re-trigger after the dropout
  engine.process(frameWith(0.7, nullptr, nullptr));
  o = engine.process(frameWith(0.8, nullptr, nullptr));
  CHECK(count(o.midi, 0x80) == 4);
}

TEST(changing_scale_releases_everything) {
  Engine engine;
  Params p;
  std::vector<MidiEvent> out;
  engine.setParams(p, out);
  const auto open = makeHand(0.3f, 0.5f, kOpen);
  engine.process(frameWith(0.0, &open, nullptr));
  p.scale.set(2, {0, 2, 3, 5, 7, 8, 10});
  out.clear();
  engine.setParams(p, out);
  CHECK(count(out, 0x80) > 0);
  // Fingers still extended: they re-trigger in the new key.
  Output o = engine.process(frameWith(0.1, &open, nullptr));
  CHECK(count(o.midi, 0x90) > 0);
}

TEST(notes_off_silences_fingers_but_keeps_expressions) {
  Engine engine;
  Params p;
  p.notes = false;
  std::vector<MidiEvent> out;
  engine.setParams(p, out);
  const auto open = makeHand(0.7f, 0.3f, kOpen);
  Output o = engine.process(frameWith(0.0, nullptr, &open));
  CHECK(o.midi.empty());
  CHECK(o.expr[RHeight] > 0.5f);
}

TEST(cc_out_sends_each_expression_once_until_it_changes) {
  Engine engine;
  Params p;
  p.notes = false;
  p.ccOut = true;
  std::vector<MidiEvent> out;
  engine.setParams(p, out);
  const auto open = makeHand(0.7f, 0.3f, kOpen);
  Output o = engine.process(frameWith(0.0, nullptr, &open));
  CHECK(count(o.midi, 0xB0) == kExpr);
  o = engine.process(frameWith(0.02, nullptr, &open));
  CHECK(count(o.midi, 0xB0) == 0);
}

TEST(assigner_sorts_two_hands_by_position) {
  HandAssigner a;
  Detection l, r;
  l.lm = makeHand(0.3f, 0.5f, kOpen);
  r.lm = makeHand(0.7f, 0.5f, kOpen);
  l.chirality = Right;  // detector wrong on one hand only: position wins
  r.chirality = Right;
  Frame f = a.assign({r, l}, 0.0, 1.f);
  CHECK(f.hands[Left].present && f.hands[Left].lm[kWrist].x < 0.5f);
  CHECK(f.hands[Right].present && f.hands[Right].lm[kWrist].x > 0.5f);
}

TEST(assigner_trusts_crossed_hands_only_when_both_agree) {
  HandAssigner a;
  Detection l, r;
  l.lm = makeHand(0.3f, 0.5f, kOpen);
  r.lm = makeHand(0.7f, 0.5f, kOpen);
  l.chirality = Right;
  r.chirality = Left;
  Frame f = a.assign({l, r}, 0.0, 1.f);
  CHECK(f.hands[Right].lm[kWrist].x < 0.5f);
}

TEST(assigner_keeps_single_hand_on_its_side) {
  HandAssigner a;
  Detection d;
  d.lm = makeHand(0.6f, 0.5f, kOpen);
  d.chirality = Left;
  a.assign({d}, 0.0, 1.f);
  d.chirality = Right;  // detector flips, hand barely moved
  d.lm = makeHand(0.61f, 0.5f, kOpen);
  Frame f = a.assign({d}, 0.02, 1.f);
  CHECK(f.hands[Left].present && !f.hands[Right].present);
}

TEST(hands_setting_limits_notes_to_one_side) {
  Engine engine;
  Params p;
  p.layout = Keys;
  p.fingers = presetSlots(Keys);
  p.hands = LeftHandOnly;
  std::vector<MidiEvent> ignored;
  engine.setParams(p, ignored);
  const auto left = makeHand(0.3f, 0.5f, kOpen);
  const auto right = makeHand(0.7f, 0.5f, kOpen);
  Output o = engine.process(frameWith(0.0, &left, &right));
  CHECK(count(o.midi, 0x90) == 4);  // only the left hand's fingers
  CHECK(o.fingerOn[Left][Index] && !o.fingerOn[Right][Index]);
  CHECK(o.expr[RHeight] > 0.f);     // expressions still follow both hands
}

TEST(preview_is_mirrored_and_draws_hands) {
  // Source frame: left half dark, right half bright.
  std::vector<uint8_t> luma(64 * 32);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 64; ++x) luma[y * 64 + x] = x < 32 ? 0 : 200;
  const GrayImage g = mirroredThumbnail({luma.data(), 64, 32, 64}, 32, 16);
  CHECK(g.pixels[0] > 150);       // mirrored: bright side now on the left
  CHECK(g.pixels[31] < 50);

  Frame f;
  const auto hand = makeHand(0.5f, 0.5f, kOpen);
  f.hands[Right] = {true, 1.f, hand};
  std::array<std::array<bool, kFingers>, kSides> on{};
  on[Right][Index] = true;
  std::vector<uint8_t> argb(32 * 16 * 4);
  renderPreview(g, f, on, argb.data(), 32 * 4);
  bool colored = false;
  for (size_t i = 0; i < argb.size(); i += 4) colored |= argb[i + 1] != argb[i + 3];  // r != b
  CHECK(colored);
}

TEST(presets_lay_out_fingers_left_to_right) {
  const auto keys = presetSlots(Keys);
  for (int k = 0; k < kKeys; ++k) CHECK(keys[k].mode == FingerNote && keys[k].degree == k);
  const auto split = presetSlots(Split);
  CHECK(split[0].mode == FingerChord && split[0].degree == 0);  // left pinky: I
  CHECK(split[3].degree == 5);                                   // left index: vi
  CHECK(split[4].mode == FingerNote && split[4].degree == 0);   // right index: root
  CHECK(keyPosition(Left, Pinky) == 0 && keyPosition(Left, Index) == 3);
  CHECK(keyPosition(Right, Index) == 4 && keyPosition(Right, Pinky) == 7);
}

TEST(custom_finger_slots_play_what_they_say) {
  Engine engine;
  Params p;
  p.layout = Custom;
  p.fingers = presetSlots(Keys);
  p.fingers[4] = {FingerChord, 3, 0};  // right index: IV chord
  p.fingers[5] = {FingerOff, 0, 0};    // right middle: silent
  p.fingers[6] = {FingerNote, 2, 1};   // right ring: 3rd, one octave up
  std::vector<MidiEvent> out;
  engine.setParams(p, out);
  CHECK((engine.notesForKey(4) == std::vector<int>{65, 69, 72}));  // F A C
  CHECK(engine.notesForKey(5).empty());
  CHECK((engine.notesForKey(6) == std::vector<int>{76}));
  const auto open = makeHand(0.7f, 0.5f, kOpen);
  Output o = engine.process(frameWith(0.0, nullptr, &open));
  CHECK(count(o.midi, 0x90) == 3 + 0 + 1 + 1);  // chord + off + note + pinky note
}

// ---- audio features (MidiHands Audio) ---------------------------------------------

namespace {
constexpr double kSr = 48000.0;
constexpr double kTau = 6.283185307179586;

// Feeds `seconds` of a generated mono signal in 512-sample blocks, like Live would.
void feed(AudioAnalyzer& a, double seconds, const std::function<float(double)>& signal, double& t) {
  std::vector<float> block(512);
  const int total = int(seconds * kSr);
  for (int done = 0; done < total; done += 512) {
    const int n = std::min(512, total - done);
    for (int i = 0; i < n; ++i, t += 1.0 / kSr) block[i] = signal(t);
    a.process(block.data(), static_cast<const float*>(nullptr), n);
  }
}
}  // namespace

TEST(audio_silence_is_zero) {
  AudioAnalyzer a;
  a.setSampleRate(kSr);
  double t = 0;
  feed(a, 1.0, [](double) { return 0.f; }, t);
  const auto f = a.features();
  CHECK(f.level == 0.f && f.bass == 0.f && f.mid == 0.f && f.high == 0.f && f.beat == 0.f);
  CHECK(a.beats() == 0);
}

TEST(audio_bands_follow_frequency) {
  AudioAnalyzer low, high;
  low.setSampleRate(kSr);
  high.setSampleRate(kSr);
  double t1 = 0, t2 = 0;
  feed(low, 1.5, [](double t) { return 0.5f * float(std::sin(kTau * 70.0 * t)); }, t1);
  feed(high, 1.5, [](double t) { return 0.5f * float(std::sin(kTau * 6000.0 * t)); }, t2);
  const auto l = low.features(), h = high.features();
  CHECK(l.level > 0.9f);
  CHECK(l.bass > 0.9f);
  CHECK(l.high < 0.05f);
  CHECK(l.mid < l.bass - 0.3f);
  CHECK(h.high > 0.9f);
  CHECK(h.bass < 0.05f);
}

TEST(audio_beats_on_kicks) {
  // A kick-like burst (60 Hz, 80 ms decay) every 500 ms for 4 s: 8 hits.
  AudioAnalyzer a;
  a.setSampleRate(kSr);
  double t = 0;
  std::vector<double> hitTimes;
  float lastPulse = 0.f;
  for (int block = 0; block < int(4.0 * kSr / 512); ++block) {
    feed(a, 512 / kSr, [](double t) {
      const double local = std::fmod(t, 0.5);
      return float(0.8 * std::exp(-local / 0.08) * std::sin(kTau * 60.0 * local));
    }, t);
    if (a.features().beat > lastPulse + 0.5f) hitTimes.push_back(t);
    lastPulse = a.features().beat;
  }
  CHECK(a.beats() == 8);
  CHECK(hitTimes.size() == 8);
  for (size_t i = 0; i < hitTimes.size(); ++i) CHECK(std::fabs(hitTimes[i] - 0.5 * double(i)) < 0.03);
}

TEST(audio_beats_over_bass_and_noise) {
  // Kicks every 500 ms over a held bass note and hats-like noise, as in a mix.
  AudioAnalyzer a;
  a.setSampleRate(kSr);
  double t = 0;
  unsigned seed = 7;
  feed(a, 8.0, [&seed](double t) {
    seed = seed * 1664525u + 1013904223u;
    const double local = std::fmod(t, 0.5);
    const double kick = 0.6 * std::exp(-local / 0.08) * std::sin(kTau * 55.0 * local);
    return float(kick + 0.2 * std::sin(kTau * 82.0 * t) + 0.1 * (double(seed >> 8) / 8388608.0 - 1.0));
  }, t);
  CHECK(a.beats() == 16);
}

TEST(audio_no_beats_in_steady_noise) {
  AudioAnalyzer a;
  a.setSampleRate(kSr);
  double t = 0;
  unsigned seed = 12345;
  auto noise = [&seed](double) { seed = seed * 1664525u + 1013904223u; return 0.3f * (float(seed >> 8) / 8388608.f - 1.f); };
  feed(a, 0.5, noise, t);
  const int afterStart = a.beats();  // the noise starting may count once
  feed(a, 3.0, noise, t);
  CHECK(afterStart <= 1);
  CHECK(a.beats() - afterStart == 0);
}

TEST(audio_auto_level_fills_range) {
  // Quiet and loud material both reach the top; without auto level the quiet one stays low.
  for (const bool autoLevel : {true, false}) {
    AudioAnalyzer quiet, loud;
    AudioSettings s;
    s.autoLevel = autoLevel;
    for (AudioAnalyzer* a : {&quiet, &loud}) {
      a->setSampleRate(kSr);
      a->setSettings(s);
    }
    double t1 = 0, t2 = 0;
    feed(quiet, 2.0, [](double t) { return 0.02f * float(std::sin(kTau * 220.0 * t)); }, t1);
    feed(loud, 2.0, [](double t) { return 0.7f * float(std::sin(kTau * 220.0 * t)); }, t2);
    if (autoLevel) CHECK(std::fabs(quiet.features().level - loud.features().level) < 0.1f);
    else CHECK(quiet.features().level < loud.features().level - 0.5f);
  }
}

TEST(thumb_span_tells_tucked_relaxed_and_out) {
  auto hand = makeHand(0.5f, 0.5f, kOpen);
  const auto span = [&](Point tip) {
    hand[kThumbTip] = tip;
    return computeFeatures({true, 1.f, hand}, Right, 1.f).thumbSpan;
  };
  CHECK(span({0.475f, 0.455f}) < HandClutches::kThresholds[GestureThumbIn].on);    // against the index finger
  const float relaxed = span({0.38f, 0.42f});
  CHECK(relaxed > HandClutches::kThresholds[GestureThumbIn].off && relaxed < HandClutches::kThresholds[GestureThumbOut].off);
  CHECK(span({0.25f, 0.48f}) > HandClutches::kThresholds[GestureThumbOut].on);     // stuck out
}

TEST(clutch_gestures_settle_hold_and_let_go) {
  HandClutches c;
  HandFeatures f;
  f.present = true;
  f.thumbSpan = 0.6f;
  f.curl.fill(1.f);
  double t = 0.0;
  const auto at = [&](double dt, bool usable = true) { t += dt; return c.apply(f, usable, t); };
  CHECK(!at(0.0)[GestureThumbOut]);
  f.thumbSpan = 1.2f;
  CHECK(!at(1 / 30.0)[GestureThumbOut]);  // a single frame does not count yet
  CHECK(!at(1 / 30.0)[GestureThumbOut]);
  CHECK(at(1 / 30.0)[GestureThumbOut]);
  f.thumbSpan = 0.75f;                     // between the thresholds: stays out
  CHECK(at(0.1)[GestureThumbOut]);
  f.thumbSpan = 0.5f;
  at(1 / 30.0);
  CHECK(!at(0.1)[GestureThumbOut]);
  f.curl[Pinky] = 0.f;  // a straight pinky counts, whatever the other fingers do
  f.curl[Index] = f.curl[Middle] = f.curl[Ring] = 0.f;
  at(1 / 30.0);
  CHECK(at(0.1)[GesturePinky]);
  f.fist = 0.9f;
  f.curl[Index] = f.curl[Middle] = f.curl[Ring] = 0.9f;
  at(1 / 30.0);
  const auto g = at(0.1);
  CHECK(g[GestureFist] && g[GesturePinky] && !g[GestureThumbIn]);
  CHECK(!at(1 / 30.0, false)[GestureFist]);  // hand lost: lets go at once
}

TEST(engage_options_name_a_hand_and_a_gesture) {
  Gestures g{};
  g[Right][GestureFist] = true;
  CHECK(engaged(kEngageAlways, g));
  CHECK(engaged(clutchFor(Right, GestureFist), g));
  CHECK(!engaged(clutchFor(Left, GestureFist), g));
  CHECK(!engaged(clutchFor(Right, GesturePinky), g));
  CHECK(clutchFor(Left, GestureThumbOut) == 1 && clutchFor(Right, GesturePinky) == kClutches);
}

namespace {
struct LinkRig {
  Links links;
  std::vector<LinkEvent> ev;
  std::array<float, kExpr> expr{};
  Gestures g{};
  double t = 0.0;
  LinkRig(LinkParams p) {
    p.source = RHeight;
    links.setParams(0, p);
    links.setTarget(0, 42);
    links.setReady(true);
    links.setMovement(true);
  }
  void step(float height, double dt = 1 / 30.0) {
    expr[RHeight] = height;
    t += dt;
    links.setInput(expr, g);
    ev.clear();
    links.update(t, ev);
  }
  int count(LinkEvent::Kind kind) const {
    int n = 0;
    for (const auto& e : ev) n += e.kind == kind;
    return n;
  }
  float value() const {
    for (auto e = ev.rbegin(); e != ev.rend(); ++e)
      if (e->kind == LinkEvent::Value) return e->value;
    return -1.f;
  }
  bool near(float v) const { return std::fabs(value() - v) < 1e-4f; }
};
}  // namespace

TEST(links_jump_to_the_hand_and_need_a_target) {
  LinkRig r(LinkParams{});
  r.step(0.3f);
  CHECK(r.near(0.3f) && r.count(LinkEvent::Attach) == 1 && r.ev[0].kind == LinkEvent::Value && r.ev[0].rampMs == 0.f);
  r.step(0.4f);
  CHECK(r.near(0.4f) && r.ev[0].rampMs > 0.f && r.count(LinkEvent::Attach) == 0);
  r.links.setTarget(0, 0);
  r.step(0.5f);
  CHECK(r.count(LinkEvent::Detach) == 1);
  r.step(0.6f);
  CHECK(r.ev.empty());
}

TEST(links_clutch_engages_and_lets_go_in_place) {
  LinkParams p;
  p.engage = clutchFor(Left, GestureFist);
  LinkRig r(p);
  r.step(0.3f);
  CHECK(r.ev.empty() && r.links.state(0) == LinkIdle);
  r.g[Left][GestureFist] = true;
  r.step(0.5f);
  CHECK(r.near(0.5f) && r.count(LinkEvent::Attach) == 1 && r.links.state(0) == LinkMoving);
  r.step(0.6f);
  CHECK(r.near(0.6f));
  r.g[Left][GestureFist] = false;
  r.step(0.9f);
  CHECK(r.count(LinkEvent::Detach) == 1 && r.count(LinkEvent::Value) == 0);  // stays where it was
}

TEST(links_grab_moves_on_from_the_parameter) {
  LinkParams p;
  p.takeover = TakeoverGrab;
  LinkRig r(p);
  r.step(0.8f);
  CHECK(r.count(LinkEvent::Read) == 1 && r.count(LinkEvent::Attach) == 0);
  r.links.parameterValue(0, 0.2f);
  r.step(0.8f);
  CHECK(r.near(0.2f) && r.count(LinkEvent::Attach) == 1);  // no jump to the hand's 0.8
  r.step(0.9f);
  CHECK(r.near(0.3f));
  r.step(0.7f);
  CHECK(r.near(0.1f));
  r.step(0.0f);
  CHECK(r.near(0.f));
  r.step(0.1f);
  CHECK(r.near(0.1f));  // back up at once, like a knob at its end stop

  LinkRig silent(p);  // nobody answers: carries on after the timeout
  silent.step(0.6f);
  silent.step(0.6f, Links::kReadTimeout);
  CHECK(silent.count(LinkEvent::Attach) == 1 && silent.near(0.6f));
}

TEST(links_pickup_waits_for_the_hand) {
  LinkParams p;
  p.takeover = TakeoverPickup;
  LinkRig r(p);
  r.step(0.8f);
  r.links.parameterValue(0, 0.5f);
  r.step(0.8f);
  CHECK(r.ev.empty() && r.links.state(0) == LinkWaiting);
  r.step(0.6f);
  CHECK(r.ev.empty());
  r.step(0.45f);  // passed it
  CHECK(r.near(0.45f) && r.count(LinkEvent::Attach) == 1);
  r.step(0.4f);
  CHECK(r.near(0.4f));

  LinkRig w(p);  // switched to Jump while waiting: takes over at once
  w.step(0.8f);
  w.links.parameterValue(0, 0.5f);
  w.step(0.8f);
  p.takeover = TakeoverJump;
  p.source = RHeight;
  w.links.setParams(0, p);
  w.step(0.8f);
  CHECK(w.near(0.8f) && w.count(LinkEvent::Attach) == 1);
}

TEST(links_return_puts_the_parameter_back) {
  LinkParams p;
  p.engage = clutchFor(Right, GestureThumbOut);
  p.ret = true;
  LinkRig r(p);
  r.g[Right][GestureThumbOut] = true;
  r.step(0.7f);
  CHECK(r.count(LinkEvent::Read) == 1);
  r.links.parameterValue(0, 0.25f);
  r.step(0.7f);
  CHECK(r.near(0.7f) && r.count(LinkEvent::Attach) == 1);
  r.g[Right][GestureThumbOut] = false;
  r.step(0.7f);
  CHECK(r.near(0.25f) && r.count(LinkEvent::Detach) == 0);
  CHECK(std::fabs(r.links.deadline() - (r.t + Links::kReturnSettle)) < 1e-9);
  r.step(0.7f, Links::kReturnSettle);
  CHECK(r.count(LinkEvent::Detach) == 1 && r.links.deadline() < 0.0);
}

TEST(links_follow_the_movement_switch_and_new_targets) {
  LinkRig r(LinkParams{});
  r.step(0.3f);
  r.links.setMovement(false);
  r.step(0.3f);
  CHECK(r.count(LinkEvent::Detach) == 1);
  r.links.setMovement(true);
  r.step(0.3f);
  CHECK(r.count(LinkEvent::Attach) == 1);
  r.links.setTarget(0, 43);
  r.step(0.3f);
  CHECK(r.ev.size() == 3 && r.ev[0].kind == LinkEvent::Detach && r.ev[2].kind == LinkEvent::Attach);
}

TEST(links_shape_ranges_curves_and_inversion) {
  Links links;
  LinkParams p;
  p.lo = 0.2f;
  p.hi = 0.6f;
  p.min = 1.f;
  p.max = 0.f;  // inverted
  links.setParams(0, p);
  CHECK(std::fabs(links.shape(0, 0.2f) - 1.f) < 1e-5f && std::fabs(links.shape(0, 0.6f)) < 1e-5f);
  CHECK(std::fabs(links.shape(0, 0.9f)) < 1e-5f);
  p.min = 0.f;
  p.max = 1.f;
  p.curve = 50.f;  // exponent 2
  links.setParams(0, p);
  CHECK(std::fabs(links.shape(0, 0.4f) - 0.25f) < 1e-5f);
}

TEST(slot_switches_hold_and_toggle) {
  SlotSwitches sw;
  std::vector<SlotSwitch> out;
  Gestures g{};
  const int fist = clutchFor(Right, GestureFist);
  sw.setOn(0, true);
  sw.setOn(1, true);
  sw.setGesture(0, fist);  // Hold
  sw.setGesture(1, fist);
  sw.setMode(1, SlotSwitches::Toggle);
  sw.update(g, out);
  CHECK(out.size() == 1 && out[0].slot == 0 && !out[0].on);  // Hold: off until held
  out.clear();
  g[Right][GestureFist] = true;
  sw.update(g, out);
  CHECK(out.size() == 2 && out[0].on && !out[1].on);  // Hold on; Toggle flips off
  out.clear();
  sw.update(g, out);
  CHECK(out.empty());  // still held: nothing new
  g[Right][GestureFist] = false;
  sw.update(g, out);
  CHECK(out.size() == 1 && out[0].slot == 0 && !out[0].on);  // Hold lets go; Toggle stays
  out.clear();
  g[Right][GestureFist] = true;
  sw.update(g, out);
  CHECK(out.size() == 2 && out[1].slot == 1 && out[1].on);
  sw.setGesture(2, kEngageAlways);
  out.clear();
  sw.update(g, out);
  CHECK(out.empty());  // slots without a gesture are left alone
}

int main() {
  for (auto& [name, fn] : registry()) {
    const int before = failures;
    fn();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", name.c_str());
  }
  std::printf("\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
