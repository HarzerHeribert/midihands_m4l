#include "audio.hpp"

#include <algorithm>
#include <cmath>

namespace mh {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kWindowDb = 24.f;      // a value goes from 0 to 1 over this many dB
constexpr float kBandFloorDb = 12.f;   // a band's reference stays within this of the overall loudness
constexpr float kPeakFallDbPerS = 3.f; // auto level: how fast the reference follows quieter material
constexpr float kManualTopDb = -6.f;   // without auto level, -6 dBFS (plus gain) reads 1
constexpr float kSilenceDb = -70.f;

float toDb(float power) { return 10.f * std::log10(power + 1e-12f); }
}  // namespace

void Biquad::lowpass(double sr, double hz, double q) {
  const double w = 2.0 * kPi * hz / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
  b0 = float((1.0 - c) / 2.0 / a0);
  b1 = float((1.0 - c) / a0);
  b2 = b0;
  a1 = float(-2.0 * c / a0);
  a2 = float((1.0 - alpha) / a0);
  z1 = z2 = 0.f;
}

void Biquad::highpass(double sr, double hz, double q) {
  const double w = 2.0 * kPi * hz / sr, c = std::cos(w), alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
  b0 = float((1.0 + c) / 2.0 / a0);
  b1 = float(-(1.0 + c) / a0);
  b2 = b0;
  a1 = float(-2.0 * c / a0);
  a2 = float((1.0 - alpha) / a0);
  z1 = z2 = 0.f;
}

void AudioAnalyzer::setSampleRate(double sampleRate) {
  sampleRate_ = sampleRate > 1000.0 ? sampleRate : 48000.0;
  const double q = 0.7071;
  bassA_.lowpass(sampleRate_, 150.0, q);
  bassB_.lowpass(sampleRate_, 150.0, q);
  midA_.highpass(sampleRate_, 150.0, q);
  midB_.highpass(sampleRate_, 150.0, q);
  midC_.lowpass(sampleRate_, 2500.0, q);
  midD_.lowpass(sampleRate_, 2500.0, q);
  highA_.highpass(sampleRate_, 2500.0, q);
  highB_.highpass(sampleRate_, 2500.0, q);
  hopSize_ = std::max(32, int(std::lround(sampleRate_ * 0.0053)));
  hopSeconds_ = double(hopSize_) / sampleRate_;
  attack_ = float(1.0 - std::exp(-1.0 / (sampleRate_ * 0.005)));
  reset();
}

void AudioAnalyzer::reset() {
  for (Biquad* b : {&bassA_, &bassB_, &midA_, &midB_, &midC_, &midD_, &highA_, &highB_}) b->z1 = b->z2 = 0.f;
  env_.fill(0.f);
  peakDb_.fill(kSilenceDb);
  hopEnergy_.fill(0.f);
  for (auto& r : recent_) r.fill(0.f);
  average_.fill(0.f);
  ringPos_ = 0;
  hopFill_ = 0;
  above_ = false;
  sinceBeat_ = 1.0;
  beatPulse_ = 0.f;
  beats_ = 0;
  features_ = AudioFeatures{};
}

void AudioAnalyzer::sample(float x) {
  const float b = bassB_.apply(bassA_.apply(x));
  const float m = midD_.apply(midC_.apply(midB_.apply(midA_.apply(x))));
  const float h = highB_.apply(highA_.apply(x));
  const float power[kBands] = {x * x, b * b, m * m, h * h};
  const float release = float(1.0 - std::exp(-1.0 / (sampleRate_ * std::max(5.f, settings_.smoothMs) / 1000.0)));
  for (int i = 0; i < kBands; ++i) {
    env_[i] += (power[i] > env_[i] ? attack_ : release) * (power[i] - env_[i]);
    hopEnergy_[i] += power[i];
  }
  if (++hopFill_ >= hopSize_) hop();
}

void AudioAnalyzer::hop() {
  // ---- levels: dB against a reference, so 0..1 covers the last kWindowDb below it
  std::array<float, kBands> db{};
  for (int i = 0; i < kBands; ++i) {
    db[i] = toDb(env_[i]) + settings_.gainDb;
    float& peak = peakDb_[i];
    peak = std::max(db[i], std::max(kSilenceDb, peak - float(kPeakFallDbPerS * hopSeconds_)));
  }
  float value[kBands];
  for (int i = 0; i < kBands; ++i) {
    float top;
    if (!settings_.autoLevel) top = kManualTopDb;
    else if (i == kLevel) top = peakDb_[kLevel];
    else top = std::max(peakDb_[i], peakDb_[kLevel] - kBandFloorDb);  // an empty band stays low
    value[i] = db[kLevel] < kSilenceDb ? 0.f : std::clamp((db[i] - (top - kWindowDb)) / kWindowDb, 0.f, 1.f);
  }

  // ---- beats: a band's latest energy jumping above its own recent average
  ringPos_ = (ringPos_ + 1) % 4;
  float novelty = 0.f;
  std::array<float, kBands> window{};
  const float weight[kBands] = {0.f, 1.f, 0.6f, 0.4f};
  const int windowHops[kBands] = {2, 4, 2, 2};  // bass needs 20 ms to measure reliably
  for (int i = 0; i < kBands; ++i) {
    const float e = hopEnergy_[i] / float(hopSize_);
    recent_[i][ringPos_] = e;
    hopEnergy_[i] = 0.f;
    for (int k = 0; k < windowHops[i]; ++k) window[i] += recent_[i][(ringPos_ + 4 - k) % 4];
    window[i] /= float(windowHops[i]);
    if (i != kLevel) novelty += weight[i] * std::max(0.f, toDb(window[i]) - toDb(average_[i]));
    average_[i] += float(hopSeconds_ / 0.1) * (e - average_[i]);
  }
  hopFill_ = 0;

  const float threshold = 13.f - 9.f * std::clamp(settings_.sensitivity, 0.f, 1.f);  // dB
  const bool loud = db[kLevel] - settings_.gainDb > kSilenceDb + 10.f;
  const bool hit = loud && novelty > threshold;
  sinceBeat_ += hopSeconds_;
  if (hit && !above_ && sinceBeat_ > 0.09) {
    beatPulse_ = 1.f;
    sinceBeat_ = 0.0;
    ++beats_;
    // The rest of this sound is not a new hit: lift the averages towards it.
    for (int i = 0; i < kBands; ++i) average_[i] = std::max(average_[i], 0.5f * recent_[i][ringPos_]);
  } else {
    beatPulse_ *= float(std::exp(-hopSeconds_ / (std::max(20.f, settings_.beatDecayMs) / 1000.0)));
  }
  above_ = hit;

  features_.level = value[kLevel];
  features_.bass = value[kBass];
  features_.mid = value[kMid];
  features_.high = value[kHigh];
  features_.beat = beatPulse_;
}

}  // namespace mh
