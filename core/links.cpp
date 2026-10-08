#include "links.hpp"

#include <algorithm>
#include <cmath>

namespace mh {

namespace {
float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
float curveExponent(float curve) { return std::pow(2.f, curve / 50.f); }
}  // namespace

void Links::setParams(int k, const LinkParams& p) {
  if (k < 0 || k >= kLinks) return;
  const bool rebase = p.source != p_[k].source || p.lo != p_[k].lo || p.hi != p_[k].hi;
  const bool regrab = p.takeover == TakeoverGrab && p_[k].takeover != TakeoverGrab;
  const bool retake = p.takeover != p_[k].takeover;
  p_[k] = p;
  Run& r = run_[k];
  if (retake && r.phase == Waiting) r.phase = Idle;  // not attached yet: start over in the new mode
  // Grab follows how far the hand moves: a new movement or input range starts from here.
  if (rebase || regrab) r.prevIn = inputPos(k);
  if (regrab && r.sent >= 0.f) r.pos = inverse(k, r.sent);
}

void Links::setTarget(int k, long id) {
  if (k < 0 || k >= kLinks || target_[k] == id) return;
  target_[k] = id;
  run_[k].restart = true;
}

void Links::setMovement(bool on) { movement_ = on; }
void Links::setReady(bool ready) { ready_ = ready; }

void Links::parameterValue(int k, float value) {
  if (k < 0 || k >= kLinks || run_[k].phase != Reading) return;
  run_[k].answer = value < 0.f ? -1.f : clamp01(value);
  run_[k].answered = true;
}

void Links::setInput(const std::array<float, kExpr>& expr, const Gestures& gestures) {
  expr_ = expr;
  gestures_ = gestures;
}

bool Links::wants(int k) const {
  const LinkParams& p = p_[k];
  return ready_ && movement_ && p.on && target_[k] != 0 && engaged(p.engage, gestures_);
}

float Links::inputPos(int k) const {
  const LinkParams& p = p_[k];
  const float v = expr_[std::clamp(p.source, 0, kExpr - 1)];
  return (v - p.lo) / std::max(p.hi - p.lo, 0.001f);
}

float Links::outShape(int k, float pos) const {
  const LinkParams& p = p_[k];
  return p.min + (p.max - p.min) * std::pow(clamp01(pos), curveExponent(p.curve));
}

float Links::shape(int k, float movement) const {
  const LinkParams& p = p_[k];
  return outShape(k, (movement - p.lo) / std::max(p.hi - p.lo, 0.001f));
}

// Where on the curve a parameter value lies (values outside the output range
// sit at its nearest end).
float Links::inverse(int k, float value) const {
  const LinkParams& p = p_[k];
  if (std::fabs(p.max - p.min) < 1e-4f) return 0.f;
  return std::pow(clamp01((value - p.min) / (p.max - p.min)), 1.f / curveExponent(p.curve));
}

void Links::send(int k, float value, float rampMs, std::vector<LinkEvent>& out) {
  run_[k].sent = value;
  out.push_back({LinkEvent::Value, k, value, rampMs});
}

void Links::begin(int k, float current, std::vector<LinkEvent>& out) {
  Run& r = run_[k];
  const LinkParams& p = p_[k];
  const float target = shape(k, expr_[std::clamp(p.source, 0, kExpr - 1)]);
  // Without an answer the best guess is what this link last sent.
  const float known = current >= 0.f ? current : r.sent;
  r.start = known;
  if (p.takeover == TakeoverGrab && known >= 0.f) {
    r.pos = inverse(k, known);
    r.prevIn = inputPos(k);
    send(k, known, 0.f, out);
  } else if (p.takeover == TakeoverPickup && known >= 0.f && std::fabs(target - known) > 0.02f) {
    r.side = target > known ? 1.f : -1.f;
    r.phase = Waiting;
    return;
  } else {
    send(k, target, 0.f, out);
  }
  out.push_back({LinkEvent::Attach, k});
  r.phase = Moving;
}

void Links::release(int k, double now, std::vector<LinkEvent>& out) {
  Run& r = run_[k];
  if (r.phase != Moving) {
    r.phase = Idle;
    return;
  }
  if (p_[k].ret && r.start >= 0.f) {
    send(k, r.start, 0.f, out);
    r.phase = Releasing;  // let go once the old value has arrived
    r.at = now;
    return;
  }
  out.push_back({LinkEvent::Detach, k});
  r.phase = Idle;
}

void Links::update(double now, std::vector<LinkEvent>& out) {
  for (int k = 0; k < kLinks; ++k) {
    Run& r = run_[k];
    const LinkParams& p = p_[k];
    if (r.restart) {
      r.restart = false;
      if (r.phase == Moving || r.phase == Releasing) out.push_back({LinkEvent::Detach, k});
      r.phase = Idle;
      r.sent = -1.f;  // a new parameter: nothing sent to it yet
    }
    const bool want = wants(k);
    if (r.phase == Releasing && want) {
      // Engaged again while putting the old value back: start over from where it is.
      out.push_back({LinkEvent::Detach, k});
      r.phase = Idle;
    }
    switch (r.phase) {
      case Idle:
        if (!want) break;
        if (p.takeover != TakeoverJump || p.ret) {
          r.phase = Reading;
          r.at = now;
          r.answered = false;
          out.push_back({LinkEvent::Read, k});
        } else {
          begin(k, -1.f, out);
        }
        break;
      case Reading:
        if (!want) r.phase = Idle;
        else if (r.answered || now - r.at >= kReadTimeout) begin(k, r.answered ? r.answer : -1.f, out);
        break;
      case Waiting: {
        if (!want) {
          r.phase = Idle;
          break;
        }
        const float target = shape(k, expr_[std::clamp(p.source, 0, kExpr - 1)]);
        if (std::fabs(target - r.start) <= 0.02f || (target - r.start) * r.side <= 0.f) {
          send(k, target, 0.f, out);
          out.push_back({LinkEvent::Attach, k});
          r.phase = Moving;
        }
        break;
      }
      case Moving: {
        if (!want) {
          release(k, now, out);
          break;
        }
        float v;
        if (p.takeover == TakeoverGrab) {
          const float in = inputPos(k);
          r.pos = clamp01(r.pos + in - r.prevIn);
          r.prevIn = in;
          v = outShape(k, r.pos);
        } else {
          v = shape(k, expr_[std::clamp(p.source, 0, kExpr - 1)]);
        }
        // A parameter grabbed outside the link's output range glides into it.
        if (std::fabs(v - r.sent) > 1e-5f) send(k, v, std::fabs(v - r.sent) > 0.1f && p.takeover == TakeoverGrab ? 150.f : kRampMs, out);
        break;
      }
      case Releasing:
        if (now - r.at >= kReturnSettle) {
          out.push_back({LinkEvent::Detach, k});
          r.phase = Idle;
        }
        break;
    }
  }
}

double Links::deadline() const {
  double next = -1.0;
  for (const Run& r : run_) {
    double t = -1.0;
    if (r.phase == Reading) t = r.at + kReadTimeout;
    else if (r.phase == Releasing) t = r.at + kReturnSettle;
    if (t >= 0.0 && (next < 0.0 || t < next)) next = t;
  }
  return next;
}

int Links::state(int k) const {
  switch (run_[k].phase) {
    case Moving: return LinkMoving;
    case Reading:
    case Waiting: return LinkWaiting;
    default: return LinkIdle;
  }
}

}  // namespace mh
