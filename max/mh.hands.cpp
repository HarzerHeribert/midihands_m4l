// mh.hands: camera hand tracking -> MIDI, as a Max external for Max for Live.
//
// All instances in Live share one backend (mac/camera_hub): each camera that
// at least one instance has switched on runs one capture + Vision pipeline,
// and every instance on that camera receives the same tracked hands. Each
// instance has its own engine, so settings (layout, scale, hands) are per
// device.
//
// Threads:
//   capture thread   shared detection, then this instance's engine
//   scheduler thread a clock flushes MIDI and expression values (timing-critical)
//   main thread      qelems send drawing data, status and the finger layout
//
// Inlet messages (all from the device patch):
//   open [camera index | name], close, cameras, panic
//   notes 0/1, hands 0-2, layout 0-3, octave n, velmode 0-2, velocity 1-127,
//   sensitivity 0-1, minnote ms, hold ms, smoothing ms, channel 1-16,
//   ccout 0/1, ccbase n, root 0-11, scaletype index, scale <intervals...>,
//   finger <key 0-7> <mode 0-2> <degree> <octave>, or one field at a time:
//   fingermode|fingerdeg|fingeroct <key> <value>
//   "layout 0-2" loads that preset into the finger table; 3 (custom) keeps it.
//   report: resend version, assets url, status, picture and finger layout (an editor page
//     opened), and check GitHub for a newer release
//   checkupdate, update: check for / install the latest release (mac/updater)
//   record 1 <x> <y> <w> <h> | record 0, reveal: record the video window (mac/recorder)
//   links (core/links): linkset <k> <on|src|lo|hi|curve|min|max|eng|take|ret> <value>,
//     linktarget <k> <id> (the linked parameter, 0 = none), linkvalue <k> <0-1 | -1>
//     (answer to "read"), movement 0/1 (the Movement switch), linksready (Live's API is up)
//   effect slots switched by a gesture (core/clutch SlotSwitches): fxset <slot> <on|sw|swm> <value>
//
// Outlets, left to right:
//   0 MIDI bytes as 3-int lists, for [midiout]
//   1 expression values: 10 floats 0-1 (see kExprNames), then 8 clutch gestures 0/1
//     (left thumb out, thumb in, fist, pinky, then right), then the left and right thumb
//     span (what thumb in/out compare, 0 without a hand)
//   2 drawing data for the hand views: "hands" aspect + 2 x (present, 21 x/y, 4 finger states)
//   3 info: cameras, status, stats, error, picture <url>, assets <url> (Live's fonts),
//      version <x.y.z>, update available|current|unknown|installing|installed|failed [text],
//      recording 0|1, recorded <path>, recordfail <why>,
//      fingertable <8 x mode degree octave>, fingers <8 x count + 4 notes>,
//      scaleinfo <root> <intervals...>, linkstate <16 x 0 idle | 1 moving | 2 waiting>
//   4 links, for each link's live.remote~: link <k> value <0-1> <ramp ms>, link <k> attach,
//      link <k> detach; read <k> (ask for the parameter's value); fxon <slot> <0|1> (a
//      gesture switched an effect slot: set its On parameter)
#include "ext.h"
#include "ext_obex.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../core/engine.hpp"
#include "../core/links.hpp"
#include "../core/version.hpp"
#include "../platform/camera_hub.hpp"
#include "../platform/preview_server.hpp"
#include "../platform/recorder.hpp"
#include "../platform/shell.hpp"
#include "../platform/updater.hpp"

