// Audio features for the visuals: loudness, three bands and a beat pulse,
// each 0..1, from a stereo signal. Runs on the audio thread (no allocation
// after setSampleRate); used by the mh.audio~ external in "MidiHands Audio".
#pragma once

#include <array>

namespace mh {

struct AudioFeatures {
  float level = 0.f;  // whole signal
  float bass = 0.f;   // below 150 Hz: kick, bass
  float mid = 0.f;    // 150 Hz to 2.5 kHz: snare body, vocals, chords
  float high = 0.f;   // above 2.5 kHz: hats, air
  float beat = 0.f;   // 1 on a hit, then decays
};

struct AudioSettings {
  float gainDb = 0.f;         // added before the range (manual level)
  bool autoLevel = true;      // follow the loudest recent part, so quiet and loud material both fill 0..1
  float smoothMs = 120.f;     // how slowly values fall back
  float sensitivity = 0.5f;   // beat detection, 0..1 (higher: more hits)
  float beatDecayMs = 250.f;  // how long a beat pulse lasts
};

// RBJ biquad, transposed direct form II.
struct Biquad {
  float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
  float z1 = 0.f, z2 = 0.f;
  void lowpass(double sampleRate, double hz, double q);
  void highpass(double sampleRate, double hz, double q);
  float apply(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

class AudioAnalyzer {
 public:
  AudioAnalyzer() { setSampleRate(48000.0); }
  void setSampleRate(double sampleRate);
  void setSettings(const AudioSettings& s) { settings_ = s; }
  void reset();

  // Interleaving-free stereo input; right may be null (mono).
  template <typename T>
  void process(const T* left, const T* right, int frames) {
    for (int i = 0; i < frames; ++i) {
      const float x = right ? 0.5f * (float(left[i]) + float(right[i])) : float(left[i]);
      sample(x);
    }
  }

  // Latest values; read from another thread through the owner's own handoff.
  const AudioFeatures& features() const { return features_; }
  // Hits counted since reset (tests, diagnostics).
  int beats() const { return beats_; }

 private:
  enum Band { kLevel, kBass, kMid, kHigh, kBands };
  void sample(float x);
  void hop();

  double sampleRate_ = 48000.0;
  AudioSettings settings_;
  AudioFeatures features_;

  // Band filters, 24 dB/octave edges: bass = 2 x lowpass 150, mid = 2 x highpass 150
  // + 2 x lowpass 2500, high = 2 x highpass 2500.
  Biquad bassA_, bassB_, midA_, midB_, midC_, midD_, highA_, highB_;

  // Per band: power envelope (fast attack, smoothMs release) and the auto level reference.
  std::array<float, kBands> env_{};
  std::array<float, kBands> peakDb_{};
  float attack_ = 0.f;  // per-sample coefficient

  // Beat detection works on hops of ~5 ms: the energy of the latest window per band
  // (20 ms for bass, 10 ms above) against that band's average over ~100 ms. Tuned
  // with kicks over a held bass and a noise bed: all kicks up to a bass at a third
  // of the kick's level, no hits in steady noise.
  int hopSize_ = 256;
  int hopFill_ = 0;
  std::array<float, kBands> hopEnergy_{};
  std::array<std::array<float, 4>, kBands> recent_{};  // energy of the last hops, a ring
  int ringPos_ = 0;
  std::array<float, kBands> average_{};
  bool above_ = false;
  double sinceBeat_ = 1.0;  // seconds
  float beatPulse_ = 0.f;
  int beats_ = 0;
  double hopSeconds_ = 0.0;
};

}  // namespace mh
