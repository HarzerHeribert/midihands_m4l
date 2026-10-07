// Hand view for the midihands device (v8ui). Draws both hands as skeletons,
// lights up fingers that are playing, and shows camera status.
//
// Messages from the device:
//   hands <aspect> then per hand: <present> <21 x,y pairs> <4 finger states>
//   stats <fps> <vision ms> <frame->landmarks ms>
//   status running <camera> <w> <h> <fps> | status stopped | status <text...>
//   error <text...>

mgraphics.init();
mgraphics.relative_coords = 0;
mgraphics.autofill = 0;

const BONES = [
  [0, 1], [1, 2], [2, 3], [3, 4],
  [0, 5], [5, 6], [6, 7], [7, 8],
  [9, 10], [10, 11], [11, 12],
  [13, 14], [14, 15], [15, 16],
  [0, 17], [17, 18], [18, 19], [19, 20],
  [5, 9], [9, 13], [13, 17],
];
// Bones and tips that belong to index, middle, ring, pinky.
const FINGER_BONES = [[5, 6, 7, 8], [9, 10, 11, 12], [13, 14, 15, 16], [17, 18, 19, 20]];

const COLORS = {
  bg: [0.11, 0.11, 0.11, 1],
  frame: [0.22, 0.22, 0.22, 1],
  bone: [0.62, 0.62, 0.62, 1],
  active: [1.0, 0.63, 0.15, 1],
  text: [0.75, 0.75, 0.75, 1],
  error: [1.0, 0.38, 0.32, 1],
};

let aspect = 16 / 9;
let handState = [null, null];
let line = "Camera off";
let lineIsError = false;

// v8ui calls the function named after each incoming message.
function hands(...args) {
  if (args[0] > 0) aspect = args[0];
  let n = 1;
  for (let s = 0; s < 2; s++) {
    const present = args[n++] > 0.5;
    const pts = [];
    for (let i = 0; i < 21; i++) {
      pts.push([args[n], args[n + 1]]);
      n += 2;
    }
    const on = [args[n], args[n + 1], args[n + 2], args[n + 3]].map((v) => v > 0.5);
    n += 4;
    handState[s] = present ? { pts, on } : null;
  }
  mgraphics.redraw();
}

function stats(fps, visionMs) {
  if (!lineIsError && fps > 0) {
    line = `${fps.toFixed(0)} fps · ${visionMs.toFixed(1)} ms`;
    mgraphics.redraw();
  }
}

function status(...args) {
  lineIsError = false;
  if (args[0] === "running") line = String(args[1]);
  else if (args[0] === "stopped") {
    line = "Camera off";
    handState = [null, null];
  } else line = args.join(" ");
  mgraphics.redraw();
}

function error(...args) {
  lineIsError = true;
  line = args.join(" ");
  mgraphics.redraw();
}

function setColor(c) {
  mgraphics.set_source_rgba(c[0], c[1], c[2], c[3]);
}

function paint() {
  const [w, h] = mgraphics.size;
  setColor(COLORS.bg);
  mgraphics.rectangle(0, 0, w, h);
  mgraphics.fill();

  // Camera image area, letterboxed to the camera's aspect ratio.
  const textH = 14;
  let iw = w, ih = w / aspect;
  if (ih > h - textH) {
    ih = h - textH;
    iw = ih * aspect;
  }
  const ox = (w - iw) / 2;
  const oy = 0;
  setColor(COLORS.frame);
  mgraphics.set_line_width(1);
  mgraphics.rectangle(ox + 0.5, oy + 0.5, iw - 1, ih - 1);
  mgraphics.stroke();

  const P = (p) => [ox + p[0] * iw, oy + p[1] * ih];
  for (const hand of handState) {
    if (!hand) continue;
    const activePoints = new Set();
    hand.on.forEach((on, f) => on && FINGER_BONES[f].forEach((i) => activePoints.add(i)));
    mgraphics.set_line_width(1.5);
    for (const [a, b] of BONES) {
      setColor(activePoints.has(a) && activePoints.has(b) ? COLORS.active : COLORS.bone);
      const pa = P(hand.pts[a]), pb = P(hand.pts[b]);
      mgraphics.move_to(pa[0], pa[1]);
      mgraphics.line_to(pb[0], pb[1]);
      mgraphics.stroke();
    }
    for (let f = 0; f < 4; f++) {
      const tip = P(hand.pts[FINGER_BONES[f][3]]);
      setColor(hand.on[f] ? COLORS.active : COLORS.bone);
      mgraphics.arc(tip[0], tip[1], hand.on[f] ? 3.5 : 2, 0, Math.PI * 2);
      mgraphics.fill();
    }
  }

  setColor(lineIsError ? COLORS.error : COLORS.text);
  mgraphics.select_font_face("Ableton Sans Medium");
  mgraphics.set_font_size(9);
  let text = line;
  while (text.length > 4 && mgraphics.text_measure(text)[0] > w - 4) text = text.slice(0, -2);
  if (text !== line) text += "…";
  mgraphics.move_to(2, h - 3);
  mgraphics.show_text(text);
}
