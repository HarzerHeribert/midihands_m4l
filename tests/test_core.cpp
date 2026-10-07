// Unit tests for core/. Run with `make test`.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../core/assign.hpp"
#include "../core/engine.hpp"
#include "../core/features.hpp"
#include "../core/music.hpp"

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

int main() {
  for (auto& [name, fn] : registry()) {
    const int before = failures;
    fn();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", name.c_str());
  }
  std::printf("\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
