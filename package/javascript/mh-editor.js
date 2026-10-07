// Status line and movement meters for the MidiHands editor window (v8ui).
//
// Messages:
//   status running <camera> <w> <h> <fps> <devices on this camera> | status stopped | status <text...>
//   stats <fps> <vision ms> <frame->landmarks ms>
//   error <text...>
//   expr <10 floats 0-1>  (L height, x, pinch, fist, tilt, then the same for R)

mgraphics.init();
mgraphics.relative_coords = 0;
mgraphics.autofill = 0;

const NAMES = ["Height", "X", "Pinch", "Fist", "Tilt"];
const COLORS = {
  text: [0.78, 0.78, 0.78, 1],
  dim: [0.5, 0.5, 0.5, 1],
  track: [0.24, 0.24, 0.24, 1],
  left: [0.35, 0.78, 1.0, 1],
  right: [1.0, 0.63, 0.15, 1],
  error: [1.0, 0.38, 0.32, 1],
};

let camera = "";
let devices = 0;
let fps = 0;
let visionMs = 0;
let latencyMs = 0;
let running = false;
let errorText = "";
let values = new Array(10).fill(0);

function status(...args) {
  errorText = "";
  running = args[0] === "running";
  if (running) {
    camera = String(args[1]);
    devices = args[5] || 1;
  } else {
    camera = args[0] === "stopped" ? "" : args.join(" ");
    fps = 0;
  }
  mgraphics.redraw();
}

function stats(f, v, l) {
  fps = f;
  visionMs = v;
  latencyMs = l;
  mgraphics.redraw();
}

function error(...args) {
  errorText = args.join(" ");
  mgraphics.redraw();
}

function expr(...args) {
  values = args.slice(0, 10);
  mgraphics.redraw();
}

function setColor(c) {
  mgraphics.set_source_rgba(c[0], c[1], c[2], c[3]);
}

function text(str, x, y, size, color) {
  mgraphics.select_font_face("Ableton Sans Medium");
  mgraphics.set_font_size(size);
  setColor(color);
  mgraphics.move_to(x, y);
  mgraphics.show_text(str);
}

function paint() {
  const [w] = mgraphics.size;

  // Status line.
  if (errorText) text(errorText, 0, 14, 11, COLORS.error);
  else if (!running && !camera) text("Camera off. Switch it on in the device on the track.", 0, 14, 11, COLORS.dim);
  else if (!running) text(camera, 0, 14, 11, COLORS.text);
  else {
    const shared = devices > 1 ? ` · shared by ${devices} devices` : "";
    text(`${camera} · ${fps.toFixed(0)} fps · detection ${visionMs.toFixed(1)} ms${shared}`, 0, 14, 11, COLORS.text);
  }

  // Movement meters: left hand column, right hand column.
  text("MOVEMENT", 0, 40, 12, COLORS.text);
  const colW = (w - 24) / 2;
  for (let side = 0; side < 2; side++) {
    const x0 = side * (colW + 24);
    const color = side === 0 ? COLORS.left : COLORS.right;
    text(side === 0 ? "Left hand" : "Right hand", x0, 60, 10, color);
    for (let i = 0; i < 5; i++) {
      const y = 70 + i * 22;
      const v = Math.max(0, Math.min(1, values[side * 5 + i] || 0));
      text(NAMES[i], x0, y + 11, 10, COLORS.dim);
      const bx = x0 + 52, bw = colW - 52 - 40;
      setColor(COLORS.track);
      mgraphics.rectangle(bx, y + 3, bw, 9);
      mgraphics.fill();
      setColor(color);
      mgraphics.rectangle(bx, y + 3, bw * v, 9);
      mgraphics.fill();
      text(`${Math.round(v * 100)}%`, bx + bw + 6, y + 11, 10, COLORS.dim);
    }
  }
}