namespace {

struct ScaleType {
  const char* name;
  std::vector<int> steps;
};

// Same order as the device's scale menu.
const std::vector<ScaleType>& scaleTypes() {
  static const std::vector<ScaleType> types = {
      {"Major", {0, 2, 4, 5, 7, 9, 11}},          {"Minor", {0, 2, 3, 5, 7, 8, 10}},
      {"Dorian", {0, 2, 3, 5, 7, 9, 10}},         {"Phrygian", {0, 1, 3, 5, 7, 8, 10}},
      {"Lydian", {0, 2, 4, 6, 7, 9, 11}},         {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
      {"Locrian", {0, 1, 3, 5, 6, 8, 10}},        {"Harmonic Minor", {0, 2, 3, 5, 7, 8, 11}},
      {"Melodic Minor", {0, 2, 3, 5, 7, 9, 11}},  {"Major Pentatonic", {0, 2, 4, 7, 9}},
      {"Minor Pentatonic", {0, 3, 5, 7, 10}},     {"Blues", {0, 3, 5, 6, 7, 10}},
      {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
  };
  return types;
}

constexpr int kViewFloats = 1 + mh::kSides * (1 + mh::kLandmarks * 2 + 4);
constexpr int kExprFloats = mh::kExpr + mh::kClutches + mh::kSides;

// Links and their timeouts run on one clock, whatever the camera does.
double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Everything shared between threads lives here, guarded by `mutex`.
struct State {
  std::mutex mutex;
  mh::Engine engine;
  mh::Params params;
  std::vector<int> scaleSteps = {0, 2, 4, 5, 7, 9, 11};
  std::vector<mh::MidiEvent> pendingMidi;
  std::array<float, mh::kExpr> expr{};
  mh::Gestures gestures{};
  std::array<float, mh::kSides> thumbSpan{};
  bool exprFresh = false;
  mh::SlotSwitches switches;
  std::vector<mh::SlotSwitch> pendingSwitches;
  mh::Links links;
  std::vector<mh::LinkEvent> pendingLinks;
  std::array<int, mh::Links::kLinks> linkStates{};
  bool linkStatesFresh = false;
  mh::Output view;
  mh::TrackerStats stats;
  // Main thread only: what was last reported, to skip unchanged layouts.
  std::string lastLayout;
  mh::CameraInfo camera;
  int cameraUsers = 0;
  // Update check / install result, handed to the main thread by updateQelem.
  std::string update;      // "available", "current", "unknown", "installing", "installed", "failed"
  std::string updateText;  // version or reason
  // Info messages from other threads (recorder), sent by infoQelem.
  std::vector<std::pair<std::string, std::string>> pendingInfo;
};

}  // namespace

typedef struct _mh_hands {
  t_object ob;
  void* outMidi;
  void* outExpr;
  void* outView;
  void* outInfo;
  void* outLinks;
  t_clock* flushClock;
  t_clock* linkClock;  // wakes the links for read timeouts and Return
  t_qelem* linkStateQelem;
  t_qelem* viewQelem;
  t_qelem* layoutQelem;
  t_qelem* accessQelem;
  t_qelem* updateQelem;
  t_qelem* infoQelem;
  t_symbol* pendingCamera;
  long pendingCameraIndex;
  long subscription;  // CameraHub subscriber id, 0 while closed
  State* st;
} t_mh_hands;

static t_class* mh_class = nullptr;

// The camera permission prompt can outlive the object that asked for it.
static std::mutex gAliveMutex;
static std::set<t_mh_hands*> gAlive;

static void mh_info(t_mh_hands* x, const char* selector, long argc, t_atom* argv) {
  outlet_anything(x->outInfo, gensym(selector), static_cast<short>(argc), argv);
}

static void mh_error(t_mh_hands* x, const std::string& text) {
  t_atom a;
  atom_setsym(&a, gensym(text.c_str()));
  mh_info(x, "error", 1, &a);
  object_error(reinterpret_cast<t_object*>(x), "%s", text.c_str());
}

// Under the lock: let the links act on what changed, and wake them again when
// one of them waits for something (a read timeout, a Return settling).
static void mh_linksUpdate(t_mh_hands* x) {
  State* st = x->st;
  const double now = nowSeconds();
  st->links.update(now, st->pendingLinks);
  st->switches.update(st->gestures, st->pendingSwitches);
  for (int k = 0; k < mh::Links::kLinks; ++k) {
    const int state = st->links.state(k);
    if (state != st->linkStates[k]) {
      st->linkStates[k] = state;
      st->linkStatesFresh = true;
    }
  }
  if (st->linkStatesFresh) qelem_set(x->linkStateQelem);
  const double next = st->links.deadline();
  if (next >= 0.0) clock_fdelay(x->linkClock, std::max(0.0, (next - now) * 1000.0) + 1.0);
}

// Scheduler thread: MIDI first, then expressions, then link actions.
static void mh_flush(t_mh_hands* x) {
  std::vector<mh::MidiEvent> midi;
  std::vector<mh::LinkEvent> links;
  std::vector<mh::SlotSwitch> switches;
  t_atom values[kExprFloats];
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    midi.swap(x->st->pendingMidi);
    links.swap(x->st->pendingLinks);
    switches.swap(x->st->pendingSwitches);
    fresh = x->st->exprFresh;
    x->st->exprFresh = false;
    if (fresh) {
      int n = 0;
      for (float v : x->st->expr) atom_setfloat(values + n++, v);
      for (const auto& side : x->st->gestures)
        for (bool g : side) atom_setfloat(values + n++, g ? 1.0 : 0.0);
      for (float span : x->st->thumbSpan) atom_setfloat(values + n++, span);
    }
  }
  for (const mh::MidiEvent& e : midi) {
    t_atom a[3];
    atom_setlong(a, e.status);
    atom_setlong(a + 1, e.data1);
    atom_setlong(a + 2, e.data2);
    outlet_list(x->outMidi, nullptr, 3, a);
  }
  if (fresh) outlet_list(x->outExpr, nullptr, kExprFloats, values);
  for (const mh::LinkEvent& e : links) {
    t_atom a[4];
    atom_setlong(a, e.link);
    if (e.kind == mh::LinkEvent::Read) {
      outlet_anything(x->outLinks, gensym("read"), 1, a);
      continue;
    }
    static const char* kinds[] = {"value", "attach", "detach"};
    atom_setsym(a + 1, gensym(kinds[e.kind]));
    atom_setfloat(a + 2, e.value);
    atom_setfloat(a + 3, e.rampMs);
    outlet_anything(x->outLinks, gensym("link"), e.kind == mh::LinkEvent::Value ? 4 : 2, a);
  }
  for (const mh::SlotSwitch& sw : switches) {
    t_atom a[2];
    atom_setlong(a, sw.slot);
    atom_setlong(a + 1, sw.on ? 1 : 0);
    outlet_anything(x->outLinks, gensym("fxon"), 2, a);
  }
}

// Scheduler thread: a link waited long enough.
static void mh_linkTick(t_mh_hands* x) {
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    mh_linksUpdate(x);
  }
  mh_flush(x);
}

