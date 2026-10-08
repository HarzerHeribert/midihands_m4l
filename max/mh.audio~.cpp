// mh.audio~: the sound of a track as values for MidiHands' video effects.
// Used by the "MidiHands Audio" device; the analysis is core/audio.
//
// Inlets: left and right signal (the device passes its audio through itself).
// Messages:
//   channel <0..7>         which letter (A..H) this device sends as
//   name <words>           shown in MidiHands next to the letter (the track's name)
//   gain <dB>, autolevel <0|1>, smooth <ms>, sensitivity <0..1>, decay <ms>
// Left outlet (to `s mh_audio`, read by every MidiHands device):
//   v <channel> <level> <bass> <mid> <high> <beat>   about 60 times a second
//   n <channel> <id> <name>                          once a second and on changes
// Right outlet: <level> <bass> <mid> <high> <beat> for the device's own meters.
#include <atomic>
#include <mutex>
#include <random>
#include <string>

#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"

#include "../core/audio.hpp"

namespace {

constexpr double kTickMs = 1000.0 / 60.0;
constexpr int kPresenceTicks = 60;  // presence once a second
constexpr int kIdleTicks = 12;      // no audio for ~200 ms: report silence once

struct State {
  mh::AudioAnalyzer analyzer;
  // Settings: written by messages, picked up by the audio thread when it can take the lock.
  std::mutex settingsMutex;
  mh::AudioSettings settings;
  bool settingsDirty = true;
  bool stereo = true;
  std::atomic<float> values[5] = {};
  std::atomic<unsigned> blocks{0};  // audio blocks processed, to notice when audio stops
};

}  // namespace

struct t_mh_audio {
  t_pxobject ob;
  State* st;
  void* out;    // left: messages for MidiHands
  void* meter;  // right: values for the device's meters
  void* clock;
  long channel;
  long id;
  t_symbol* name;
  int ticks;
  int idle;
  unsigned lastBlocks;
};

static t_class* gClass = nullptr;

static void mh_presence(t_mh_audio* x) {
  t_atom a[3];
  atom_setlong(a, x->channel);
  atom_setlong(a + 1, x->id);
  atom_setsym(a + 2, x->name);
  outlet_anything(x->out, gensym("n"), 3, a);
}

static void mh_tick(t_mh_audio* x) {
  State& st = *x->st;
  float v[5];
  for (int i = 0; i < 5; ++i) v[i] = st.values[i].load(std::memory_order_relaxed);
  const unsigned blocks = st.blocks.load(std::memory_order_relaxed);
  x->idle = blocks == x->lastBlocks ? x->idle + 1 : 0;
  x->lastBlocks = blocks;
  const bool sending = x->idle < kIdleTicks;
  if (x->idle == kIdleTicks) for (float& f : v) f = 0.f;  // audio stopped (device off, transport idle)
  if (sending || x->idle == kIdleTicks) {
    t_atom a[6];
    atom_setlong(a, x->channel);
    for (int i = 0; i < 5; ++i) atom_setfloat(a + 1 + i, v[i]);
    outlet_list(x->meter, nullptr, 5, a + 1);
    outlet_anything(x->out, gensym("v"), 6, a);
  }
  if (++x->ticks >= kPresenceTicks) {
    x->ticks = 0;
    mh_presence(x);
  }
  clock_fdelay(x->clock, kTickMs);
}

static void mh_perform64(t_mh_audio* x, t_object*, double** ins, long numins, double**, long, long frames, long, void*) {
  State& st = *x->st;
  {
    std::unique_lock<std::mutex> lock(st.settingsMutex, std::try_to_lock);
    if (lock.owns_lock() && st.settingsDirty) {
      st.analyzer.setSettings(st.settings);
      st.settingsDirty = false;
    }
  }
  st.analyzer.process(ins[0], numins > 1 && st.stereo ? ins[1] : static_cast<const double*>(nullptr), int(frames));
  const mh::AudioFeatures& f = st.analyzer.features();
  st.values[0].store(f.level, std::memory_order_relaxed);
  st.values[1].store(f.bass, std::memory_order_relaxed);
  st.values[2].store(f.mid, std::memory_order_relaxed);
  st.values[3].store(f.high, std::memory_order_relaxed);
  st.values[4].store(f.beat, std::memory_order_relaxed);
  st.blocks.fetch_add(1, std::memory_order_relaxed);
}

