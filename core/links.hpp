// Links: hand movements moving Live parameters, each engaged always or while
// a clutch gesture is held (clutch.hpp), and taking over the parameter
// without a jump.
//
// The device patch owns the Live side: per link a live.remote~ fed by a line~
// and the stored target id. Links decides what that patch does and says so as
// events: set the value, attach live.remote~ to the target, let go of it, or
// read the parameter's current value (answered with parameterValue()).
// Values are normalized 0..1, as live.remote~ @normalized 1 takes them.
#pragma once

#include <array>
#include <vector>

#include "clutch.hpp"
#include "engine.hpp"

namespace mh {

// What happens when a link engages and the hand is somewhere else than the parameter.
enum Takeover {
  TakeoverJump = 0,    // the parameter goes straight to the hand's value
  TakeoverGrab = 1,    // it stays, then moves by however far the hand moves
  TakeoverPickup = 2,  // it waits until the hand passes its value, then follows
};

struct LinkParams {
  bool on = true;
  int source = 0;        // Expr index
  float lo = 0.f;        // input range: the part of the movement used
  float hi = 1.f;
  float curve = 0.f;     // -100 .. 100, 0 = straight
  float min = 0.f;       // output range; min > max turns the parameter the other way
  float max = 1.f;
  int engage = kEngageAlways;  // or clutchFor(side, gesture)
  int takeover = TakeoverJump;
  bool ret = false;      // let go: put the parameter back where it was when engaged
};

struct LinkEvent {
  enum Kind { Value, Attach, Detach, Read };
  Kind kind;
  int link;
  float value = 0.f;   // Value: normalized parameter value
  float rampMs = 0.f;  // Value: glide time, 0 = at once
};

// What a link is doing, for the editor.
enum LinkState { LinkIdle = 0, LinkMoving = 1, LinkWaiting = 2 };

class Links {
 public:
  static constexpr int kLinks = 16;
  static constexpr double kReadTimeout = 0.3;   // no answer: carry on without it
  static constexpr double kReturnSettle = 0.1;  // Return: let the old value arrive before letting go
  static constexpr float kRampMs = 20.f;        // glide between camera frames

  void setParams(int k, const LinkParams& p);
  const LinkParams& params(int k) const { return p_[k]; }
  void setTarget(int k, long id);   // the linked Live parameter's id, 0 = none
  void setMovement(bool on);        // the device's Movement switch
  void setReady(bool ready);        // Live's API is up: live.remote~ may attach
  // Answer to a Read: the parameter's normalized value, < 0 when unknown.
  void parameterValue(int k, float value);

  // Latest movements and gestures (each camera frame), then work out the events.
  void setInput(const std::array<float, kExpr>& expr, const Gestures& gestures);
  void update(double now, std::vector<LinkEvent>& out);
  // When update() must run again without new input (read timeout, Return), or < 0.
  double deadline() const;

  int state(int k) const;

  // The link's response: movement value -> parameter value.
  float shape(int k, float movement) const;

 private:
  enum Phase { Idle, Reading, Waiting, Moving, Releasing };
  struct Run {
    Phase phase = Idle;
    float start = -1.f;  // the parameter's value when engaged (< 0: unknown)
    float sent = -1.f;   // last value sent
    float pos = 0.f;     // Grab: place on the curve, 0..1
    float prevIn = 0.f;  // Grab: last input position
    float side = 0.f;    // Pickup: which side of the parameter the hand started on
    double at = 0.0;     // Reading: asked at; Releasing: let go at
    float answer = -1.f;
    bool answered = false;
    bool restart = false;  // the target changed: let go of the old one first
  };

  bool wants(int k) const;
  void begin(int k, float current, std::vector<LinkEvent>& out);
  void release(int k, double now, std::vector<LinkEvent>& out);
  float inputPos(int k) const;  // unclamped place in the input range
  float outShape(int k, float pos) const;
  float inverse(int k, float value) const;
  void send(int k, float value, float rampMs, std::vector<LinkEvent>& out);

  std::array<LinkParams, kLinks> p_{};
  std::array<Run, kLinks> run_{};
  std::array<long, kLinks> target_{};
  bool movement_ = true;
  bool ready_ = false;
  std::array<float, kExpr> expr_{};
  Gestures gestures_{};
};

}  // namespace mh