// Main thread: what each link is doing, for the editor.
static void mh_linkStateOut(t_mh_hands* x) {
  t_atom a[mh::Links::kLinks];
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    if (!x->st->linkStatesFresh) return;
    x->st->linkStatesFresh = false;
    for (int k = 0; k < mh::Links::kLinks; ++k) atom_setlong(a + k, x->st->linkStates[k]);
  }
  mh_info(x, "linkstate", mh::Links::kLinks, a);
}

// Main thread: hand drawing data and timing stats, coalesced by the qelem.
static void mh_view(t_mh_hands* x) {
  mh::Output view;
  mh::TrackerStats stats;
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    view = x->st->view;
    stats = x->st->stats;
  }
  t_atom a[kViewFloats];
  int n = 0;
  atom_setfloat(a + n++, view.frame.aspect);
  for (int s = 0; s < mh::kSides; ++s) {
    const mh::Hand& hand = view.frame.hands[s];
    atom_setfloat(a + n++, hand.present ? 1.0 : 0.0);
    for (int i = 0; i < mh::kLandmarks; ++i) {
      atom_setfloat(a + n++, hand.lm[i].x);
      atom_setfloat(a + n++, hand.lm[i].y);
    }
    for (int f = mh::Index; f <= mh::Pinky; ++f) atom_setfloat(a + n++, view.fingerOn[s][f] ? 1.0 : 0.0);
  }
  outlet_anything(x->outView, gensym("hands"), n, a);

  t_atom s[3];
  atom_setfloat(s, stats.fps);
  atom_setfloat(s + 1, stats.detectMs);
  atom_setfloat(s + 2, stats.latencyMs);
  mh_info(x, "stats", 3, s);
}

// Main thread: what every finger plays, for the editor's labels, keyboard
// and stored finger parameters.
static void mh_layoutOut(t_mh_hands* x, bool force) {
  mh::Params p;
  std::vector<std::vector<int>> notes;
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    p = x->st->engine.params();
    for (int k = 0; k < mh::kKeys; ++k) notes.push_back(x->st->engine.notesForKey(k));
  }
  // Reporting the table makes the patch store it, which comes back as
  // settings; only speaking up on real changes keeps that from looping.
  std::string key;
  for (int k = 0; k < mh::kKeys; ++k) {
    key += std::to_string(p.fingers[k].mode) + "," + std::to_string(p.fingers[k].degree) + "," +
           std::to_string(p.fingers[k].octave) + ":";
    for (int n : notes[k]) key += std::to_string(n) + ",";
  }
  key += "|" + std::to_string(p.scale.root);
  for (int i = 0; i < p.scale.size; ++i) key += "," + std::to_string(p.scale.intervals[i]);
  if (!force && key == x->st->lastLayout) return;
  x->st->lastLayout = key;

  t_atom table[mh::kKeys * 3];
  for (int k = 0; k < mh::kKeys; ++k) {
    atom_setlong(table + k * 3, p.fingers[k].mode);
    atom_setlong(table + k * 3 + 1, p.fingers[k].degree);
    atom_setlong(table + k * 3 + 2, p.fingers[k].octave);
  }
  mh_info(x, "fingertable", mh::kKeys * 3, table);

  t_atom resolved[mh::kKeys * 5];
  for (int k = 0; k < mh::kKeys; ++k) {
    atom_setlong(resolved + k * 5, static_cast<long>(notes[k].size()));
    for (int i = 0; i < 4; ++i) atom_setlong(resolved + k * 5 + 1 + i, i < int(notes[k].size()) ? notes[k][i] : -1);
  }
  mh_info(x, "fingers", mh::kKeys * 5, resolved);

  std::vector<t_atom> scale(1 + p.scale.size);
  atom_setlong(scale.data(), p.scale.root);
  for (int i = 0; i < p.scale.size; ++i) atom_setlong(scale.data() + 1 + i, p.scale.intervals[i]);
  mh_info(x, "scaleinfo", static_cast<long>(scale.size()), scale.data());
}