static void mh_dsp64(t_mh_audio* x, t_object* dsp64, short* count, double samplerate, long, long) {
  x->st->analyzer.setSampleRate(samplerate);  // the audio thread is stopped while the chain compiles
  x->st->stereo = count[1] != 0;
  object_method(dsp64, gensym("dsp_add64"), x, mh_perform64, 0, nullptr);
}

template <typename F>
static void mh_set(t_mh_audio* x, F change) {
  std::lock_guard<std::mutex> lock(x->st->settingsMutex);
  change(x->st->settings);
  x->st->settingsDirty = true;
}

static void mh_channel(t_mh_audio* x, long c) {
  x->channel = c < 0 ? 0 : (c > 7 ? 7 : c);
  mh_presence(x);
}

static void mh_name(t_mh_audio* x, t_symbol*, long argc, t_atom* argv) {
  std::string name;
  for (long i = 0; i < argc; ++i) {
    char* text = nullptr;
    long size = 0;
    atom_gettext(1, argv + i, &size, &text, OBEX_UTIL_ATOM_GETTEXT_SYM_NO_QUOTE);
    if (text) {
      if (!name.empty()) name += ' ';
      name += text;
      sysmem_freeptr(text);
    }
  }
  x->name = gensym(name.c_str());
  mh_presence(x);
}

static void mh_gain(t_mh_audio* x, double db) { mh_set(x, [db](mh::AudioSettings& s) { s.gainDb = float(db); }); }
static void mh_autolevel(t_mh_audio* x, long on) { mh_set(x, [on](mh::AudioSettings& s) { s.autoLevel = on != 0; }); }
static void mh_smooth(t_mh_audio* x, double ms) { mh_set(x, [ms](mh::AudioSettings& s) { s.smoothMs = float(ms); }); }
static void mh_sensitivity(t_mh_audio* x, double v) { mh_set(x, [v](mh::AudioSettings& s) { s.sensitivity = float(v); }); }
static void mh_decay(t_mh_audio* x, double ms) { mh_set(x, [ms](mh::AudioSettings& s) { s.beatDecayMs = float(ms); }); }

static void mh_assist(t_mh_audio*, void*, long io, long index, char* s) {
  if (io == ASSIST_INLET) snprintf(s, 256, index == 0 ? "(signal) left, messages" : "(signal) right");
  else snprintf(s, 256, index == 0 ? "v/n messages for MidiHands (s mh_audio)" : "level bass mid high beat");
}

static void* mh_new(t_symbol*, long, t_atom*) {
  auto* x = static_cast<t_mh_audio*>(object_alloc(gClass));
  if (!x) return nullptr;
  dsp_setup(reinterpret_cast<t_pxobject*>(x), 2);
  x->meter = listout(x);
  x->out = outlet_new(x, nullptr);
  x->st = new State();
  x->channel = 0;
  std::random_device rd;
  x->id = long(rd() & 0x7fffffff);
  x->name = gensym("");
  x->ticks = kPresenceTicks;  // announce on the first tick
  x->idle = 0;
  x->lastBlocks = 0;
  x->clock = clock_new(x, reinterpret_cast<method>(mh_tick));
  clock_fdelay(x->clock, kTickMs);
  return x;
}

static void mh_free(t_mh_audio* x) {
  dsp_free(reinterpret_cast<t_pxobject*>(x));
  clock_unset(x->clock);
  object_free(x->clock);
  delete x->st;
}

void ext_main(void*) {
  t_class* c = class_new("mh.audio~", reinterpret_cast<method>(mh_new), reinterpret_cast<method>(mh_free),
                         sizeof(t_mh_audio), nullptr, A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_dsp64), "dsp64", A_CANT, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_channel), "channel", A_LONG, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_name), "name", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_gain), "gain", A_FLOAT, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_autolevel), "autolevel", A_LONG, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_smooth), "smooth", A_FLOAT, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_sensitivity), "sensitivity", A_FLOAT, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_decay), "decay", A_FLOAT, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_assist), "assist", A_CANT, 0);
  class_dspinit(c);
  class_register(CLASS_BOX, c);
  gClass = c;
}
