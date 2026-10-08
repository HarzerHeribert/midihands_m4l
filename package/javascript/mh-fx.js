// MidiHands video effects for mh-gl.js.
//
// Every effect has up to six knobs, all 0..1 (Live parameters and gesture
// modulation work on the same scale); the shader maps them to real ranges.
// Image space: v_uv in 0..1, y down. Hands arrive as u_handA / u_handB
// (x, y, radius, present) in the same space.
//
//   MHFX.list   [{id, name, group, knobs: [{name, def}], run(ctx, input, output)}]
//   MHFX.byId   id -> effect
"use strict";

(function () {
  const HEAD = `#version 300 es
precision highp float;
precision highp sampler2DArray;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_size;
uniform float u_time;
uniform float u_p[6];
uniform vec4 u_handA, u_handB;
out vec4 o;
#define P0 u_p[0]
#define P1 u_p[1]
#define P2 u_p[2]
#define P3 u_p[3]
#define P4 u_p[4]
#define P5 u_p[5]
const float PI = 3.14159265;
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 hsv2rgb(vec3 c) { vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0); return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y); }
vec3 rgb2hsv(vec3 c) {
  vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
  vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
  vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
  float d = q.x - min(q.w, q.y);
  return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash12(i), hash12(i + vec2(1, 0)), u.x), mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p, float octaves) {
  float a = 0.5, s = 0.0;
  for (int i = 0; i < 5; i++) { if (float(i) >= octaves) break; s += a * vnoise(p); p = p * 2.03 + 17.1; a *= 0.5; }
  return s;
}
vec2 aspect() { return vec2(u_size.x / u_size.y, 1.0); }
// The point between the visible hands (or c without hands), blended by f.
vec2 follow(vec2 c, float f) {
  vec2 h = c;
  if (u_handA.w > 0.5 && u_handB.w > 0.5) h = (u_handA.xy + u_handB.xy) * 0.5;
  else if (u_handA.w > 0.5) h = u_handA.xy;
  else if (u_handB.w > 0.5) h = u_handB.xy;
  return mix(c, h, f);
}
// 1 inside the hands' reach, 0 far away; used to keep an effect near the hands.
float nearHands(vec2 uv) {
  float m = 0.0;
  vec2 a = aspect();
  if (u_handA.w > 0.5) m = max(m, 1.0 - smoothstep(u_handA.z * 0.6, u_handA.z * 1.9, length((uv - u_handA.xy) * a)));
  if (u_handB.w > 0.5) m = max(m, 1.0 - smoothstep(u_handB.z * 0.6, u_handB.z * 1.9, length((uv - u_handB.xy) * a)));
  return m;
}
vec3 tex(vec2 uv) { return texture(u_tex, uv).rgb; }
`;

  // Uniforms every effect gets: knobs, hands.
  function common(ctx, more) {
    const [a, b] = ctx.anchors;
    return Object.assign({
      u_p: ctx.p,
      u_handA: a ? [a[0], a[1], a[2], 1] : [0.5, 0.5, 0.2, 0],
      u_handB: b ? [b[0], b[1], b[2], 1] : [0.5, 0.5, 0.2, 0],
    }, more || {});
  }
  const simple = (body, extra) => ({
    frag: HEAD + body,
    run(ctx, input, output) { ctx.pass(ctx.prog.main, common(ctx, extra && extra(ctx)), { u_tex: input.tex }, output); },
  });
  const knobs = (...list) => list.map(([name, def]) => ({ name, def }));

  const list = [];
  const add = (id, name, group, knobList, impl) => list.push(Object.assign({ id, name, group, knobs: knobs(...knobList) }, impl));

  // ---- Color --------------------------------------------------------------------------------------
  add("grade", "Film Grade", "Color",
    [["Exposure", 0.5], ["Contrast", 0.5], ["Saturation", 0.5], ["Warmth", 0.5], ["Hue", 0], ["Fade", 0]],
    simple(`
void main() {
  vec3 c = tex(v_uv);
  c *= exp2((P0 - 0.5) * 4.0);                                  // -2..+2 stops
  c = mix(vec3(0.18), c, 0.4 + P1 * 1.6);                       // contrast around mid grey
  c += vec3(0.06, 0.0, -0.06) * (P3 - 0.5) * 2.0;               // warm / cool
  vec3 hsv = rgb2hsv(max(c, 0.0));
  hsv.x = fract(hsv.x + P4);
  hsv.y = clamp(hsv.y * P2 * 2.0, 0.0, 1.0);
  c = hsv2rgb(hsv);
  c = mix(c, vec3(0.08, 0.07, 0.09) + c * 0.82, P5);             // lifted, faded blacks
  o = vec4(clamp(c, 0.0, 1.0), 1.0);
}`));

  add("gradient", "Gradient Map", "Color",
    [["Palette", 0], ["Balance", 0.5], ["Contrast", 0.5], ["Hue", 0], ["Keep Color", 0], ["Grain", 0.15]],
    simple(`
vec3 pal(int i, float t) {
  vec3 a, b, c;
  if (i == 0) { a = vec3(0.10, 0.04, 0.23); b = vec3(1.00, 0.31, 0.43); c = vec3(1.00, 0.86, 0.55); }       // sunset
  else if (i == 1) { a = vec3(0.01, 0.07, 0.17); b = vec3(0.04, 0.48, 0.62); c = vec3(0.72, 0.95, 1.00); }  // ocean
  else if (i == 2) { a = vec3(0.06, 0.00, 0.13); b = vec3(1.00, 0.00, 0.66); c = vec3(0.00, 0.94, 1.00); }  // neon
  else if (i == 3) { a = vec3(0.04, 0.10, 0.00); b = vec3(0.65, 1.00, 0.00); c = vec3(0.97, 1.00, 0.62); }  // acid
  else if (i == 4) { a = vec3(0.07, 0.00, 0.00); b = vec3(1.00, 0.24, 0.00); c = vec3(1.00, 0.88, 0.54); }  // fire
  else if (i == 5) { a = vec3(0.04, 0.05, 0.16); b = vec3(0.37, 0.42, 1.00); c = vec3(0.91, 0.94, 1.00); }  // ice
  else if (i == 6) { a = vec3(0.17, 0.11, 0.09); b = vec3(0.85, 0.41, 0.23); c = vec3(0.96, 0.89, 0.71); }  // retro
  else { a = vec3(0.0); b = vec3(0.5); c = vec3(1.0); }                                                      // mono
  return t < 0.5 ? mix(a, b, t * 2.0) : mix(b, c, t * 2.0 - 1.0);
}
void main() {
  vec3 src = tex(v_uv);
  float l = luma(src);
  l = clamp((l - 0.5) * (0.4 + P2 * 2.2) + 0.5, 0.0, 1.0);
  l = pow(l, exp2((0.5 - P1) * 2.4));                             // balance: more shadow or more highlight
  l += (hash12(v_uv * u_size + fract(u_time) * 97.0) - 0.5) * P5 * 0.18;
  vec3 c = pal(int(floor(P0 * 7.99)), clamp(l, 0.0, 1.0));
  vec3 hsv = rgb2hsv(c); hsv.x = fract(hsv.x + P3); c = hsv2rgb(hsv);
  c = mix(c, c * (src / max(luma(src), 0.05)), P4 * 0.6);
  o = vec4(c, 1.0);
}`));

  add("thermal", "Thermal", "Color",
    [["Palette", 0], ["Gain", 0.5], ["Offset", 0.5], ["Edges", 0.3], ["Hot Hands", 0.6], ["Noise", 0.15]],
    simple(`
vec3 iron(float t) { return clamp(vec3(1.6 * t - 0.1, 1.9 * t * t - 0.25, 0.9 * sin(t * PI * 1.1) + max(0.0, t - 0.8) * 4.0), 0.0, 1.0); }
vec3 rainbow(float t) { return hsv2rgb(vec3(0.7 - 0.75 * t, 0.9, 0.25 + 0.75 * t)); }
vec3 night(float t) { return vec3(0.1, 1.0, 0.35) * (0.08 + t * 1.1); }
void main() {
  vec2 px = 1.0 / u_size;
  float l = luma(tex(v_uv));
  float gx = luma(tex(v_uv + vec2(px.x, 0.0))) - luma(tex(v_uv - vec2(px.x, 0.0)));
  float gy = luma(tex(v_uv + vec2(0.0, px.y))) - luma(tex(v_uv - vec2(0.0, px.y)));
  float t = (l - 0.5) * (0.5 + P1 * 2.5) + 0.5 + (P2 - 0.5);
  t += length(vec2(gx, gy)) * P3 * 4.0;
  t += nearHands(v_uv) * P4 * 0.6;
  t += (vnoise(v_uv * u_size * 0.25 + u_time * 30.0) - 0.5) * P5 * 0.15;
  t = clamp(t, 0.0, 1.0);
  int i = int(floor(P0 * 2.99));
  vec3 c = i == 0 ? iron(t) : (i == 1 ? rainbow(t) : night(t));
  o = vec4(c, 1.0);
}`));

  // ---- Stylize --------------------------------------------------------------------------------------
  add("edges", "Neon Edges", "Stylize",
    [["Width", 0.3], ["Glow", 0.6], ["Threshold", 0.25], ["Color", 0.55], ["Background", 0.15], ["Rainbow", 0]],
    simple(`
float lumAt(vec2 uv) { return luma(tex(uv)); }
float edgeAt(vec2 uv, float d) {
  vec2 px = d / u_size;
  float tl = lumAt(uv + px * vec2(-1, -1)), t = lumAt(uv + px * vec2(0, -1)), tr = lumAt(uv + px * vec2(1, -1));
  float l = lumAt(uv + px * vec2(-1, 0)), r = lumAt(uv + px * vec2(1, 0));
  float bl = lumAt(uv + px * vec2(-1, 1)), b = lumAt(uv + px * vec2(0, 1)), br = lumAt(uv + px * vec2(1, 1));
  float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
  float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);
  return length(vec2(gx, gy));
}
void main() {
  float d = 1.0 + P0 * 4.0;
  float e = smoothstep(P2 * 0.6, P2 * 0.6 + 0.12, edgeAt(v_uv, d));
  // A soft halo from a few wider samples.
  float halo = 0.0;
  for (int i = 0; i < 8; i++) {
    float a = float(i) * PI / 4.0;
    halo += smoothstep(P2 * 0.6, P2 * 0.6 + 0.2, edgeAt(v_uv + vec2(cos(a), sin(a)) * d * 3.0 / u_size, d));
  }
  halo /= 8.0;
  float hue = P3 + P5 * (v_uv.x * 0.6 + v_uv.y * 0.3 + u_time * 0.1);
  vec3 col = hsv2rgb(vec3(fract(hue), 0.85, 1.0));
  vec3 c = tex(v_uv) * P4 + col * (e * (0.6 + P1 * 1.4) + halo * P1 * 0.9) + vec3(1.0) * e * P1 * 0.25;
  o = vec4(c, 1.0);
}`));

  add("glow", "Glow", "Stylize",
    [["Threshold", 0.5], ["Intensity", 0.6], ["Radius", 0.5], ["Tint", 0.08], ["Tint Amount", 0], ["Streaks", 0]],
    {
      frag: {
        bright: HEAD + `
void main() {
  vec3 c = tex(v_uv);
  float l = luma(c), knee = 0.12, th = P0 * 0.9;
  float soft = clamp(l - th + knee, 0.0, 2.0 * knee); soft = soft * soft / (4.0 * knee + 1e-5);
  float w = max(soft, l - th) / max(l, 1e-4);
  o = vec4(c * w, 1.0);
}`,
        blur: HEAD + `
uniform vec2 u_dir;
void main() {
  vec3 s = vec3(0.0); float t = 0.0;
  for (int i = -6; i <= 6; i++) { float w = exp(-float(i * i) / 18.0); s += tex(v_uv + u_dir * float(i) / u_size) * w; t += w; }
  o = vec4(s / t, 1.0);
}`,
        comp: HEAD + `
uniform sampler2D u_glow;
void main() {
  vec3 g = texture(u_glow, v_uv).rgb;
  vec3 tint = mix(vec3(1.0), hsv2rgb(vec3(P3, 0.8, 1.0)) * 1.4, P4);
  o = vec4(tex(v_uv) + g * tint * P1 * 4.0, 1.0);
}`,
      },
      run(ctx, input, output) {
        const w = Math.max(2, ctx.w >> 1), h = Math.max(2, ctx.h >> 1);
        const a = ctx.target("a", w, h), b = ctx.target("b", w, h);
        const u = common(ctx);
        ctx.pass(ctx.prog.bright, u, { u_tex: input.tex }, a);
        const passes = 2 + Math.round(ctx.p[2] * 3), streak = ctx.p[5];
        for (let i = 0; i < passes; i++) {
          const r = (1 + i * 2) * (0.6 + ctx.p[2] * 1.6);
          ctx.pass(ctx.prog.blur, Object.assign({}, u, { u_dir: [r * (1 + streak * 4), 0] }), { u_tex: a.tex }, b);
          ctx.pass(ctx.prog.blur, Object.assign({}, u, { u_dir: [0, r * (1 - streak * 0.9)] }), { u_tex: b.tex }, a);
        }
        ctx.pass(ctx.prog.comp, u, { u_tex: input.tex, u_glow: a.tex }, output);
      },
    });

  add("halftone", "Halftone", "Stylize",
    [["Dot Size", 0.3], ["Angle", 0.15], ["Color", 1], ["Contrast", 0.5], ["Paper", 0.95], ["Softness", 0.3]],
    simple(`
float dotLayer(vec2 uv, float angle, float size, float value) {
  vec2 p = uv * u_size;
  float s = sin(angle), c = cos(angle);
  vec2 q = mat2(c, -s, s, c) * p / size;
  vec2 cell = fract(q) - 0.5;
  float r = sqrt(clamp(value, 0.0, 1.0)) * 0.62;
  float soft = 0.02 + P5 * 0.25;
  return 1.0 - smoothstep(r - soft, r + soft, length(cell));
}
void main() {
  float size = 4.0 + P0 * 22.0;
  vec3 src = tex(v_uv);
  src = clamp((src - 0.5) * (0.6 + P3 * 1.6) + 0.5, 0.0, 1.0);
  vec3 paper = vec3(P4);
  float a = P1 * PI;
  // CMYK-ish: subtract cyan, magenta, yellow and black dots from paper.
  vec3 cmy = 1.0 - src;
  float k = min(cmy.r, min(cmy.g, cmy.b));
  vec3 col = paper;
  col -= vec3(1.0, 0.0, 0.0) * dotLayer(v_uv, a + 0.26, size, cmy.r - k) * 0.85;
  col -= vec3(0.0, 1.0, 0.0) * dotLayer(v_uv, a + 1.31, size, cmy.g - k) * 0.85;
  col -= vec3(0.0, 0.0, 1.0) * dotLayer(v_uv, a, size, cmy.b - k) * 0.85;
  col -= vec3(1.0) * dotLayer(v_uv, a + 0.79, size, k) * 0.9;
  vec3 mono = paper - vec3(dotLayer(v_uv, a + 0.79, size, 1.0 - luma(src))) * P4;
  o = vec4(clamp(mix(mono, col, P2), 0.0, 1.0), 1.0);
}`));

  add("ascii", "ASCII", "Stylize",
    [["Cell Size", 0.3], ["Contrast", 0.5], ["Color", 0], ["Density", 0.5], ["Invert", 0], ["Glow", 0.3]],
    simple(`
// 5x5 glyphs, darkest to brightest: " .:-=+*#%@" (bit 24 = top left).
int glyph(int i) {
  if (i == 0) return 0;
  if (i == 1) return 4;
  if (i == 2) return 131200;
  if (i == 3) return 14336;
  if (i == 4) return 459200;
  if (i == 5) return 145536;
  if (i == 6) return 22511061;
  if (i == 7) return 11512810;
  if (i == 8) return 27070835;
  return 15455982;
}
void main() {
  float cell = 6.0 + P0 * 18.0;
  vec2 grid = floor(v_uv * u_size / cell);
  vec2 center = (grid + 0.5) * cell / u_size;
  vec3 src = tex(center);
  float l = clamp((luma(src) - 0.5) * (0.5 + P1 * 2.5) + 0.5, 0.0, 1.0);
  if (P4 > 0.5) l = 1.0 - l;
  float levels = 3.0 + floor(P3 * 7.0);                          // how many of the 10 glyphs are used
  int g = int(floor(floor(l * (levels - 0.01)) * 9.0 / (levels - 1.0) + 0.5));
  vec2 inCell = fract(v_uv * u_size / cell);
  ivec2 bit = ivec2(floor(inCell * 5.0));
  int n = 4 - bit.x + (4 - bit.y) * 5;
  float on = float((glyph(g) >> n) & 1);
  vec3 ink = mix(vec3(0.25, 1.0, 0.45), src * 1.4 + 0.1, P2);
  float glow = P5 * l * 0.35;
  o = vec4(ink * (on + glow), 1.0);
}`));

  add("mosaic", "Mosaic", "Stylize",
    [["Size", 0.3], ["Shape", 0], ["Gap", 0.1], ["Levels", 1], ["Vivid", 0.3], ["Jitter", 0]],
    simple(`
void main() {
  float size = 4.0 + P0 * 44.0;
  vec2 p = v_uv * u_size / size;
  vec2 cell = floor(p);
  float j = P5 * (hash12(cell + floor(u_time * 6.0)) - 0.5) * 1.2;
  vec3 c = tex((cell + 0.5 + vec2(j, 0.0)) * size / u_size);
  float levels = 2.0 + floor(P3 * 30.0);
  c = floor(c * levels + 0.5) / levels;
  vec3 hsv = rgb2hsv(c); hsv.y = clamp(hsv.y * (1.0 + P4 * 1.5), 0.0, 1.0); c = hsv2rgb(hsv);
  vec2 f = fract(p) - 0.5;
  float sq = max(abs(f.x), abs(f.y)), ci = length(f), di = abs(f.x) + abs(f.y) * 0.9;
  float d = P1 < 0.34 ? sq : (P1 < 0.67 ? ci : di);
  float edge = 0.5 - P2 * 0.25;
  float m = 1.0 - smoothstep(edge - 0.02, edge, d);
  o = vec4(c * m, 1.0);
}`));

  // ---- Distort ---------------------------------------------------------------------------------------
  add("kaleido", "Kaleidoscope", "Distort",
    [["Segments", 0.3], ["Angle", 0], ["Spin", 0.5], ["Zoom", 0.4], ["Follow Hands", 0], ["Tile", 0]],
    simple(`
void main() {
  vec2 c = follow(vec2(0.5), P4);
  vec2 a = aspect();
  vec2 p = (v_uv - c) * a;
  float r = length(p);
  float ang = atan(p.y, p.x) + P1 * 2.0 * PI + (P2 - 0.5) * u_time * 2.0;
  float seg = 2.0 + floor(P0 * 14.0);
  float w = 2.0 * PI / seg;
  ang = mod(ang, w);
  ang = abs(ang - w * 0.5);
  r *= exp2((0.5 - P3) * 2.0);
  vec2 q = vec2(cos(ang), sin(ang)) * r / a + c;
  // Tile: mirror-repeat outside the picture instead of clamping.
  vec2 t = abs(mod(q, 2.0) - 1.0);
  q = mix(clamp(q, 0.0, 1.0), 1.0 - t, P5);
  o = vec4(tex(q), 1.0);
}`));

  add("ripple", "Ripple", "Distort",
    [["Strength", 0.4], ["Rings", 0.4], ["Speed", 0.5], ["Reach", 0.5], ["Follow Hands", 1], ["Shine", 0.4]],
    simple(`
float wave(vec2 uv, vec2 c, float reach) {
  vec2 d = (uv - c) * aspect();
  float r = length(d);
  float freq = 10.0 + P1 * 70.0;
  return sin(r * freq - u_time * (1.0 + P2 * 10.0)) * exp(-r / max(reach, 0.02));
}
void main() {
  float reach = 0.05 + P3 * 0.6;
  vec2 c1 = u_handA.w > 0.5 ? mix(vec2(0.5), u_handA.xy, P4) : vec2(0.5);
  vec2 c2 = u_handB.w > 0.5 ? mix(vec2(0.5), u_handB.xy, P4) : c1;
  float e = 1.5 / u_size.y;
  float h = wave(v_uv, c1, reach) + (u_handB.w > 0.5 && P4 > 0.01 ? wave(v_uv, c2, reach) : 0.0);
  float hx = wave(v_uv + vec2(e, 0.0), c1, reach) + (u_handB.w > 0.5 && P4 > 0.01 ? wave(v_uv + vec2(e, 0.0), c2, reach) : 0.0);
  float hy = wave(v_uv + vec2(0.0, e), c1, reach) + (u_handB.w > 0.5 && P4 > 0.01 ? wave(v_uv + vec2(0.0, e), c2, reach) : 0.0);
  vec2 n = vec2(hx - h, hy - h) / e;
  vec2 uv = v_uv + n * P0 * 0.004 / (10.0 + P1 * 70.0) * 12.0;
  vec3 c = tex(uv);
  float spec = pow(max(0.0, dot(normalize(vec3(-n * 0.004, 1.0)), normalize(vec3(-0.4, -0.5, 0.8)))), 30.0);
  o = vec4(c + spec * P5 * 0.8, 1.0);
}`));

  add("liquid", "Liquid", "Distort",
    [["Amount", 0.5], ["Scale", 0.3], ["Speed", 0.3], ["Detail", 0.5], ["Swirl", 0.25], ["Near Hands", 0]],
    simple(`
void main() {
  float scale = 1.0 + P1 * 9.0, t = u_time * (0.05 + P2 * 1.2), oct = 1.0 + floor(P3 * 4.0);
  vec2 p = v_uv * aspect() * scale;
  vec2 q = vec2(fbm(p + vec2(0.0, t), oct), fbm(p + vec2(5.2, -t), oct));
  vec2 r = vec2(fbm(p + 4.0 * q + vec2(1.7, 9.2) + t, oct), fbm(p + 4.0 * q + vec2(8.3, 2.8) - t, oct));
  vec2 d = (r - 0.5) * P0 * 0.3;
  vec2 c = v_uv - 0.5;
  float sw = P4 * 3.0 * (fbm(p * 0.5 + t, 2.0) - 0.5);
  d += vec2(-c.y, c.x) * sw * 0.3;
  float m = mix(1.0, nearHands(v_uv), P5);
  o = vec4(tex(v_uv + d * m), 1.0);
}`));

  add("rgbsplit", "RGB Split", "Distort",
    [["Amount", 0.45], ["Angle", 0], ["Radial", 0.6], ["Follow Hands", 0], ["Jitter", 0.1], ["Smear", 0.3]],
    simple(`
void main() {
  vec2 c = follow(vec2(0.5), P3);
  float amt = P0 * 0.05 * (1.0 + P4 * (vnoise(vec2(u_time * 8.0, 0.0)) - 0.5) * 3.0);
  vec2 lin = vec2(cos(P1 * 2.0 * PI), sin(P1 * 2.0 * PI));
  vec2 rad = (v_uv - c);
  vec2 dir = mix(lin, rad * 2.0, P2) * amt;
  vec3 s = vec3(0.0); float t = 0.0;
  for (int i = 0; i < 5; i++) {
    float k = 1.0 + float(i) * P5 * 0.6;
    s += vec3(tex(v_uv + dir * k).r, tex(v_uv).g, tex(v_uv - dir * k).b);
    t += 1.0;
    if (P5 < 0.01) break;
  }
  o = vec4(s / t, 1.0);
}`));

  add("mirror", "Mirror", "Distort",
    [["Mode", 0], ["Position", 0.5], ["Follow Hands", 0], ["Feather", 0], ["Flip", 0], ["", 0]],
    simple(`
void main() {
  vec2 uv = v_uv;
  float pos = mix(P1, follow(vec2(P1), 1.0).x, P2);
  int mode = int(floor(P0 * 3.99));
  if (mode == 0 || mode == 3) uv.x = uv.x < pos ? uv.x : 2.0 * pos - uv.x;
  if (mode == 1 || mode == 3) uv.y = uv.y < P1 ? uv.y : 2.0 * P1 - uv.y;
  if (mode == 2) uv.x = uv.x > pos ? uv.x : 2.0 * pos - uv.x;
  if (P4 > 0.5) uv.x = 1.0 - uv.x;
  vec3 m = tex(clamp(uv, 0.0, 1.0));
  float f = P3 * 0.2;
  float seam = mode == 1 ? abs(v_uv.y - P1) : abs(v_uv.x - pos);
  o = vec4(mix(m, tex(v_uv), f > 0.0 ? 1.0 - smoothstep(0.0, f, seam) : 0.0), 1.0);
}`));

  add("tunnel", "Tunnel", "Distort",
    [["Depth", 0.5], ["Speed", 0.5], ["Twist", 0.3], ["Segments", 0], ["Follow Hands", 0], ["Fade", 0.5]],
    simple(`
void main() {
  vec2 c = follow(vec2(0.5), P4);
  vec2 p = (v_uv - c) * aspect();
  float r = max(length(p), 1e-3);
  float a = atan(p.y, p.x);
  float seg = 1.0 + floor(P3 * 8.0);
  float z = (0.2 + P0 * 1.6) / r + u_time * (P1 - 0.5) * 2.0;
  float ang = a / (2.0 * PI) * seg + z * P2;
  vec2 uv = vec2(fract(ang), fract(z));
  uv = abs(uv * 2.0 - 1.0);                                       // mirrored, seamless
  vec3 col = tex(uv);
  float fade = mix(1.0, smoothstep(0.0, 0.5, r), P5);
  o = vec4(col * fade, 1.0);
}`));

  // ---- Feedback ----------------------------------------------------------------------------------------
  // Shared: the previous output, kept per slot, reset when the slot starts.
  const feedback = (body) => ({
    frag: { main: HEAD + "uniform sampler2D u_prev;\n" + body },
    run(ctx, input, output) {
      const prev = ctx.target("prev");
      if (!ctx.mem.primed) { ctx.copy(input, prev); ctx.mem.primed = true; }
      ctx.pass(ctx.prog.main, common(ctx), { u_tex: input.tex, u_prev: prev.tex }, output);
      ctx.copy(output, prev);
    },
  });

  add("echo", "Echo", "Feedback",
    [["Decay", 0.6], ["Zoom", 0.5], ["Rotate", 0.5], ["Hue Drift", 0], ["Moving Only", 0.5], ["Follow Hands", 0]],
    feedback(`
void main() {
  vec2 c = follow(vec2(0.5), P5);
  vec2 p = (v_uv - c) * aspect();
  float rot = (P2 - 0.5) * 0.08, s = sin(rot), co = cos(rot);
  p = mat2(co, -s, s, co) * p * (1.0 - (P1 - 0.5) * 0.06);
  vec2 puv = p / aspect() + c;
  vec3 prev = texture(u_prev, puv).rgb;
  vec3 hsv = rgb2hsv(prev); hsv.x = fract(hsv.x + P3 * 0.03); prev = hsv2rgb(hsv);
  vec3 now = tex(v_uv);
  float decay = 0.6 + P0 * 0.39;
  // Moving only: keep echoes where the picture changes, let still areas show the live picture.
  float motion = smoothstep(0.02, 0.12, length(now - texture(u_prev, v_uv).rgb));
  vec3 echo = max(now, prev * decay);
  o = vec4(mix(echo, mix(now, echo, motion), P4), 1.0);
}`));

  add("trails", "RGB Trails", "Feedback",
    [["Decay", 0.75], ["Spread", 0.5], ["Drift", 0.3], ["Direction", 0.75], ["Glow", 0.4], ["Threshold", 0.3]],
    {
      // hist: trail energy of moving pixels; last: the previous input, to see what moved.
      frag: { main: HEAD + `
uniform sampler2D u_hist, u_last;
void main() {
  vec2 dir = vec2(cos(P3 * 2.0 * PI), sin(P3 * 2.0 * PI)) * P2 * 0.008;
  vec3 now = tex(v_uv);
  float motion = smoothstep(0.03 + P5 * 0.2, 0.1 + P5 * 0.3, length(now - texture(u_last, v_uv).rgb));
  float base = 0.75 + P0 * 0.23;
  vec3 decay = clamp(base + vec3(-1.0, 0.0, 1.0) * P1 * 0.1, 0.0, 0.99);
  vec3 h = vec3(texture(u_hist, v_uv - dir).r, texture(u_hist, v_uv - dir * 2.0).g, texture(u_hist, v_uv - dir * 3.0).b) * decay;
  h = max(h, now * motion);
  o = vec4(h, 1.0);
}`, show: HEAD + `
uniform sampler2D u_hist;
void main() { vec3 now = tex(v_uv); vec3 h = texture(u_hist, v_uv).rgb; o = vec4(max(now, h) + h * P4 * 0.5, 1.0); }` },
      run(ctx, input, output) {
        const hist = ctx.target("hist"), next = ctx.target("next"), last = ctx.target("last");
        if (!ctx.mem.primed) { ctx.clear(hist, [0, 0, 0, 1]); ctx.copy(input, last); ctx.mem.primed = true; }
        ctx.pass(ctx.prog.main, common(ctx), { u_tex: input.tex, u_hist: hist.tex, u_last: last.tex }, next);
        ctx.copy(next, hist);
        ctx.copy(input, last);
        ctx.pass(ctx.prog.show, common(ctx), { u_tex: input.tex, u_hist: hist.tex }, output);
      },
    });

  // Time Warp: a ring of past frames; each part of the picture shows a different moment.
  add("timewarp", "Time Warp", "Feedback",
    [["Delay", 0.6], ["Direction", 0], ["Follow Hands", 0], ["Bands", 0], ["Invert", 0], ["", 0]],
    {
      frag: {
        main: HEAD + `
uniform sampler2DArray u_hist;
uniform float u_head, u_frames;
void main() {
  float t;
  int mode = int(floor(P1 * 2.99));
  vec2 c = follow(vec2(0.5), P2);
  if (mode == 0) t = v_uv.y;                                      // top to bottom
  else if (mode == 1) t = v_uv.x;                                  // left to right
  else t = clamp(length((v_uv - c) * aspect()) * 1.6, 0.0, 1.0);   // rings from the center
  if (P3 > 0.01) t = fract(t * (1.0 + floor(P3 * 7.0)));
  if (P4 > 0.5) t = 1.0 - t;
  float delay = floor(t * (u_frames - 1.0) * max(P0, 0.02));
  float layer = mod(u_head - delay + u_frames, u_frames);
  o = vec4(texture(u_hist, vec3(v_uv, layer)).rgb, 1.0);
}`,
      },
      run(ctx, input, output) {
        const gl = ctx.gl, N = 48;
        const w = Math.min(ctx.w, 960), h = Math.max(2, Math.round(w * ctx.h / ctx.w));
        const m = ctx.mem;
        if (!m.hist || m.w !== w || m.h !== h) {
          if (m.hist) gl.deleteTexture(m.hist);
          m.hist = gl.createTexture(); m.w = w; m.h = h; m.head = 0; m.filled = false;
          gl.bindTexture(gl.TEXTURE_2D_ARRAY, m.hist);
          gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.RGBA8, w, h, N);
          gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
          gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
          gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
          gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
          m.fb = gl.createFramebuffer();
        }
        // Write this frame into the next layer (all layers at the start).
        const writeLayer = (layer) => {
          gl.bindFramebuffer(gl.FRAMEBUFFER, m.fb);
          gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, m.hist, 0, layer);
          ctx.copy(input, { fb: m.fb, w, h });
        };
        m.head = (m.head + 1) % N;
        if (!m.filled) { for (let i = 0; i < N; i++) writeLayer(i); m.filled = true; } else writeLayer(m.head);
        ctx.pass(ctx.prog.main, common(ctx, { u_head: m.head, u_frames: N }), { u_tex: input.tex, u_hist: { array: m.hist } }, output);
      },
    });

  // ---- Glitch ------------------------------------------------------------------------------------------
  add("vhs", "VHS", "Glitch",
    [["Tracking", 0.3], ["Wobble", 0.3], ["Color Bleed", 0.5], ["Grain", 0.4], ["Scanlines", 0.4], ["Fade", 0.3]],
    simple(`
void main() {
  vec2 uv = v_uv;
  float t = u_time;
  uv.x += (vnoise(vec2(uv.y * 3.0, t * 2.0)) - 0.5) * P1 * 0.012;
  // Tracking bands rolling down the picture.
  float band = smoothstep(0.0, 0.04, abs(fract(uv.y * 0.7 - t * 0.15) - 0.5) - 0.44 + P0 * 0.06);
  float tear = (1.0 - band) * P0;
  uv.x += tear * (hash12(vec2(floor(uv.y * 240.0), floor(t * 30.0))) - 0.5) * 0.06;
  float bleed = P2 * 0.006;
  vec3 c = vec3(tex(uv + vec2(bleed, 0.0)).r, tex(uv).g, tex(uv - vec2(bleed, 0.0)).b);
  c = mix(c, vec3(luma(c)), 0.15) * vec3(1.02, 0.98, 1.06);
  c += (hash12(uv * u_size + fract(t) * 123.0) - 0.5) * P3 * 0.25;
  c += tear * 0.4 * hash12(vec2(uv.y * 500.0, t));
  c *= 1.0 - P4 * 0.35 * (0.5 + 0.5 * sin(uv.y * u_size.y * PI * 0.5));
  c = mix(c, c * 0.85 + 0.08, P5);
  o = vec4(clamp(c, 0.0, 1.0), 1.0);
}`));

  add("glitch", "Glitch", "Glitch",
    [["Amount", 0.4], ["Block Size", 0.4], ["Shift", 0.5], ["RGB", 0.5], ["Rate", 0.5], ["Scramble", 0.2]],
    simple(`
void main() {
  float rate = 2.0 + P4 * 28.0;
  float step_ = floor(u_time * rate);
  vec2 bs = vec2(0.04 + P1 * 0.3, 0.01 + P1 * 0.08);
  vec2 block = floor(v_uv / bs);
  float r = hash12(block + step_ * 1.37);
  float hit = step(1.0 - P0 * 0.6, r);
  vec2 uv = v_uv;
  uv.x += hit * (hash12(block.yx + step_) - 0.5) * P2 * 0.3;
  if (hash12(block * 3.1 + step_) < P5 * hit) uv = (floor(uv / bs) + vec2(hash12(block + 9.0), hash12(block + 4.0))) * bs + fract(uv / bs) * bs;
  float split = hit * P3 * 0.02;
  vec3 c = vec3(tex(uv + vec2(split, 0.0)).r, tex(uv).g, tex(uv - vec2(split, 0.0)).b);
  if (hit > 0.5 && hash12(block + step_ * 2.0) > 0.85) c = 1.0 - c;
  o = vec4(c, 1.0);
}`));

  add("crt", "CRT", "Glitch",
    [["Curvature", 0.4], ["Scanlines", 0.5], ["Mask", 0.4], ["Glow", 0.3], ["Vignette", 0.5], ["Flicker", 0.1]],
    simple(`
void main() {
  vec2 p = v_uv * 2.0 - 1.0;
  p *= 1.0 + P0 * 0.18 * dot(p.yx, p.yx);
  vec2 uv = p * 0.5 + 0.5;
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }
  vec3 c = tex(uv);
  vec3 blur = (tex(uv + vec2(2.0, 0.0) / u_size) + tex(uv - vec2(2.0, 0.0) / u_size) + tex(uv + vec2(0.0, 2.0) / u_size) + tex(uv - vec2(0.0, 2.0) / u_size)) * 0.25;
  c += blur * P3 * 0.6;
  float lines = u_size.y * 0.5;
  c *= 1.0 - P1 * 0.55 * (0.5 - 0.5 * cos(uv.y * lines * 2.0 * PI));
  float m = mod(floor(v_uv.x * u_size.x), 3.0);
  vec3 mask = m < 1.0 ? vec3(1.0, 0.6, 0.6) : (m < 2.0 ? vec3(0.6, 1.0, 0.6) : vec3(0.6, 0.6, 1.0));
  c *= mix(vec3(1.0), mask * 1.25, P2);
  c *= 1.0 - P4 * 0.7 * dot(p * 0.7, p * 0.7);
  c *= 1.0 - P5 * 0.15 * hash12(vec2(floor(u_time * 24.0), 1.0));
  o = vec4(c, 1.0);
}`));

  add("strobe", "Strobe", "Glitch",
    [["Rate", 0.4], ["Color", 0], ["Invert", 0], ["Length", 0.3], ["Saturation", 0], ["Random", 0]],
    simple(`
void main() {
  // Rate 0 = always on, so Mix (or a gesture on Mix) decides when it flashes.
  float hz = P0 < 0.02 ? 0.0 : 1.0 + P0 * 19.0;
  float ph = hz > 0.0 ? fract(u_time * hz) : 0.0;
  float on = hz > 0.0 ? step(ph, 0.05 + P3 * 0.5) : 1.0;
  if (P5 > 0.01 && hz > 0.0) on *= step(1.0 - P5, hash12(vec2(floor(u_time * hz), 7.0)));
  vec3 src = tex(v_uv);
  vec3 flash = mix(vec3(1.0), hsv2rgb(vec3(P1, max(P4, 0.001), 1.0)), step(0.001, P4));
  vec3 c = P2 > 0.5 ? 1.0 - src : flash;
  o = vec4(mix(src, c, on), 1.0);
}`));

  // ---- Simulation ----------------------------------------------------------------------------------------
  // Gray-Scott reaction diffusion in half-float buffers, seeded by the hands and bright edges.
  add("reaction", "Reaction Diffusion", "Simulation",
    [["Pattern", 0.3], ["Speed", 0.6], ["Seed", 0.7], ["Color", 0.6], ["Video", 0.7], ["Reset", 0]],
    {
      frag: {
        sim: HEAD + `
uniform sampler2D u_state, u_video;
void main() {
  vec2 px = 1.0 / u_size;
  vec2 s = texture(u_state, v_uv).rg;
  vec2 lap = -s;
  lap += 0.2 * (texture(u_state, v_uv + vec2(px.x, 0.0)).rg + texture(u_state, v_uv - vec2(px.x, 0.0)).rg
              + texture(u_state, v_uv + vec2(0.0, px.y)).rg + texture(u_state, v_uv - vec2(0.0, px.y)).rg);
  lap += 0.05 * (texture(u_state, v_uv + px).rg + texture(u_state, v_uv - px).rg
              + texture(u_state, v_uv + vec2(px.x, -px.y)).rg + texture(u_state, v_uv + vec2(-px.x, px.y)).rg);
  // Pattern knob walks through classic feed/kill pairs: spots, worms, coral, maze, waves.
  float t = P0 * 4.0;
  vec2 fk = t < 1.0 ? mix(vec2(0.0367, 0.0649), vec2(0.078, 0.061), t)
          : t < 2.0 ? mix(vec2(0.078, 0.061), vec2(0.0545, 0.062), t - 1.0)
          : t < 3.0 ? mix(vec2(0.0545, 0.062), vec2(0.029, 0.057), t - 2.0)
          : mix(vec2(0.029, 0.057), vec2(0.014, 0.047), t - 3.0);
  float a = s.r, b = s.g, abb = a * b * b;
  float na = a + (1.0 * lap.r - abb + fk.x * (1.0 - a));
  float nb = b + (0.5 * lap.g + abb - (fk.x + fk.y) * b);
  float seed = max(nearHands(v_uv), smoothstep(0.9, 1.0, luma(texture(u_video, v_uv).rgb)) * 0.6);
  seed *= step(0.985, hash12(floor(v_uv * u_size / 2.0) + floor(u_time * 3.0)));  // sparse specks, so patterns can form
  nb = max(nb, seed * P2);
  o = vec4(clamp(na, 0.0, 1.0), clamp(nb, 0.0, 1.0), 0.0, 1.0);
}`,
        show: HEAD + `
uniform sampler2D u_state;
void main() {
  float b = texture(u_state, v_uv).g;
  float v = smoothstep(0.05, 0.3, b);
  vec3 col = hsv2rgb(vec3(fract(P3 + v * 0.3), 0.7, 0.35 + v * 0.75));
  o = vec4(mix(tex(v_uv) * P4, col, v), 1.0);
}`,
      },
      run(ctx, input, output) {
        if (!ctx.floatOk) { ctx.copy(input, output); return; }
        const w = Math.max(2, ctx.w >> 1), h = Math.max(2, ctx.h >> 1);
        const m = ctx.mem;
        const a = ctx.target("a", w, h, "float"), b = ctx.target("b", w, h, "float");
        const u = common(ctx);
        if (!m.seeded || ctx.p[5] > 0.5) {   // fill with A=1, B=0 plus a few seeds
          ctx.clear(a, [1, 0, 0, 1]); ctx.clear(b, [1, 0, 0, 1]); m.seeded = true;
        }
        const steps = 2 + Math.round(ctx.p[1] * 14);
        let src = a, dst = b;
        for (let i = 0; i < steps; i++) {
          ctx.pass(ctx.prog.sim, u, { u_state: src.tex, u_video: input.tex }, dst);
          [src, dst] = [dst, src];
        }
        if (src !== a) ctx.copy(src, a);
        ctx.pass(ctx.prog.show, u, { u_tex: input.tex, u_state: a.tex }, output);
      },
    });

  const byId = {};
  for (const e of list) byId[e.id] = e;
  const GROUPS = ["Color", "Stylize", "Distort", "Feedback", "Glitch", "Simulation"];
  const SLOTS = 4;

  // The chain from Live parameter values (P, as the device stores them) and the
  // modulation sources: x<s>fx is 1 + the effect's index (0 = none), x<s>on
  // switches the slot, x<s>mix and x<s>p<k> are 0..1, and every control j
  // (0 = mix, 1..6 = knobs) can be moved by a source x<s>m<j>s (0 = none) times
  // x<s>m<j>a (-1..1). Sources: 1..10 the hand movements, then MidiHands Audio
  // letters A..H x level, bass, mid, high, beat (11..50), as the generator's
  // MOD_SOURCES. `m` holds them: { expr: 10 movements, held: 8 x 10 movements
  // that only follow the hand while a clutch gesture is held (mh.hands), audio: 40 }.
  // A slot engaged by a gesture (x<s>eng, 0 = always) takes its hand movements
  // from that gesture's held set, so they stay put while the gesture is let go.
  function values(P, s, m) {
    const eng = P[`x${s}eng`] | 0, held = m.held || [];
    const hands = eng && held.length >= eng * HAND_SOURCES ? held.slice((eng - 1) * HAND_SOURCES, eng * HAND_SOURCES) : m.expr || [];
    return hands.concat(m.audio || []);
  }
  function modulated(P, v, s, j, base) {
    const src = P[`x${s}m${j}s`] | 0;
    if (!src) return base;
    return Math.max(0, Math.min(1, base + (P[`x${s}m${j}a`] || 0) * (v[src - 1] || 0)));
  }
  function slots(P, m) {
    const out = [];
    for (let s = 0; s < SLOTS; s++) {
      const fx = list[(P[`x${s}fx`] | 0) - 1];
      if (!fx || P[`x${s}on`] === 0) continue;
      const v = values(P, s, m);
      out.push({
        id: fx.id, slot: s,
        mix: modulated(P, v, s, 0, P[`x${s}mix`] ?? 1),
        p: [0, 1, 2, 3, 4, 5].map((k) => modulated(P, v, s, k + 1, P[`x${s}p${k}`] ?? 0.5)),
      });
    }
    return out;
  }
  // Hand movements that drive effects (expression indices), for the gesture cues.
  function sources(P) {
    const used = new Set();
    for (let s = 0; s < SLOTS; s++) {
      if (!(P[`x${s}fx`] | 0) || P[`x${s}on`] === 0) continue;
      for (let j = 0; j < 7; j++) { const src = P[`x${s}m${j}s`] | 0; if (src && src <= HAND_SOURCES) used.add(src - 1); }
    }
    return used;
  }
  const HAND_SOURCES = 10, AUDIO_LETTERS = "ABCDEFGH", AUDIO_FEATURES = ["Level", "Bass", "Mid", "High", "Beat"];
  // Modulation source index for letter c (0..7) and feature f (0..4).
  const audioSource = (c, f) => HAND_SOURCES + 1 + c * AUDIO_FEATURES.length + f;
  const FORMATS = [16 / 9, 9 / 16, 1, 4 / 5];
  window.MHFX = { list, byId, GROUPS, SLOTS, FORMATS, slots, sources, values, modulated, HAND_SOURCES, AUDIO_LETTERS, AUDIO_FEATURES, audioSource };
})();