static void mh_layout(t_mh_hands* x) { mh_layoutOut(x, false); }

// Settings can arrive on the main or the scheduler thread, so every edit
// happens under the lock together with handing the result to the engine.
template <typename Edit>
static void mh_edit(t_mh_hands* x, Edit edit) {
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    edit(x->st->params, x->st->scaleSteps);
    x->st->params.scale.set(x->st->params.scale.root, x->st->scaleSteps);
    x->st->engine.setParams(x->st->params, x->st->pendingMidi);
  }
  clock_fdelay(x->flushClock, 0.0);
  qelem_set(x->layoutQelem);
}

static void mh_cameras(t_mh_hands* x) {
  std::vector<t_atom> atoms;
  for (const mh::CameraInfo& c : mh::listCameras()) {
    t_atom a;
    atom_setsym(&a, gensym(c.name.c_str()));
    atoms.push_back(a);
  }
  mh_info(x, "cameras", static_cast<long>(atoms.size()), atoms.data());
}

static void mh_stop(t_mh_hands* x) {
  if (x->subscription) {
    mh::CameraHub::shared().unsubscribe(x->subscription);  // no callbacks after this
    x->subscription = 0;
  }
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    x->st->engine.panic(x->st->pendingMidi);
    x->st->view = mh::Output{};
    // No camera, no clutch gestures: clutched links let go; the rest keep their value.
    x->st->gestures = mh::Gestures{};
    x->st->thumbSpan = {};
    x->st->links.setInput(x->st->expr, x->st->gestures);
    mh_linksUpdate(x);
  }
  clock_fdelay(x->flushClock, 0.0);
  qelem_set(x->viewQelem);
  t_atom a;
  atom_setsym(&a, gensym("stopped"));
  mh_info(x, "status", 1, &a);
}

static void mh_status(t_mh_hands* x) {
  if (!x->subscription) {
    t_atom a;
    atom_setsym(&a, gensym("stopped"));
    mh_info(x, "status", 1, &a);
    return;
  }
  const mh::CameraInfo& info = x->st->camera;
  int users = 1;
  for (const auto& [name, count] : mh::CameraHub::shared().activeCameras())
    if (name == info.name) users = count;
  t_atom a[6];
  atom_setsym(a, gensym("running"));
  atom_setsym(a + 1, gensym(info.name.c_str()));
  atom_setlong(a + 2, info.width);
  atom_setlong(a + 3, info.height);
  atom_setfloat(a + 4, info.fps);
  atom_setlong(a + 5, users);
  mh_info(x, "status", 6, a);

  const std::string url = mh::CameraHub::shared().pictureUrl(x->subscription);
  if (!url.empty()) {
    t_atom u;
    atom_setsym(&u, gensym(url.c_str()));
    mh_info(x, "picture", 1, &u);
  }
}

// Main thread: report the update state set by a background callback.
static void mh_updateOut(t_mh_hands* x) {
  std::string state, text;
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    state = x->st->update;
    text = x->st->updateText;
  }
  if (state.empty()) return;
  t_atom a[2];
  atom_setsym(a, gensym(state.c_str()));
  atom_setsym(a + 1, gensym(text.c_str()));
  mh_info(x, "update", text.empty() ? 1 : 2, a);
}

static void mh_setUpdate(t_mh_hands* x, const std::string& state, const std::string& text) {
  std::lock_guard<std::mutex> alive(gAliveMutex);
  if (!gAlive.count(x)) return;  // the device went away while GitHub answered
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    x->st->update = state;
    x->st->updateText = text;
  }
  qelem_set(x->updateQelem);
}

// Main thread: info messages queued by background callbacks.
static void mh_infoOut(t_mh_hands* x) {
  std::vector<std::pair<std::string, std::string>> items;
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    items.swap(x->st->pendingInfo);
  }
  for (const auto& [selector, text] : items) {
    t_atom a;
    atom_setsym(&a, gensym(text.c_str()));
    mh_info(x, selector.c_str(), text.empty() ? 0 : 1, &a);
  }
}

