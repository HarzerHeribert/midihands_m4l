// MidiHands WebGL2 renderer: camera picture, effect chain (mh-fx.js) and hands.
//
// Shared by the editor (mh-editor.html) and the video window (mh-video.html).
// Image space everywhere: x right, y down, 0..1 across the camera picture,
// which arrives already mirrored, like the landmarks.
//
//   const r = MHGL.create(canvas);          // null without WebGL2
//   r.setSource(img);                       // <img crossOrigin="anonymous"> (MJPEG) or null
//   r.setHands([left, right]);              // each null or {pts: 21 x [x, y], on: [_, i, m, r, p]}
//   r.setExpr(tenValues);                   // 0..1, order of EXPR in the editor
//   r.setFx([{id, mix, p: [6 x 0..1]}]);    // effective values (after modulation)
//   Object.assign(r.options, {...});        // see DEFAULTS
//   r.render(seconds);
//   r.toCss([x, y]) -> [cssX, cssY]          // where an image point is drawn, for labels
"use strict";

(function () {
  const BONES = [[0,1],[1,2],[2,3],[3,4],[0,5],[5,6],[6,7],[7,8],[9,10],[10,11],[11,12],[13,14],[14,15],
    [15,16],[0,17],[17,18],[18,19],[19,20],[5,9],[9,13],[13,17]];
  const TIPS = [4, 8, 12, 16, 20];
  const REF_H = 540;  // the old app's look was tuned on a view about this tall

  const THEMES = {
    live:  { left: "#03c3d5", right: "#ffad56", accent: "#ffad56", highlight: "#f4fbff", track: "#3a3a3a", play: "#00d38d" },
    amber: { left: "#d0ad5d", right: "#ff962f", accent: "#e59b24", highlight: "#ffe094", track: "#5b4124", play: "#ffe094" },
  };
  const DEFAULTS = {
    look: "jelly",        // "jelly" | "lines" | "off"
    theme: "live",
    cues: [],             // expression indices to show gesture cues for (0..9)
    camera: 1,            // camera picture level 0..1 (0 = black, hands only)
    grade: "none",        // "play": desaturated and dimmed, for the editor's Play view
    fxOnHands: true,      // effects also apply to the drawn hands
    fit: "contain",       // "contain" (whole picture) | "cover" (fill, cropping)
    aspect: 0,            // output aspect; 0 = the camera's
    brackets: true,
    maxWidth: 1920,       // effect processing resolution limit
  };

  const hex = (h) => [parseInt(h.slice(1, 3), 16) / 255, parseInt(h.slice(3, 5), 16) / 255, parseInt(h.slice(5, 7), 16) / 255];
  const clamp01 = (v) => Math.max(0, Math.min(1, v));
  const mix3 = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];

  // ---- shaders ------------------------------------------------------------------------------
  // Pixel positions (y down) to clip space; u_yFlip is 1 on screen, -1 into textures so that
  // row 0 of every texture is the top of the picture.
  const TO_CLIP = `vec4 toClip(vec2 p, vec2 res) { return vec4(p.x / res.x * 2.0 - 1.0, u_yFlip * (1.0 - p.y / res.y * 2.0), 0.0, 1.0); }`;

  const QUAD_VERT = `#version 300 es
precision highp float;
in vec2 a_pos;            // 0..1 over the destination rectangle
uniform vec4 u_dst;       // x, y, w, h in target pixels (y down)
uniform vec4 u_src;       // u, v, w, h in source uv (v down)
uniform vec2 u_res;
uniform float u_yFlip;
out vec2 v_uv;
out vec2 v_local;
${TO_CLIP}
void main() {
  v_local = a_pos;
  v_uv = u_src.xy + a_pos * u_src.zw;
  gl_Position = toClip(u_dst.xy + a_pos * u_dst.zw, u_res);
}`;

  const CAMERA_FRAG = `#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_level, u_desat, u_hasTex;
out vec4 o;
void main() {
  vec3 c = u_hasTex > 0.5 ? texture(u_tex, v_uv).rgb : vec3(0.0);
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
  c = mix(c, vec3(l), u_desat);
  o = vec4(c * u_level, 1.0);
}`;

  const COPY_FRAG = `#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 o;
void main() { o = vec4(texture(u_tex, v_uv).rgb, 1.0); }`;

  const MIX_FRAG = `#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_base, u_fx;
uniform float u_mix;
out vec4 o;
void main() { o = vec4(mix(texture(u_base, v_uv).rgb, texture(u_fx, v_uv).rgb, u_mix), 1.0); }`;

  // The hand body ("Jelly"): slim tapered fingers smoothly joined to a glassy palm,
  // as a signed distance field. Sized from the hand itself (knuckle spacing), so it
  // stays slimmer than the real fingers at any distance from the camera.
  const FIELD_VERT = `#version 300 es
precision highp float;
in vec2 a_pos;
uniform vec4 u_rect;
uniform vec2 u_res;
uniform float u_yFlip;
out vec2 v_px;
${TO_CLIP}
void main() {
  v_px = u_rect.xy + a_pos * u_rect.zw;
  gl_Position = toClip(v_px, u_res);
}`;

  const FIELD_FRAG = `#version 300 es
precision highp float;
#define JOINTS 21
in vec2 v_px;
uniform vec2 u_joint[JOINTS];
uniform float u_jointR[JOINTS];
uniform float u_jointOn[JOINTS];
uniform float u_palmR, u_k, u_R;
uniform vec3 u_color, u_highlight, u_play;
uniform float u_alpha, u_time, u_phase;
out vec4 o;
// Fingers: thumb from the wrist, then index to pinky from their knuckles.
const int BA[16] = int[16](0, 1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15, 17, 18, 19);
const int BB[16] = int[16](1, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15, 16, 18, 19, 20);
const int PALM[6] = int[6](0, 1, 5, 9, 13, 17);
float cro(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
// A capsule whose radius changes from ra to rb (Inigo Quilez).
float taper(vec2 p, vec2 pa, vec2 pb, float ra, float rb) {
  p -= pa; pb -= pa;
  float h = dot(pb, pb);
  if (h < 1.0) return length(p) - max(ra, rb);
  vec2 q = vec2(dot(p, vec2(pb.y, -pb.x)), dot(p, pb)) / h;
  q.x = abs(q.x);
  float b = ra - rb;
  vec2 c = vec2(sqrt(max(h - b * b, 1e-3)), b);
  float k = cro(c, q), m = dot(c, q), n = dot(q, q);
  if (k < 0.0) return sqrt(h * n) - ra;
  if (k > c.x) return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;
  return m - ra;
}
float palm(vec2 p) {
  vec2 v0 = u_joint[PALM[0]];
  float d = dot(p - v0, p - v0), s = 1.0;
  int j = 5;
  for (int i = 0; i < 6; i++) {
    vec2 vi = u_joint[PALM[i]], vj = u_joint[PALM[j]];
    vec2 e = vj - vi, w = p - vi;
    vec2 b = w - e * clamp(dot(w, e) / max(dot(e, e), 1e-3), 0.0, 1.0);
    d = min(d, dot(b, b));
    bvec3 c = bvec3(p.y >= vi.y, p.y < vj.y, e.x * w.y > e.y * w.x);
    if (all(c) || all(not(c))) s = -s;
    j = i;
  }
  return s * sqrt(d);
}
float smin(float a, float b, float k) { float h = max(k - abs(a - b), 0.0) / k; return min(a, b) - h * h * k * 0.25; }
// x: distance (px, negative inside), y: how much a playing finger tints this point.
// The segments of a finger join plainly (no bulging joints); only fingers and palm blend.
vec2 scene(vec2 p) {
  float d = palm(p) - u_palmR * 0.2, on = 0.0;
  int bone = 0;
  for (int f = 0; f < 5; f++) {
    int count = f == 0 ? 4 : 3;
    float df = 1e9;
    for (int k = 0; k < 4; k++) {
      if (k >= count) break;
      int a = BA[bone], b = BB[bone];
      df = min(df, taper(p, u_joint[a], u_joint[b], u_jointR[a], u_jointR[b]));
      bone++;
    }
    on = max(on, u_jointOn[BB[bone - 1]] * (1.0 - smoothstep(-2.0, u_k, df)));
    d = smin(d, df, u_k);
  }
  return vec2(d, on);
}
void main() {
  vec2 s = scene(v_px);
  float body = clamp(0.5 - s.x, 0.0, 1.0);             // 1 px soft edge
  if (body <= 0.0) discard;
  float R = max(u_R, 2.0);                              // one thickness for the whole hand: no seams
  float t = clamp(-s.x / R, 0.0, 1.0);                  // 0 at the rim, 1 deep inside
  // A rounded profile: the normal tilts outwards towards the rim.
  float e = max(1.0, R * 0.12);
  vec2 g = vec2(scene(v_px + vec2(e, 0.0)).x - scene(v_px - vec2(e, 0.0)).x,
                scene(v_px + vec2(0.0, e)).x - scene(v_px - vec2(0.0, e)).x) / (2.0 * e);
  float u = 1.0 - t;
  float slope = u / max(sqrt(1.0 - u * u), 0.12);
  vec3 n = normalize(vec3(g * min(slope, 6.0), 1.0));
  vec3 L = normalize(vec3(-0.45, -0.55, 0.7));
  float diffuse = max(dot(n, L), 0.0);
  float spec = pow(max(dot(reflect(-L, n), vec3(0.0, 0.0, 1.0)), 0.0), 36.0);
  float fresnel = pow(1.0 - n.z, 2.0);
  float sheen = 0.75 + 0.25 * sin(dot(v_px, vec2(0.018, 0.031)) - u_time * 1.3 + u_phase * 6.28318);
  vec3 base = mix(u_color, u_play, s.y * 0.7);
  vec3 c = base * (0.5 + 0.6 * diffuse) + u_highlight * (spec * 0.85 * sheen + fresnel * 0.3);
  // Glass: clear inside, solid at the rim; a playing finger fills up.
  float rim = 1.0 - smoothstep(0.0, 0.45, t);
  float a = u_alpha * body * mix(0.72, 1.0, max(rim, s.y * 0.6));
  a = clamp(a + spec * 0.3, 0.0, 1.0);
  o = vec4(c * a, a);
}`;

  // Gel lines (bones, pinch band, sliders), from the old app.
  const LINE_VERT = `#version 300 es
precision highp float;
in vec2 a_p;     // pixel position
in vec2 a_n;     // x: 0..1 along, y: -1..1 across
in vec4 a_c;
uniform vec2 u_res;
uniform float u_yFlip;
out vec4 v_c;
out float v_across, v_along;
${TO_CLIP}
void main() { gl_Position = toClip(a_p, u_res); v_c = a_c; v_along = a_n.x; v_across = a_n.y; }`;

  const LINE_FRAG = `#version 300 es
precision highp float;
in vec4 v_c;
in float v_across, v_along;
uniform vec3 u_highlight;
uniform float u_time;
out vec4 o;
void main() {
  float d = abs(v_across);
  float pw = fwidth(d);
  float ends = max(exp(-pow(v_along * 5.5, 2.0)), exp(-pow((1.0 - v_along) * 5.5, 2.0)));
  float shaped = d / (0.72 + ends * 0.42);
  float body = 1.0 - smoothstep(1.0 - pw * 2.0, 1.0, shaped);
  if (body < 0.001) discard;
  float rail = 1.0 - smoothstep(0.38, 0.84 + pw * 2.0, shaped);
  float core = 1.0 - smoothstep(0.0, 0.42 + pw * 2.0, shaped);
  float sweepC = fract(u_time * 0.16 + v_c.a * 0.18);
  float sd = abs(v_along - sweepC); sd = min(sd, 1.0 - sd);
  float sweep = exp(-pow(sd * 6.0, 2.0)) * rail;
  vec3 c = mix(v_c.rgb, u_highlight, core * 0.12 + sweep * 0.18);
  float a = v_c.a * body;
  o = vec4(c * a, a);
}`;

  // Soft round joints for the line look, and 270-degree gauges for fist and tilt.
  const DOT_VERT = `#version 300 es
precision highp float;
in vec2 a_pos;
uniform vec3 u_dot;   // x, y, radius (px)
uniform vec2 u_res;
uniform float u_yFlip;
out vec2 v_uv;
${TO_CLIP}
void main() { v_uv = a_pos * 2.0 - 1.0; gl_Position = toClip(u_dot.xy + v_uv * (u_dot.z + 2.0), u_res); }`;

  const DOT_FRAG = `#version 300 es
precision highp float;
in vec2 v_uv;
uniform vec4 u_c;
uniform vec3 u_highlight;
uniform float u_ring;       // 0 = filled dot; >0 = gauge with that track width (in uv units)
uniform float u_value;
uniform vec3 u_track;
out vec4 o;
const float PI = 3.14159265;
void main() {
  float r = length(v_uv);
  float pw = fwidth(r);
  if (u_ring <= 0.0) {
    float a = (1.0 - smoothstep(0.86 - pw, 0.86 + pw, r)) * u_c.a;
    if (a < 0.001) discard;
    vec3 c = mix(u_c.rgb, u_highlight, (1.0 - smoothstep(0.0, 0.5, r)) * 0.35);
    o = vec4(c * a, a);
    return;
  }
  float ang = atan(-v_uv.y, v_uv.x);              // y down: flip for a clockwise dial
  float t = mod(PI * 1.25 - ang, 2.0 * PI) / (PI * 1.5);
  float band = smoothstep(0.86 - u_ring - pw, 0.86 - u_ring + pw, r) * (1.0 - smoothstep(0.86 - pw, 0.86 + pw, r));
  if (t > 1.0 || band < 0.001) discard;
  float fill = step(t, u_value);
  float cap = exp(-pow((t - u_value) * 30.0, 2.0)) * fill;
  vec3 c = mix(u_track, u_c.rgb, fill);
  c = mix(c, u_highlight, cap * 0.6);
  float a = band * mix(0.55, 1.0, fill) * u_c.a;
  o = vec4(c * a, a);
}`;

  // ---- GL helpers ------------------------------------------------------------------------------
  function program(gl, vs, fs, name) {
    const make = (type, src) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, src); gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(`${name}: ${gl.getShaderInfoLog(s)}`);
      return s;
    };
    const p = gl.createProgram();
    gl.attachShader(p, make(gl.VERTEX_SHADER, vs)); gl.attachShader(p, make(gl.FRAGMENT_SHADER, fs));
    gl.bindAttribLocation(p, 0, "a_pos"); gl.bindAttribLocation(p, 0, "a_p");
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`${name}: ${gl.getProgramInfoLog(p)}`);
    const u = {};
    const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < n; i++) {
      const info = gl.getActiveUniform(p, i), key = info.name.replace(/\[0\]$/, "");
      u[key] = gl.getUniformLocation(p, info.name);
    }
    return { p, u };
  }

  function texture(gl, w, h, format) {
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    const f = format === "float" ? [gl.RGBA16F, gl.RGBA, gl.HALF_FLOAT] : [gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE];
    gl.texImage2D(gl.TEXTURE_2D, 0, f[0], w, h, 0, f[1], f[2], null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return t;
  }

  function target(gl, w, h, format) {
    const tex = texture(gl, w, h, format), fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { tex, fb, w, h, format };
  }

  function create(canvas) {
    const gl = canvas.getContext("webgl2", { alpha: false, antialias: false, premultipliedAlpha: true, preserveDrawingBuffer: false });
    if (!gl) return null;
    const floatOk = !!gl.getExtension("EXT_color_buffer_float");

    // One unit quad for everything, and a dynamic buffer for gel lines.
    const quad = gl.createVertexArray(), quadBuf = gl.createBuffer();
    gl.bindVertexArray(quad);
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    const lineVao = gl.createVertexArray(), lineBuf = gl.createBuffer();
    gl.bindVertexArray(lineVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, lineBuf);
    for (const [loc, size, off] of [[0, 2, 0], [1, 2, 8], [2, 4, 16]]) {
      gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, size, gl.FLOAT, false, 32, off);
    }
    gl.bindVertexArray(null);

    const progs = {
      camera: program(gl, QUAD_VERT, CAMERA_FRAG, "camera"),
      copy: program(gl, QUAD_VERT, COPY_FRAG, "copy"),
      mix: program(gl, QUAD_VERT, MIX_FRAG, "mix"),
      field: program(gl, FIELD_VERT, FIELD_FRAG, "hand field"),
      dot: program(gl, DOT_VERT, DOT_FRAG, "dot"),
    };
    // The line program uses three attributes.
    {
      const p = program(gl, LINE_VERT, LINE_FRAG, "lines");
      gl.bindAttribLocation(p.p, 0, "a_p"); gl.bindAttribLocation(p.p, 1, "a_n"); gl.bindAttribLocation(p.p, 2, "a_c");
      gl.linkProgram(p.p);
      for (const k of Object.keys(p.u)) p.u[k] = gl.getUniformLocation(p.p, k);
      progs.line = p;
    }
    const fxPrograms = {};   // compiled on first use: id -> program(s)

    const camTex = texture(gl, 2, 2);
    let source = null, sourceOk = false;
    let hands = [null, null], expr = new Array(10).fill(0), fx = [];
    const targets = {};      // name -> {tex, fb, w, h}
    const state = {};        // per-effect-slot memory (feedback buffers etc.)
    let lastTime = 0, contentCss = { x: 0, y: 0, w: 1, h: 1 }, crop = { x: 0, y: 0, w: 1, h: 1 };
    const r = { options: Object.assign({}, DEFAULTS), floatOk, gl };

    const need = (name, w, h, format) => {
      const t = targets[name];
      if (t && t.w === w && t.h === h && t.format === format) return t;
      if (t) { gl.deleteTexture(t.tex); gl.deleteFramebuffer(t.fb); }
      return (targets[name] = target(gl, w, h, format));
    };

    // Draw `prog` over a target rectangle. dst: target or null (screen) and rect in its pixels.
    function blit(prog, uniforms, textures, dst, rect, src) {
      const w = dst ? dst.w : gl.drawingBufferWidth, h = dst ? dst.h : gl.drawingBufferHeight;
      gl.bindFramebuffer(gl.FRAMEBUFFER, dst ? dst.fb : null);
      gl.viewport(0, 0, w, h);
      gl.useProgram(prog.p);
      const R = rect || [0, 0, w, h], S = src || [0, 0, 1, 1];
      gl.uniform4f(prog.u.u_dst, R[0], R[1], R[2], R[3]);
      gl.uniform4f(prog.u.u_src, S[0], S[1], S[2], S[3]);
      gl.uniform2f(prog.u.u_res, w, h);
      gl.uniform1f(prog.u.u_yFlip, dst ? -1 : 1);
      let unit = 0;
      for (const [name, tex] of Object.entries(textures || {})) {
        gl.activeTexture(gl.TEXTURE0 + unit);
        if (tex && tex.array) gl.bindTexture(gl.TEXTURE_2D_ARRAY, tex.array);
        else gl.bindTexture(gl.TEXTURE_2D, tex);
        if (prog.u[name]) gl.uniform1i(prog.u[name], unit);
        unit++;
      }
      for (const [name, v] of Object.entries(uniforms || {})) setUniform(prog, name, v);
      gl.bindVertexArray(quad);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
    }
    function setUniform(prog, name, v) {
      const loc = prog.u[name];
      if (loc === undefined || loc === null) return;
      if (typeof v === "number") gl.uniform1f(loc, v);
      else if (v.length === 2) gl.uniform2f(loc, v[0], v[1]);
      else if (v.length === 3) gl.uniform3f(loc, v[0], v[1], v[2]);
      else if (v.length === 4) gl.uniform4f(loc, v[0], v[1], v[2], v[3]);
      else gl.uniform1fv(loc, v);
    }

    // ---- hands --------------------------------------------------------------------------------
    // `map` turns an image point into target pixels; `unit` is pixels per reference pixel.
    function drawHands(dst, map, unit, time, theme) {
      const look = r.options.look;
      if (look === "off") return;
      const w = dst ? dst.w : gl.drawingBufferWidth, h = dst ? dst.h : gl.drawingBufferHeight;
      gl.bindFramebuffer(gl.FRAMEBUFFER, dst ? dst.fb : null);
      gl.viewport(0, 0, w, h);
      gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      const lines = [];
      for (let s = 0; s < 2; s++) {
        const hand = hands[s]; if (!hand) continue;
        const pts = hand.pts.map(map);
        const color = hex(s ? theme.right : theme.left), hi = hex(theme.highlight);
        const fv = (i) => { const f = i <= 4 ? 0 : Math.floor((i - 1) / 4); return f > 0 && hand.on[f] ? 1 : 0; };
        const phase = s ? 0.58 : 0.12;  // the two hands shimmer out of step
        if (look === "jelly") {
          // Thickness from the hand itself: the spacing of the four knuckles.
          const dist = (i, j) => Math.hypot(pts[i][0] - pts[j][0], pts[i][1] - pts[j][1]);
          const sp = Math.max((dist(5, 9) + dist(9, 13) + dist(13, 17)) / 3, dist(0, 9) * 0.22, 3);
          const joints = new Float32Array(42), radii = new Float32Array(21), on = new Float32Array(21);
          // Wrist, thumb (CMC, MCP, IP, tip), then per finger: knuckle, middle joints, tip.
          const thumb = [0.42, 0.36, 0.3, 0.26, 0.22], finger = [0.3, 0.26, 0.23, 0.2];
          let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
          for (let i = 0; i < 21; i++) {
            const f = i <= 4 ? 0 : Math.floor((i - 1) / 4);
            let rad = (i <= 4 ? thumb[i] : finger[(i - 1) % 4]) * sp;
            if (f === 4) rad *= 0.88;                                  // pinky
            on[i] = fv(i);
            if (on[i]) rad *= 1.1;
            joints[i * 2] = pts[i][0]; joints[i * 2 + 1] = pts[i][1]; radii[i] = rad;
            x0 = Math.min(x0, pts[i][0] - rad); y0 = Math.min(y0, pts[i][1] - rad);
            x1 = Math.max(x1, pts[i][0] + rad); y1 = Math.max(y1, pts[i][1] + rad);
          }
          const pad = 0.3 * sp + 3;
          const prog = progs.field;
          gl.useProgram(prog.p);
          gl.uniform4f(prog.u.u_rect, x0 - pad, y0 - pad, x1 - x0 + pad * 2, y1 - y0 + pad * 2);
          gl.uniform2f(prog.u.u_res, w, h);
          gl.uniform1f(prog.u.u_yFlip, dst ? -1 : 1);
          gl.uniform2fv(prog.u.u_joint, joints); gl.uniform1fv(prog.u.u_jointR, radii); gl.uniform1fv(prog.u.u_jointOn, on);
          gl.uniform1f(prog.u.u_palmR, 0.5 * sp); gl.uniform1f(prog.u.u_k, 0.22 * sp); gl.uniform1f(prog.u.u_R, 0.3 * sp);
          gl.uniform3fv(prog.u.u_color, color); gl.uniform3fv(prog.u.u_highlight, hi); gl.uniform3fv(prog.u.u_play, hex(theme.play));
          gl.uniform1f(prog.u.u_alpha, 0.92); gl.uniform1f(prog.u.u_time, time);
          gl.uniform1f(prog.u.u_phase, phase);
          gl.bindVertexArray(quad); gl.drawArrays(gl.TRIANGLES, 0, 6);
        } else {
          for (const [a, b] of BONES) {
            const on = fv(b) > 0;
            line(lines, pts[a], pts[b], (on ? 6.5 : 4.2) * unit, on ? mix3(color, hex(theme.play), 0.75) : color, on ? 0.95 : 0.85);
          }
        }
        // Gesture cues for movements in use.
        const cues = r.options.cues;
        const palmC = [0, 5, 9, 13, 17].reduce((a, i) => [a[0] + pts[i][0] / 5, a[1] + pts[i][1] / 5], [0, 0]);
        const accent = hex(theme.accent);
        if (cues.includes(s * 5 + 2)) {   // pinch: a band from thumb to index
          const v = clamp01(expr[s * 5 + 2]);
          line(lines, pts[4], pts[8], (2 + v * 7) * unit, mix3(accent, hi, 0.2), 0.35 + v * 0.5);
        }
        if (cues.includes(s * 5 + 0)) {   // height: a vertical slider beside the palm
          const x = palmC[0] + (s ? 1 : -1) * 64 * unit, top = palmC[1] - 60 * unit, bot = palmC[1] + 60 * unit;
          line(lines, [x, top], [x, bot], 2.4 * unit, hex(theme.track), 0.8);
          const y = bot + (top - bot) * clamp01(expr[s * 5]);
          line(lines, [x - 9 * unit, y], [x + 9 * unit, y], 4 * unit, accent, 0.95);
        }
        if (cues.includes(s * 5 + 1)) {   // x: a horizontal slider under the wrist
          const y = pts[0][1] + 34 * unit, l = palmC[0] - 60 * unit, rr = palmC[0] + 60 * unit;
          line(lines, [l, y], [rr, y], 2.4 * unit, hex(theme.track), 0.8);
          const x = l + (rr - l) * clamp01(expr[s * 5 + 1]);
          line(lines, [x, y - 9 * unit], [x, y + 9 * unit], 4 * unit, accent, 0.95);
        }
        flushLines(lines, dst, w, h, time, hi); lines.length = 0;
        if (look === "lines") for (let i = 0; i < 21; i++) {
          const on = fv(i) > 0, big = TIPS.includes(i) ? 7 : 5;
          dot(dst, w, h, pts[i], (on && TIPS.includes(i) ? 10 : big) * unit, on ? hex(theme.play) : color, 1, hi);
        }
        let ring = 0;
        for (const k of [3, 4]) {          // fist and tilt: gauges around the palm
          if (!cues.includes(s * 5 + k)) continue;
          gauge(dst, w, h, palmC, (46 + ring * 14) * unit, clamp01(expr[s * 5 + k]), k === 3 ? accent : color, hex(theme.track), hi, 0.16);
          ring++;
        }
      }
      gl.disable(gl.BLEND);
    }
    function line(out, a, b, width, c, alpha) {
      const dx = b[0] - a[0], dy = b[1] - a[1], len = Math.hypot(dx, dy) || 1;
      const nx = -dy / len * width / 2, ny = dx / len * width / 2;
      const v = (p, along, across) => out.push(p[0] + nx * across, p[1] + ny * across, along, across, c[0], c[1], c[2], alpha);
      v(a, 0, -1); v(b, 1, -1); v(a, 0, 1); v(a, 0, 1); v(b, 1, -1); v(b, 1, 1);
    }
    function flushLines(data, dst, w, h, time, hi) {
      if (!data.length) return;
      const prog = progs.line;
      gl.useProgram(prog.p);
      gl.uniform2f(prog.u.u_res, w, h); gl.uniform1f(prog.u.u_yFlip, dst ? -1 : 1);
      gl.uniform3fv(prog.u.u_highlight, hi); gl.uniform1f(prog.u.u_time, time);
      gl.bindVertexArray(lineVao);
      gl.bindBuffer(gl.ARRAY_BUFFER, lineBuf);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.STREAM_DRAW);
      gl.drawArrays(gl.TRIANGLES, 0, data.length / 8);
    }
    function dot(dst, w, h, p, radius, c, alpha, hi) {
      const prog = progs.dot;
      gl.useProgram(prog.p);
      gl.uniform3f(prog.u.u_dot, p[0], p[1], radius); gl.uniform2f(prog.u.u_res, w, h);
      gl.uniform1f(prog.u.u_yFlip, dst ? -1 : 1); gl.uniform4f(prog.u.u_c, c[0], c[1], c[2], alpha);
      gl.uniform3fv(prog.u.u_highlight, hi); gl.uniform1f(prog.u.u_ring, 0);
      gl.bindVertexArray(quad); gl.drawArrays(gl.TRIANGLES, 0, 6);
    }
    function gauge(dst, w, h, p, radius, value, c, track, hi, ring) {
      const prog = progs.dot;
      gl.useProgram(prog.p);
      gl.uniform3f(prog.u.u_dot, p[0], p[1], radius); gl.uniform2f(prog.u.u_res, w, h);
      gl.uniform1f(prog.u.u_yFlip, dst ? -1 : 1); gl.uniform4f(prog.u.u_c, c[0], c[1], c[2], 0.95);
      gl.uniform3fv(prog.u.u_highlight, hi); gl.uniform1f(prog.u.u_ring, ring);
      gl.uniform1f(prog.u.u_value, value); gl.uniform3fv(prog.u.u_track, track);
      gl.bindVertexArray(quad); gl.drawArrays(gl.TRIANGLES, 0, 6);
    }
    function brackets(dst, rect, unit, theme) {
      const w = dst ? dst.w : gl.drawingBufferWidth, h = dst ? dst.h : gl.drawingBufferHeight;
      gl.bindFramebuffer(gl.FRAMEBUFFER, dst ? dst.fb : null); gl.viewport(0, 0, w, h);
      gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      const out = [], L = 18 * unit, m = 10 * unit, c = hex(theme.accent), [x, y, rw, rh] = rect;
      for (const [cx, cy, sx, sy] of [[x + m, y + m, 1, 1], [x + rw - m, y + m, -1, 1], [x + m, y + rh - m, 1, -1], [x + rw - m, y + rh - m, -1, -1]]) {
        line(out, [cx, cy], [cx + L * sx, cy], 1.6 * unit, c, 0.45);
        line(out, [cx, cy], [cx, cy + L * sy], 1.6 * unit, c, 0.45);
      }
      flushLines(out, dst, w, h, 0, hex(theme.highlight));
      gl.disable(gl.BLEND);
    }

    // ---- effects --------------------------------------------------------------------------------
    function effectProgram(effect) {
      if (!fxPrograms[effect.id]) {
        fxPrograms[effect.id] = {};
        const frags = typeof effect.frag === "string" ? { main: effect.frag } : effect.frag;
        for (const [k, src] of Object.entries(frags)) fxPrograms[effect.id][k] = program(gl, QUAD_VERT, src, `effect ${effect.id}.${k}`);
      }
      return fxPrograms[effect.id];
    }

    function runChain(input, w, h, time, dt) {
      let current = input;
      const anchors = handAnchors();
      fx.forEach((slot, i) => {
        const effect = window.MHFX && window.MHFX.byId[slot.id];
        if (!effect || slot.mix <= 0.001) {
          if (state[i] && state[i].id !== slot.id) delete state[i];
          return;
        }
        if (!state[i] || state[i].id !== slot.id) state[i] = { id: slot.id };
        const out = need(`fx${i}`, w, h, "rgba8");
        const ctx = {
          gl, w, h, time, dt, p: slot.p, anchors, expr, floatOk, mem: state[i],
          prog: effectProgram(effect),
          target: (name, tw, th, format) => need(`fx${i}.${name}`, tw || w, th || h, format || "rgba8"),
          pass: (prog, uniforms, textures, dst) => blit(prog, Object.assign({ u_size: [dst ? dst.w : w, dst ? dst.h : h], u_time: time }, uniforms), textures, dst),
          copy: (src, dst) => blit(progs.copy, {}, { u_tex: src.tex }, dst),
          clear: (dst, rgba) => {
            gl.bindFramebuffer(gl.FRAMEBUFFER, dst.fb); gl.viewport(0, 0, dst.w, dst.h);
            gl.clearColor(rgba[0], rgba[1], rgba[2], rgba[3]); gl.clear(gl.COLOR_BUFFER_BIT);
          },
        };
        effect.run(ctx, current, out);
        if (slot.mix < 0.999) {
          const mixed = need(`fx${i}.mix`, w, h, "rgba8");
          blit(progs.mix, { u_mix: slot.mix }, { u_base: current.tex, u_fx: out.tex }, mixed);
          current = mixed;
        } else current = out;
      });
      return current;
    }

    // Hand centers and sizes in image space, for effects that follow the hands.
    function handAnchors() {
      const a = [];
      for (const hand of hands) {
        if (!hand) continue;
        const p = hand.pts, cx = (p[0][0] + p[9][0]) / 2, cy = (p[0][1] + p[9][1]) / 2;
        const size = Math.max(Math.hypot(p[5][0] - p[17][0], p[5][1] - p[17][1]), Math.hypot(p[0][0] - p[9][0], p[0][1] - p[9][1]));
        // Into the cropped picture's space (what the effects see).
        a.push([(cx - crop.x) / crop.w, (cy - crop.y) / crop.h, Math.max(0.06, Math.min(0.35, size * 1.35 / crop.h))]);
      }
      return a;
    }

    // ---- frame ---------------------------------------------------------------------------------
    r.setSource = (img) => { source = img; sourceOk = false; };
    r.setHands = (h) => { hands = h; };
    r.setExpr = (e) => { expr = e; };
    r.setFx = (slots) => { fx = slots || []; };
    r.toCss = (p) => [contentCss.x + (p[0] - crop.x) / crop.w * contentCss.w, contentCss.y + (p[1] - crop.y) / crop.h * contentCss.h];
    r.content = () => Object.assign({}, contentCss);

    r.render = (time) => {
      const o = r.options, theme = THEMES[o.theme] || THEMES.live;
      const dpr = window.devicePixelRatio || 1;
      const cw = Math.max(1, Math.round(canvas.clientWidth * dpr)), ch = Math.max(1, Math.round(canvas.clientHeight * dpr));
      if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
      const dt = Math.min(0.1, Math.max(0, time - lastTime)); lastTime = time;

      // Camera picture into a texture (the newest MJPEG frame).
      let camAspect = 16 / 9, hasTex = 0;
      if (source && source.complete && source.naturalWidth > 0) {
        gl.bindTexture(gl.TEXTURE_2D, camTex);
        try {
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
          hasTex = 1; sourceOk = true;
        } catch (e) { sourceOk = false; }
        camAspect = source.naturalWidth / source.naturalHeight;
      }
      // Where the picture goes: the output aspect fitted into the canvas.
      const aspect = o.aspect > 0 ? o.aspect : camAspect;
      let dw = cw, dh = cw / aspect;
      if (dh > ch) { dh = ch; dw = ch * aspect; }
      const dx = (cw - dw) / 2, dy = (ch - dh) / 2;
      contentCss = { x: dx / dpr, y: dy / dpr, w: dw / dpr, h: dh / dpr };
      // Which part of the camera picture fills it (cover crops, contain shows all).
      crop = { x: 0, y: 0, w: 1, h: 1 };
      if (Math.abs(aspect - camAspect) > 0.01) {
        if (aspect > camAspect) { crop.h = camAspect / aspect; crop.y = (1 - crop.h) / 2; }
        else { crop.w = aspect / camAspect; crop.x = (1 - crop.w) / 2; }
      }

      // Effects run at the output's resolution, capped.
      const scale = Math.min(1, o.maxWidth / dw);
      const iw = Math.max(2, Math.round(dw * scale)), ih = Math.max(2, Math.round(dh * scale));
      const base = need("base", iw, ih, "rgba8");
      blit(progs.camera, { u_level: o.camera, u_desat: o.grade === "play" ? 1 : 0, u_hasTex: hasTex },
        { u_tex: camTex }, base, null, [crop.x, crop.y, crop.w, crop.h]);
      const mapInto = (W, H, ox, oy) => (p) => [ox + (p[0] - crop.x) / crop.w * W, oy + (p[1] - crop.y) / crop.h * H];
      const unitIn = ih / REF_H;
      if (o.fxOnHands) drawHands(base, mapInto(iw, ih, 0, 0), unitIn, time, theme);
      const result = fx.length ? runChain(base, iw, ih, time, dt) : base;

      // To the screen.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, cw, ch);
      gl.clearColor(0, 0, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT);
      blit(progs.copy, {}, { u_tex: result.tex }, null, [dx, dy, dw, dh]);
      const unitOut = dh / REF_H;
      if (!o.fxOnHands) drawHands(null, mapInto(dw, dh, dx, dy), unitOut, time, theme);
      if (o.brackets) brackets(null, [dx, dy, dw, dh], unitOut, theme);
      return sourceOk;
    };

    return r;
  }

  // A colorful synthetic picture for demos and README renderings (no camera).
  function demoScene() {
    const c = document.createElement("canvas"); c.width = 1280; c.height = 720;
    const g = c.getContext("2d"), sky = g.createLinearGradient(0, 0, 0, 720);
    sky.addColorStop(0, "#1d1b4f"); sky.addColorStop(0.55, "#b8457a"); sky.addColorStop(1, "#f4a259");
    g.fillStyle = sky; g.fillRect(0, 0, 1280, 720);
    for (let i = 0; i < 40; i++) {
      const x = (i * 337) % 1280, y = (i * 211) % 420, r = 10 + (i * 17) % 50, b = g.createRadialGradient(x, y, 0, x, y, r);
      b.addColorStop(0, `hsla(${(i * 47) % 360},90%,75%,.55)`); b.addColorStop(1, "hsla(0,0%,100%,0)");
      g.fillStyle = b; g.beginPath(); g.arc(x, y, r, 0, 7); g.fill();
    }
    g.fillStyle = "#14102a"; g.beginPath(); g.moveTo(0, 560);
    for (let x = 0; x <= 1280; x += 40) g.lineTo(x, 520 - Math.sin(x * 0.01) * 40 - (x % 160 === 0 ? 70 : 0));
    g.lineTo(1280, 720); g.lineTo(0, 720); g.fill();
    return c.toDataURL();
  }

  window.MHGL = { create, THEMES, demoScene };
})();
