// Sound for the video effects (a [v8] in the MidiHands device).
//
// Every MidiHands Audio device in the Set sends on the global name mh_audio; this
// collects them by letter and passes them to this device's pages.
//
// in:  v <letter 0..7> <level> <bass> <mid> <high> <beat>   ~60 times a second per device
//      n <letter> <id> <track name>                          once a second per device
// out: audio <40 values>   letters A..H x level, bass, mid, high, beat; about 60 times
//                          a second while any device sends
//      audiosrc <json>     [{c, name, dup}] the letters in use, on changes and every second

autowatch = 0;
inlets = 1;
outlets = 1;

const LETTERS = 8;
const FEATURES = 5;
const GONE_MS = 2500;  // a letter disappears when its device has not been heard for this long

const values = new Array(LETTERS * FEATURES).fill(0);
const letters = [];
for (let c = 0; c < LETTERS; c++) letters.push({ seen: 0, devices: {} });
let changed = false;
let lastSources = "";
let lastAnnounce = 0;

function v(c, level, bass, mid, high, beat) {
  c = c | 0;
  if (c < 0 || c >= LETTERS) return;
  const at = c * FEATURES;
  values[at] = level; values[at + 1] = bass; values[at + 2] = mid; values[at + 3] = high; values[at + 4] = beat;
  letters[c].seen = Date.now();
  changed = true;
}

function n(c, id) {
  c = c | 0;
  if (c < 0 || c >= LETTERS) return;
  const name = Array.prototype.slice.call(arguments, 2).join(" ");
  const now = Date.now();
  // A device that switched letters leaves its old one.
  for (const other of letters) delete other.devices[id];
  letters[c].devices[id] = { name: name, seen: now };
  letters[c].seen = now;
}

function sources(now) {
  const list = [];
  for (let c = 0; c < LETTERS; c++) {
    const devices = letters[c].devices;
    for (const id of Object.keys(devices)) if (now - devices[id].seen > GONE_MS) delete devices[id];
    const ids = Object.keys(devices);
    if (!ids.length) {
      if (now - letters[c].seen > GONE_MS) for (let f = 0; f < FEATURES; f++) values[c * FEATURES + f] = 0;
      continue;
    }
    list.push({ c: c, name: devices[ids[0]].name, dup: ids.length > 1 });
  }
  return JSON.stringify(list);
}

function tick() {
  const now = Date.now();
  const list = sources(now);
  if (list !== lastSources || now - lastAnnounce > 1000) {
    lastSources = list;
    lastAnnounce = now;
    outlet(0, "audiosrc", list);
  }
  if (changed) {
    changed = false;
    outlet(0, ["audio"].concat(values));
  }
}

const task = new Task(tick, this);
task.interval = 16;
task.repeat();