static void mh_post(t_mh_hands* x, const std::string& selector, const std::string& text) {
  std::lock_guard<std::mutex> alive(gAliveMutex);
  if (!gAlive.count(x)) return;
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    x->st->pendingInfo.emplace_back(selector, text);
  }
  qelem_set(x->infoQelem);
}

// record 1 <x> <y> <w> <h>: record that part of the video window with Live's
// sound; record 0: stop and save. reveal: show the last recording in Finder.
static t_mh_hands* gRecordingDevice = nullptr;  // main thread: the device whose window is recorded

static void mh_record(t_mh_hands* x, t_symbol*, long argc, t_atom* argv) {
  const bool on = argc > 0 && atom_getlong(argv) != 0;
  if (on && argc >= 5) {
    gRecordingDevice = x;
    mh::Recorder::shared().start("MidiHands Video", atom_getfloat(argv + 1), atom_getfloat(argv + 2),
                                 atom_getfloat(argv + 3), atom_getfloat(argv + 4), [x](bool ok, const std::string& msg) {
                                   if (ok) mh_post(x, "recording", "1");
                                   else mh_post(x, "recordfail", msg);
                                 });
  } else if (!on) {
    if (gRecordingDevice == x) gRecordingDevice = nullptr;
    mh::Recorder::shared().stop([x](bool ok, const std::string& msg) {
      mh_post(x, "recording", "0");
      mh_post(x, ok ? "recorded" : "recordfail", msg);
    });
  }
}

static void mh_reveal(t_mh_hands*) {
  const std::string file = mh::Recorder::shared().lastFile();
  if (!file.empty()) mh::revealFile(file);
}

static void mh_checkupdate(t_mh_hands* x) {
  mh::checkForUpdate([x](const mh::UpdateCheck& r) {
    if (!r.ok) mh_setUpdate(x, "unknown", r.error);
    else if (r.newer) mh_setUpdate(x, "available", r.latest);
    else mh_setUpdate(x, "current", r.latest);
  });
}

static void mh_update(t_mh_hands* x) {
  mh_setUpdate(x, "installing", "");
  mh::installUpdate([x](bool ok, const std::string& message) {
    mh_setUpdate(x, ok ? "installed" : "failed", message);
  });
}

static void mh_report(t_mh_hands* x) {
  t_atom v;
  atom_setsym(&v, gensym(mh::kVersion));
  mh_info(x, "version", 1, &v);
  const std::string assets = mh::PreviewServer::shared().baseUrl();
  if (!assets.empty()) {
    t_atom a;
    atom_setsym(&a, gensym(assets.c_str()));
    mh_info(x, "assets", 1, &a);
  }
  mh_status(x);
  mh_layoutOut(x, true);
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    x->st->linkStatesFresh = true;  // the page that asked shows the links' states
  }
  mh_linkStateOut(x);
  mh_checkupdate(x);
}

static void mh_start(t_mh_hands* x) {
  std::string camera;
  if (x->pendingCamera && x->pendingCamera != gensym("")) {
    camera = x->pendingCamera->s_name;
  } else if (x->pendingCameraIndex >= 0) {
    const auto cams = mh::listCameras();
    if (x->pendingCameraIndex < static_cast<long>(cams.size())) camera = cams[x->pendingCameraIndex].uid;
  }

  switch (mh::cameraAccess()) {
    case mh::CameraAccess::Undetermined: {
      t_atom a;
      atom_setsym(&a, gensym("waiting for camera permission"));
      mh_info(x, "status", 1, &a);
      mh::requestCameraAccess([x](bool) {
        std::lock_guard<std::mutex> lock(gAliveMutex);
        if (gAlive.count(x)) qelem_set(x->accessQelem);
      });
      return;
    }
    case mh::CameraAccess::Denied:
      mh_error(x, "Camera access denied. Allow Ableton Live in System Settings > Privacy & Security > Camera.");
      return;
    case mh::CameraAccess::Granted:
      break;
  }

  if (x->subscription) {
    mh::CameraHub::shared().unsubscribe(x->subscription);
    x->subscription = 0;
  }
  State* st = x->st;
  t_clock* flush = x->flushClock;
  t_qelem* view = x->viewQelem;
  std::string error;
  mh::CameraInfo info;
  x->subscription = mh::CameraHub::shared().subscribe(
      camera,
      [x, st, flush, view](const mh::HubFrame& hf) {
        {
          std::lock_guard<std::mutex> lock(st->mutex);
          mh::Output out = st->engine.process(hf.frame);
          st->pendingMidi.insert(st->pendingMidi.end(), out.midi.begin(), out.midi.end());
          st->expr = out.expr;
          st->gestures = out.gestures;
          st->thumbSpan = out.thumbSpan;
          st->links.setInput(out.expr, out.gestures);
          mh_linksUpdate(x);
          st->exprFresh = true;
          st->stats = hf.stats;
          out.midi.clear();
          st->view = std::move(out);
        }
        clock_fdelay(flush, 0.0);
        qelem_set(view);
      },
      &error, &info);
  if (!x->subscription) {
    mh_error(x, error);
    return;
  }
  x->st->camera = info;
  mh_status(x);
}

static void mh_open(t_mh_hands* x, t_symbol*, long argc, t_atom* argv) {
  x->pendingCamera = gensym("");
  x->pendingCameraIndex = -1;
  if (argc > 0 && atom_gettype(argv) == A_SYM) x->pendingCamera = atom_getsym(argv);
  else if (argc > 0) x->pendingCameraIndex = atom_getlong(argv);
  mh_start(x);
}

static void mh_close(t_mh_hands* x) { mh_stop(x); }

static void mh_panic(t_mh_hands* x) {
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    x->st->engine.panic(x->st->pendingMidi);
  }
  clock_fdelay(x->flushClock, 0.0);
}

static void mh_finger(t_mh_hands* x, t_symbol*, long argc, t_atom* argv) {
  if (argc < 4) {
    object_error(reinterpret_cast<t_object*>(x), "finger needs: key mode degree octave");
    return;
  }
  const int key = static_cast<int>(atom_getlong(argv));
  if (key < 0 || key >= mh::kKeys) return;
  const mh::FingerSlot slot{std::clamp(static_cast<int>(atom_getlong(argv + 1)), 0, 2),
                            std::clamp(static_cast<int>(atom_getlong(argv + 2)), -14, 21),
                            std::clamp(static_cast<int>(atom_getlong(argv + 3)), -3, 3)};
  mh_edit(x, [&](mh::Params& p, std::vector<int>&) { p.fingers[key] = slot; });
}

// fingermode / fingerdeg / fingeroct <key> <value>
static void mh_fingerField(t_mh_hands* x, t_symbol* s, long argc, t_atom* argv) {
  if (argc < 2) return;
  const int key = static_cast<int>(atom_getlong(argv));
  const int value = static_cast<int>(atom_getlong(argv + 1));
  if (key < 0 || key >= mh::kKeys) return;
  const std::string field = s->s_name;
  mh_edit(x, [&](mh::Params& p, std::vector<int>&) {
    mh::FingerSlot& slot = p.fingers[key];
    if (field == "fingermode") slot.mode = std::clamp(value, 0, 2);
    else if (field == "fingerdeg") slot.degree = std::clamp(value, -14, 21);
    else slot.octave = std::clamp(value, -3, 3);
  });
}

// Link settings and answers from the patch; the links act on them at once.
template <typename Edit>
static void mh_linkEdit(t_mh_hands* x, Edit edit) {
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    edit(x->st->links);
    mh_linksUpdate(x);
  }
  clock_fdelay(x->flushClock, 0.0);
}

// linkset <k> <key> <value>
static void mh_linkset(t_mh_hands* x, t_symbol*, long argc, t_atom* argv) {
  if (argc < 3 || atom_gettype(argv + 1) != A_SYM) return;
  const int k = static_cast<int>(atom_getlong(argv));
  if (k < 0 || k >= mh::Links::kLinks) return;
  const std::string key = atom_getsym(argv + 1)->s_name;
  const float v = static_cast<float>(atom_getfloat(argv + 2));
  const int i = static_cast<int>(atom_getlong(argv + 2));
  mh_linkEdit(x, [&](mh::Links& links) {
    mh::LinkParams p = links.params(k);
    if (key == "on") p.on = i != 0;
    else if (key == "src") p.source = std::clamp(i, 0, mh::kExpr - 1);
    else if (key == "lo") p.lo = v;
    else if (key == "hi") p.hi = v;
    else if (key == "curve") p.curve = std::clamp(v, -100.f, 100.f);
    else if (key == "min") p.min = v;
    else if (key == "max") p.max = v;
    else if (key == "eng") p.engage = std::clamp(i, 0, mh::kClutches);
    else if (key == "take") p.takeover = std::clamp(i, 0, 2);
    else if (key == "ret") p.ret = i != 0;
    else return;
    links.setParams(k, p);
  });
}

// linktarget <k> <id>: the parameter link k moves (0 = none)
static void mh_linktarget(t_mh_hands* x, t_symbol*, long argc, t_atom* argv) {
  if (argc < 2) return;
  const int k = static_cast<int>(atom_getlong(argv));
  const long id = atom_getlong(argv + argc - 1);  // "<k> <id>" or "<k> id <id>"
  if (k < 0 || k >= mh::Links::kLinks) return;
  mh_linkEdit(x, [&](mh::Links& links) { links.setTarget(k, id); });
}

// linkvalue <k> <value>: the parameter's normalized value, -1 if unknown
static void mh_linkvalue(t_mh_hands* x, t_symbol*, long argc, t_atom* argv) {
  if (argc < 2) return;
  const int k = static_cast<int>(atom_getlong(argv));
  const float v = static_cast<float>(atom_getfloat(argv + 1));
  mh_linkEdit(x, [&](mh::Links& links) { links.parameterValue(k, v); });
}

// fxset <slot> <on|sw|swm> <value>: an effect slot's On switch, gesture and mode
static void mh_fxset(t_mh_hands* x, t_symbol*, long argc, t_atom* argv) {
  if (argc < 3 || atom_gettype(argv + 1) != A_SYM) return;
  const int slot = static_cast<int>(atom_getlong(argv));
  const std::string key = atom_getsym(argv + 1)->s_name;
  const int v = static_cast<int>(atom_getlong(argv + 2));
  {
    std::lock_guard<std::mutex> lock(x->st->mutex);
    mh::SlotSwitches& sw = x->st->switches;
    if (key == "on") sw.setOn(slot, v != 0);
    else if (key == "sw") sw.setGesture(slot, std::clamp(v, 0, mh::kClutches));
    else if (key == "swm") sw.setMode(slot, v);
    mh_linksUpdate(x);
  }
  clock_fdelay(x->flushClock, 0.0);
}

static void mh_movement(t_mh_hands* x, long on) {
  mh_linkEdit(x, [&](mh::Links& links) { links.setMovement(on != 0); });
}

static void mh_linksready(t_mh_hands* x) {
  mh_linkEdit(x, [&](mh::Links& links) { links.setReady(true); });
}

static void mh_scale(t_mh_hands* x, t_symbol*, long argc, t_atom* argv) {
  std::vector<int> steps;
  for (long i = 0; i < argc; ++i) steps.push_back(static_cast<int>(atom_getlong(argv + i)));
  mh_edit(x, [&](mh::Params&, std::vector<int>& scaleSteps) { scaleSteps = steps; });
}

// Simple numeric settings: "<name> <value>".
static void mh_anything(t_mh_hands* x, t_symbol* s, long argc, t_atom* argv) {
  if (argc < 1) {
    object_error(reinterpret_cast<t_object*>(x), "%s needs a value", s->s_name);
    return;
  }
  const double v = atom_getfloat(argv);
  const int i = static_cast<int>(atom_getlong(argv));
  const std::string name = s->s_name;
  bool known = true;
  mh_edit(x, [&](mh::Params& p, std::vector<int>& scaleSteps) {
    if (name == "notes") p.notes = i != 0;
    else if (name == "hands") p.hands = std::clamp(i, 0, 2);
    else if (name == "layout") {
      p.layout = std::clamp(i, 0, 3);
      if (p.layout != mh::Custom) p.fingers = mh::presetSlots(p.layout);
    }
    else if (name == "octave") p.octave = std::clamp(i, -4, 4);
    else if (name == "velmode") p.velocityMode = std::clamp(i, 0, 2);
    else if (name == "velocity") p.velocity = std::clamp(i, 1, 127);
    else if (name == "sensitivity") p.sensitivity = static_cast<float>(std::clamp(v, 0.0, 1.0));
    else if (name == "minnote") p.minNoteMs = static_cast<float>(std::max(0.0, v));
    else if (name == "hold") p.holdMs = static_cast<float>(std::max(0.0, v));
    else if (name == "smoothing") p.smoothingMs = static_cast<float>(std::max(0.0, v));
    else if (name == "channel") p.channel = std::clamp(i, 1, 16) - 1;
    else if (name == "ccout") p.ccOut = i != 0;
    else if (name == "ccbase") p.ccBase = std::clamp(i, 0, 127 - mh::kExpr + 1);
    else if (name == "root") p.scale.root = ((i % 12) + 12) % 12;
    else if (name == "scaletype") {
      const auto& types = scaleTypes();
      scaleSteps = types[std::clamp(i, 0, static_cast<int>(types.size()) - 1)].steps;
    } else known = false;
  });
  if (!known) object_error(reinterpret_cast<t_object*>(x), "unknown message: %s", s->s_name);
}

static void mh_assist(t_mh_hands*, void*, long io, long index, char* text) {
  if (io == ASSIST_INLET) {
    std::snprintf(text, 256, "open, close, cameras, panic, finger, settings");
    return;
  }
  static const char* outlets[] = {"MIDI bytes (to midiout)", "Expressions: L height x pinch fist tilt, R ..., gestures, held",
                                  "Hand view data", "Info: cameras, status, stats, error, picture, fingers",
                                  "Links: link <k> value|attach|detach, read <k>, fxon <slot> <0|1>"};
  std::snprintf(text, 256, "%s", outlets[std::clamp<long>(index, 0, 4)]);
}

static void* mh_new(t_symbol*, long, t_atom*) {
  t_mh_hands* x = static_cast<t_mh_hands*>(object_alloc(mh_class));
  if (!x) return nullptr;
  x->outLinks = outlet_new(x, nullptr);
  x->outInfo = outlet_new(x, nullptr);
  x->outView = outlet_new(x, nullptr);
  x->outExpr = listout(x);
  x->outMidi = listout(x);
  x->flushClock = clock_new(x, reinterpret_cast<method>(mh_flush));
  x->linkClock = clock_new(x, reinterpret_cast<method>(mh_linkTick));
  x->linkStateQelem = qelem_new(x, reinterpret_cast<method>(mh_linkStateOut));
  x->viewQelem = qelem_new(x, reinterpret_cast<method>(mh_view));
  x->layoutQelem = qelem_new(x, reinterpret_cast<method>(mh_layout));
  x->accessQelem = qelem_new(x, reinterpret_cast<method>(mh_start));
  x->updateQelem = qelem_new(x, reinterpret_cast<method>(mh_updateOut));
  x->infoQelem = qelem_new(x, reinterpret_cast<method>(mh_infoOut));
  x->pendingCamera = gensym("");
  x->pendingCameraIndex = -1;
  x->subscription = 0;
  x->st = new State();
  std::lock_guard<std::mutex> lock(gAliveMutex);
  gAlive.insert(x);
  return x;
}

static void mh_free(t_mh_hands* x) {
  {
    std::lock_guard<std::mutex> lock(gAliveMutex);
    gAlive.erase(x);
  }
  // Deleted while recording its video window: finish the file anyway.
  if (gRecordingDevice == x) {
    gRecordingDevice = nullptr;
    mh::Recorder::shared().stop([](bool, const std::string&) {});
  }
  if (x->subscription) mh::CameraHub::shared().unsubscribe(x->subscription);  // no callbacks after this
  qelem_free(x->accessQelem);
  qelem_free(x->updateQelem);
  qelem_free(x->infoQelem);
  qelem_free(x->viewQelem);
  qelem_free(x->layoutQelem);
  qelem_free(x->linkStateQelem);
  object_free(x->linkClock);
  object_free(x->flushClock);
  // Notes still sounding are cut by Live when the device goes away.
  delete x->st;
}

void ext_main(void*) {
  t_class* c = class_new("mh.hands", reinterpret_cast<method>(mh_new), reinterpret_cast<method>(mh_free),
                         sizeof(t_mh_hands), nullptr, A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_open), "open", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_close), "close", 0);
  class_addmethod(c, reinterpret_cast<method>(mh_cameras), "cameras", 0);
  class_addmethod(c, reinterpret_cast<method>(mh_panic), "panic", 0);
  class_addmethod(c, reinterpret_cast<method>(mh_finger), "finger", A_GIMME, 0);
  for (const char* field : {"fingermode", "fingerdeg", "fingeroct"})
    class_addmethod(c, reinterpret_cast<method>(mh_fingerField), field, A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_report), "report", 0);
  class_addmethod(c, reinterpret_cast<method>(mh_checkupdate), "checkupdate", 0);
  class_addmethod(c, reinterpret_cast<method>(mh_update), "update", 0);
  class_addmethod(c, reinterpret_cast<method>(mh_record), "record", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_reveal), "reveal", 0);
  class_addmethod(c, reinterpret_cast<method>(mh_scale), "scale", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_linkset), "linkset", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_linktarget), "linktarget", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_linkvalue), "linkvalue", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_movement), "movement", A_LONG, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_fxset), "fxset", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_linksready), "linksready", 0);
  class_addmethod(c, reinterpret_cast<method>(mh_anything), "anything", A_GIMME, 0);
  class_addmethod(c, reinterpret_cast<method>(mh_assist), "assist", A_CANT, 0);
  class_register(CLASS_BOX, c);
  mh_class = c;
}
