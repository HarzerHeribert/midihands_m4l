#!/usr/bin/env python3
"""Generates MidiHands.amxd, the Max for Live MIDI effect, and its companion
"MidiHands Audio.amxd", an audio effect that sends a track's sound to it.

The patch is described in code so changes are reviewable in git. Run
`make device` (or this script) after editing; open the result in Live.

Two views:
  strip   the device on the track: hand view, camera on/off and choice,
          and a button that opens the editor
  editor  a separate window (subpatcher "MidiHands") showing a web page
          (package/javascript/mh-editor.html) in a jweb: PLAY (camera,
          fingers, keyboard, sound) and MOVE (movement meters, map slots)

The two halves talk through device-local send/receive names; Max for Live
replaces the "---" prefix with an id unique to each device instance:
  ---mh_in       settings and commands -> mh.hands
  ---mh_boot     device finished loading (resend stored settings)
  ---mh_open     the editor window was opened
  ---mh_expr     ten hand-movement values, every frame
  ---mh_view     hand drawing data, every frame
  ---mh_info     status, stats, error, picture url, finger layout
  ---mh_page     strip parameters shown in the editor page ("param notes 1")
  ---mh_move     Movement switch (0/1), gates map slots and CC out
  ---mh_notes_set, ---mh_move_set   the page flips a strip switch
  ---mh_strip_report                the page opened: strip switches resend
  ---mh_audio    sound values for the pages (from every MidiHands Audio, via mh-audio-hub.js)

Every MidiHands Audio device sends on the global name mh_audio (no "---":
all devices in the Set hear it): "v <letter> <level bass mid high beat>"
about 60 times a second and "n <letter> <id> <track name>" every second.
"""
from __future__ import annotations

import json
import re
import struct
import sys
from pathlib import Path

EXPRESSIONS = ["L Height", "L X", "L Pinch", "L Fist", "L Tilt",
               "R Height", "R X", "R Pinch", "R Fist", "R Tilt"]  # order = core/engine.hpp Expr
SCALES = ["Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
          "Harmonic Minor", "Melodic Minor", "Major Pentatonic", "Minor Pentatonic",
          "Blues", "Chromatic"]  # order = max/mh.hands.mm scaleTypes()
NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
LINKS = 16  # movement -> Live parameter links; any number may share a movement
FX_SLOTS = 4  # video effect chain length; each slot: effect, mix, six knobs, a modulation per control
VIDEO_SIZE = (960.0, 540.0)
FORMATS = ["16:9", "9:16", "1:1", "4:5"]
AUDIO_LETTERS = "ABCDEFGH"  # MidiHands Audio devices send as one of these
AUDIO_FEATURES = ["Level", "Bass", "Mid", "High", "Beat"]  # order = core/audio.hpp AudioFeatures
# Modulation sources: none, the ten hand movements, then letter x feature (append only:
# Sets store the index).
MOD_SOURCES = ["None"] + EXPRESSIONS + [f"{l} {f}" for l in AUDIO_LETTERS for f in AUDIO_FEATURES]


def effect_names() -> list[str]:
    """Effect names in the order of package/javascript/mh-fx.js (the effect menu's order)."""
    src = (Path(__file__).resolve().parent.parent / "package" / "javascript" / "mh-fx.js").read_text()
    return [m.group(2) for m in re.finditer(r'add\("([a-z0-9]+)", "([^"]+)", "([A-Za-z]+)"', src)]

EDITOR_SIZE = (1200.0, 760.0)
VERSION = (Path(__file__).resolve().parent.parent / "VERSION").read_text().strip()

# (inlets, outlets, outlet types) for object classes used below.
PORTS = {
    "midiin": (1, 1, ["int"]), "midiout": (1, 0, []), "iter": (1, 1, [""]),
    "mh.hands": (1, 4, ["list", "list", "", ""]),
    "live.thisdevice": (1, 3, ["bang", "int", "int"]),
    "prepend": (1, 1, [""]), "pak": (2, 1, [""]), "gate": (2, 1, [""]),
    "==": (2, 1, ["int"]), "/": (2, 1, ["float"]), "+": (2, 1, ["int"]),
    "i": (2, 1, ["int"]), "line~": (2, 2, ["signal", "bang"]),
    "live.remote~": (2, 0, []), "live.path": (1, 3, ["", "", ""]),
    "live.observer": (2, 2, ["", ""]), "pcontrol": (1, 1, [""]),
    "s": (1, 0, []), "r": (0, 1, [""]), "inlet": (0, 1, [""]),
    "expr": (2, 1, [""]), "change": (1, 3, ["", "int", "int"]), "thispatcher": (1, 2, ["", ""]),
    "onebang": (2, 2, ["bang", "bang"]), "absolutepath": (1, 1, [""]),
    "v8": (1, 1, [""]), "metro": (2, 1, ["bang"]), "delay": (2, 1, ["bang"]),
    "plugin~": (2, 2, ["signal", "signal"]), "plugout~": (2, 0, []),
    "mh.audio~": (2, 2, ["", ""]),
}


class Patch:
    def __init__(self) -> None:
        self.boxes: list[dict] = []
        self.lines: list[dict] = []
        self.params: dict[str, list] = {}

    _n = 0  # ids are unique across all patches of the device

    def _add(self, box: dict) -> str:
        Patch._n += 1
        box["id"] = f"obj-{Patch._n}"
        box.setdefault("fontname", "Arial Bold")
        box.setdefault("fontsize", 10.0)
        self.boxes.append({"box": box})
        return box["id"]

    def obj(self, text: str, x: float, y: float) -> str:
        name = text.split()[0]
        if name in ("t", "trigger"):
            outs = len(text.split()) - 1
            ports = (1, outs, [""] * outs)
        elif name in ("route", "sel", "routepass"):
            outs = len(text.split())
            ports = (2, outs, [""] * outs)
        elif name == "zl":
            ports = (2, 2, ["", ""])
        else:
            ports = PORTS[name]
        return self._add({
            "maxclass": "newobj", "text": text,
            "numinlets": ports[0], "numoutlets": ports[1], "outlettype": ports[2],
            "patching_rect": [x, y, max(40.0, 7.0 * len(text)), 20.0],
        })

    def msg(self, text: str, x: float, y: float) -> str:
        return self._add({
            "maxclass": "message", "text": text, "numinlets": 2, "numoutlets": 1,
            "outlettype": [""], "patching_rect": [x, y, max(30.0, 7.0 * len(text)), 20.0],
        })

    def label(self, text: str, rect: list[float], size: float = 10.0) -> str:
        return self._add({
            "maxclass": "live.comment", "text": text, "numinlets": 1, "numoutlets": 0,
            "fontsize": size, "patching_rect": [rect[0] + 1400, rect[1] + 700, rect[2], rect[3]],
            "presentation": 1, "presentation_rect": rect, "textjustification": 0,
        })

    def panel(self, rect: list[float], color: list[float]) -> str:
        return self._add({
            "maxclass": "panel", "numinlets": 1, "numoutlets": 0, "background": 1,
            "bgcolor": color, "bgfillcolor_type": "color", "bgfillcolor_color": color,
            "border": 0, "rounded": 4,
            "patching_rect": [rect[0] + 1400, rect[1] + 1400, rect[2], rect[3]],
            "presentation": 1, "presentation_rect": rect,
        })

    def param(self, maxclass: str, longname: str, shortname: str, rect: list[float],
              x: float, y: float, valueof: dict, ports: tuple, **extra) -> str:
        valueof = {"parameter_longname": longname, "parameter_shortname": shortname,
                   "parameter_linknames": 1, **valueof}
        box = {
            "maxclass": maxclass, "numinlets": ports[0], "numoutlets": ports[1],
            "outlettype": ports[2], "parameter_enable": 1,
            "patching_rect": [x, y, rect[2], rect[3]],
            "presentation": 1, "presentation_rect": rect,
            "saved_attribute_attributes": {"valueof": valueof},
            "varname": longname, **extra,
        }
        oid = self._add(box)
        self.params[oid] = [longname, shortname, 0]
        return oid

    def menu(self, longname, shortname, items, initial, rect, x, y, **extra) -> str:
        return self.param("live.menu", longname, shortname, rect, x, y, {
            "parameter_type": 2, "parameter_enum": items, "parameter_mmax": len(items) - 1,
            "parameter_initial_enable": 1, "parameter_initial": [initial],
        }, (1, 3, ["", "", "float"]), **extra)

    def tab(self, longname, shortname, items, initial, rect, x, y) -> str:
        return self.param("live.tab", longname, shortname, rect, x, y, {
            "parameter_type": 2, "parameter_enum": items, "parameter_mmax": len(items) - 1,
            "parameter_initial_enable": 1, "parameter_initial": [initial],
        }, (1, 3, ["", "", "float"]), num_lines_patching=1, num_lines_presentation=1)

    def toggle_text(self, longname, shortname, text, initial, rect, x, y) -> str:
        return self.param("live.text", longname, shortname, rect, x, y, {
            "parameter_type": 2, "parameter_enum": ["off", "on"], "parameter_mmax": 1,
            "parameter_initial_enable": 1, "parameter_initial": [initial],
        }, (1, 2, ["", ""]), text=text, texton=text, mode=1)

    def dial(self, longname, shortname, lo, hi, initial, unitstyle, rect, x, y, integer=False) -> str:
        return self.param("live.dial", longname, shortname, rect, x, y, {
            "parameter_type": 1 if integer else 0, "parameter_mmin": float(lo), "parameter_mmax": float(hi),
            "parameter_unitstyle": unitstyle, "parameter_initial_enable": 1, "parameter_initial": [initial],
        }, (1, 2, ["", "float"]))

    def numbox(self, longname, shortname, lo, hi, initial, unitstyle, rect, x, y) -> str:
        return self.param("live.numbox", longname, shortname, rect, x, y, {
            "parameter_type": 1, "parameter_mmin": float(lo), "parameter_mmax": float(hi),
            "parameter_unitstyle": unitstyle, "parameter_initial_enable": 1, "parameter_initial": [initial],
        }, (1, 2, ["", "float"]))

    def connect(self, src: str, outlet: int, dst: str, inlet: int = 0) -> None:
        self.lines.append({"patchline": {"source": [src, outlet], "destination": [dst, inlet]}})

    def patcher(self, rect: list[float], **extra) -> dict:
        return {
            "fileversion": 1,
            "appversion": {"major": 9, "minor": 1, "revision": 4, "architecture": "x64", "modernui": 1},
            "classnamespace": "box", "rect": rect, "bglocked": 0, "openinpresentation": 1,
            "default_fontsize": 10.0, "default_fontface": 0, "default_fontname": "Arial Bold",
            "gridonopen": 1, "gridsize": [8.0, 8.0], "gridsnaponopen": 1, "objectsnaponopen": 1,
            "statusbarvisible": 2, "toolbarvisible": 1,
            "lefttoolbarpinned": 0, "toptoolbarpinned": 0, "righttoolbarpinned": 0,
            "bottomtoolbarpinned": 0, "toolbars_unpinned_last_save": 0, "tallnewobj": 0,
            "boxanimatetime": 500, "enablehscroll": 1, "enablevscroll": 1, "devicewidth": 0.0,
            "description": "", "digest": "", "tags": "", "style": "", "subpatcher_template": "",
            "boxes": self.boxes, "lines": self.lines, **extra,
        }


# Settings stored as hidden Live parameters. The editor page shows and edits
# them; Live saves them with the Set, automates the visible ones and shows
# them on Push. (key, long name, type, lo, hi, initial, visibility, enum)
# type: "int" | "float" | "ms" | "enum"; visibility: 0 automatable, 1 stored only.
# Max for Live int parameters span at most 256 steps, so wider ranges are floats.
def stored_params() -> list[tuple]:
    params = [
        ("layout", "Layout", "enum", 0, 3, 2, 0, ["Keys", "Chords", "Split", "Custom"]),
        ("hands", "Hands", "enum", 0, 2, 0, 0, ["Both", "Left", "Right"]),
        ("follow", "Follow Live Scale", "enum", 0, 1, 1, 0, ["off", "on"]),
        ("root", "Root", "enum", 0, 11, 0, 0, NOTES),
        ("scale", "Scale", "enum", 0, 12, 0, 0, SCALES),
        ("sens", "Sensitivity", "float", 0, 100, 50.0, 0, None),
        ("vel", "Velocity", "int", 1, 127, 100, 0, None),
        ("velmode", "Velocity Mode", "enum", 0, 2, 0, 0, ["Fixed", "Height", "Speed"]),
        ("oct", "Octave", "int", -3, 3, 0, 0, None),
        ("minnote", "Min Note Length", "ms", 0, 1000, 180.0, 0, None),
        ("hold", "Dropout Hold", "ms", 0, 2000, 300.0, 0, None),
        ("smooth", "Movement Smoothing", "ms", 0, 500, 50.0, 0, None),
        ("chan", "MIDI Channel", "int", 1, 16, 1, 0, None),
        ("ccout", "CC Out", "enum", 0, 1, 0, 0, ["off", "on"]),
        ("ccbase", "CC Base", "int", 0, 118, 20, 0, None),
    ]
    split = [(2, 0, -1), (2, 3, -1), (2, 4, -1), (2, 5, -1), (1, 0, 0), (1, 1, 0), (1, 2, 0), (1, 4, 0)]
    for k, (mode, deg, octv) in enumerate(split):
        params += [
            (f"f{k}mode", f"Finger {k + 1} Mode", "enum", 0, 2, mode, 1, ["Off", "Note", "Chord"]),
            (f"f{k}deg", f"Finger {k + 1} Degree", "int", -14, 21, deg, 1, None),
            (f"f{k}oct", f"Finger {k + 1} Octave", "int", -3, 3, octv, 1, None),
        ]
    for k in range(LINKS):
        params += [
            (f"m{k}on", f"Link {k + 1} On", "enum", 0, 1, 1, 0, ["off", "on"]),
            (f"m{k}src", f"Link {k + 1} Source", "enum", 0, 9, 0, 1, EXPRESSIONS),
            (f"m{k}lo", f"Link {k + 1} In Low", "float", 0, 1, 0.0, 1, None),
            (f"m{k}hi", f"Link {k + 1} In High", "float", 0, 1, 1.0, 1, None),
            (f"m{k}curve", f"Link {k + 1} Curve", "int", -100, 100, 0, 1, None),
            (f"m{k}min", f"Link {k + 1} Min", "float", 0, 1, 0.0, 0, None),
            (f"m{k}max", f"Link {k + 1} Max", "float", 0, 1, 1.0, 0, None),
        ]
    params += [
        ("vlook", "Hands Look", "enum", 0, 2, 2, 0, ["Off", "Lines", "Jelly"]),
        ("vtheme", "Hands Colors", "enum", 0, 1, 0, 1, ["Live", "Amber"]),
        ("vcam", "Camera Level", "float", 0, 1, 1.0, 0, None),
        ("vfxh", "FX On Hands", "enum", 0, 1, 1, 1, ["off", "on"]),
        ("vcues", "Gesture Cues", "enum", 0, 1, 1, 1, ["off", "on"]),
        ("vfmt", "Video Format", "enum", 0, 3, 0, 1, FORMATS),
    ]
    effects = ["None"] + effect_names()
    for s in range(FX_SLOTS):
        n = s + 1
        params += [
            (f"x{s}fx", f"FX{n} Effect", "enum", 0, len(effects) - 1, 0, 0, effects),
            (f"x{s}mix", f"FX{n} Mix", "float", 0, 1, 1.0, 0, None),
        ]
        params += [(f"x{s}p{k}", f"FX{n} Knob {k + 1}", "float", 0, 1, 0.5, 0, None) for k in range(6)]
        for j in range(7):  # 0 = mix, 1..6 = knobs
            what = "Mix" if j == 0 else f"Knob {j}"
            params += [
                (f"x{s}m{j}s", f"FX{n} {what} Mod Source", "enum", 0, len(MOD_SOURCES) - 1, 0, 1, MOD_SOURCES),
                (f"x{s}m{j}a", f"FX{n} {what} Mod Amount", "float", -1, 1, 0.5, 1, None),
            ]
    return params


# Settings that go straight to mh.hands: key -> message (sens is scaled first).
ENGINE_MESSAGES = {
    "layout": "layout", "hands": "hands", "vel": "velocity", "velmode": "velmode", "oct": "octave",
    "minnote": "minnote", "hold": "hold", "smooth": "smoothing", "chan": "channel", "ccbase": "ccbase",
}


def build_editor() -> tuple[Patch, dict]:
    e = Patch()
    W, H = EDITOR_SIZE
    e.obj("inlet", 20, 10)  # pcontrol in the device opens this window through it
    to_hands = e.obj("s ---mh_in", 20, 2000)

    ui = e._add({
        "maxclass": "jweb", "numinlets": 1, "numoutlets": 1, "outlettype": [""],
        "patching_rect": [20.0, 40.0, 300.0, 200.0], "presentation": 1,
        "presentation_rect": [0.0, 0.0, W, H], "rendermode": 1, "url": "", "varname": "ui",  # 1 = onscreen (fast)
    })

    # Opening the window: fixed size, and load the page the first time.
    opened = e.obj("r ---mh_open", 340, 20)
    open_steps = e.obj("t b b", 340, 50)
    e.connect(opened, 0, open_steps)
    # Fixed size, and floating above Live like a plugin window, so clicking a
    # parameter to map it does not hide the editor.
    flags = e.msg("window flags nogrow, window flags nozoom, window flags float, window exec", 340, 80)
    e.connect(open_steps, 1, flags)
    tp = e.obj("thispatcher", 340, 110)
    e.connect(flags, 0, tp)
    once = e.obj("onebang 1", 520, 80)
    e.connect(open_steps, 0, once)
    page = e.msg("mh-editor.html", 520, 110)
    e.connect(once, 0, page)
    locate = e.obj("absolutepath", 520, 140)
    e.connect(page, 0, locate)
    read = e.obj("prepend readfile", 520, 170)
    e.connect(locate, 0, read)
    e.connect(read, 0, ui)

    # Live data into the page.
    for i, name in enumerate(("r ---mh_view", "r ---mh_info", "r ---mh_audio")):
        src = e.obj(name, 340 + 120 * i, 200)
        e.connect(src, 0, ui)
    expr_in = e.obj("r ---mh_expr", 600, 200)
    expr_msg = e.obj("prepend expr", 600, 230)
    e.connect(expr_in, 0, expr_msg)
    e.connect(expr_msg, 0, ui)

    # Page commands: set <key> <value>, link <k>, unlink <k>, hello, update,
    # video (open the video window), record (start or stop recording it).
    commands = e.obj("route set link unlink hello update video record reveal", 20, 260)
    reveal = e.msg("reveal", 940, 260)
    e.connect(commands, 7, reveal)
    e.connect(reveal, 0, to_hands)
    update_msg = e.msg("update", 600, 260)
    e.connect(commands, 4, update_msg)
    e.connect(update_msg, 0, to_hands)
    open_video = e.obj("t b", 700, 260)
    e.connect(commands, 5, open_video)
    e.connect(open_video, 0, e.obj("s ---mh_vopen", 700, 290))
    # Record from the editor: open the video window, then let its page start
    # (it knows the picture's rectangle to capture).
    record_steps = e.obj("t b b", 820, 260)
    e.connect(commands, 6, record_steps)
    e.connect(record_steps, 1, e.obj("s ---mh_vopen", 860, 290))
    rec_wait = e.obj("delay 700", 820, 320)
    e.connect(record_steps, 0, rec_wait)
    rec_req = e.msg("recreq", 820, 350)
    e.connect(rec_wait, 0, rec_req)
    e.connect(rec_req, 0, e.obj("s ---mh_vreq", 820, 380))
    # The video page's commands arrive here too.
    video_cmds = e.obj("route set hello record reveal", 1000, 230)
    e.connect(e.obj("r ---mh_vcmd", 1000, 200), 0, video_cmds)
    video_rec = e.obj("prepend record", 1100, 260)
    e.connect(video_cmds, 2, video_rec)
    e.connect(video_rec, 0, to_hands)
    video_reveal = e.msg("reveal", 1200, 260)
    e.connect(video_cmds, 3, video_reveal)
    e.connect(video_reveal, 0, to_hands)
    e.connect(ui, 0, commands)

    # Stored parameters: hidden live.numbox objects.
    params = stored_params()
    keys = [p[0] for p in params]
    strip_keys = e.obj("route notes move", 20, 280)  # master switches live on the strip
    e.connect(commands, 0, strip_keys)
    e.connect(video_cmds, 0, strip_keys)
    for i, name in enumerate(("s ---mh_notes_set", "s ---mh_move_set")):
        e.connect(strip_keys, i, e.obj(name, 160 + 130 * i, 280))
    setter = e.obj("route " + " ".join(keys), 20, 300)
    e.connect(strip_keys, 2, setter)
    resend = e.obj("t b", 20, 330)
    boot = e.obj("r ---mh_boot", 120, 330)
    e.connect(boot, 0, resend)
    hello_steps = e.obj("t b b", 200, 330)
    e.connect(commands, 3, hello_steps)
    e.connect(video_cmds, 1, hello_steps)
    e.connect(hello_steps, 1, resend)
    e.connect(resend, 0, e.obj("s ---mh_strip_report", 120, 360))
    e.connect(e.obj("r ---mh_page", 600, 170), 0, ui)
    move_in = e.obj("r ---mh_move", 800, 170)
    report = e.msg("report", 200, 360)
    e.connect(hello_steps, 0, report)
    e.connect(report, 0, to_hands)

    to_video_params = e.obj("s ---mh_param", 1300, 380)  # the video window shows the same settings
    numbox = {}
    for n, (key, longname, kind, lo, hi, init, invisible, enum) in enumerate(params):
        x, y = 20 + (n % 12) * 150, 420 + (n // 12) * 130
        valueof = {"parameter_initial_enable": 1, "parameter_initial": [init],
                   "parameter_invisible": invisible}
        if kind == "enum":
            valueof.update({"parameter_type": 2, "parameter_enum": enum, "parameter_mmax": len(enum) - 1})
        else:
            valueof.update({"parameter_type": 1 if kind == "int" else 0,
                            "parameter_mmin": float(lo), "parameter_mmax": float(hi),
                            "parameter_unitstyle": {"int": 0, "float": 1, "ms": 2}[kind]})
        box = e.param("live.numbox", longname, key, [x, y, 60.0, 15.0], x, y, valueof, (1, 2, ["", "float"]))
        e.boxes[-1]["box"].pop("presentation")
        e.boxes[-1]["box"].pop("presentation_rect")
        numbox[key] = box
        e.connect(setter, n, box)
        e.connect(resend, 0, box)
        echo = e.obj(f"prepend param {key}", x, y + 30)
        e.connect(box, 0, echo)
        e.connect(echo, 0, ui)
        e.connect(echo, 0, to_video_params)
        if key in ENGINE_MESSAGES:
            prep = e.obj(f"prepend {ENGINE_MESSAGES[key]}", x, y + 60)
            e.connect(box, 0, prep)
            e.connect(prep, 0, to_hands)
        elif key == "sens":
            scale = e.obj("/ 100.", x, y + 60)
            e.connect(box, 0, scale)
            prep = e.obj("prepend sensitivity", x, y + 90)
            e.connect(scale, 0, prep)
            e.connect(prep, 0, to_hands)
        elif key[0] == "f" and key[1].isdigit():
            k, field = key[1], key[2:]
            msg = {"mode": "fingermode", "deg": "fingerdeg", "oct": "fingeroct"}[field]
            prep = e.obj(f"prepend {msg} {k}", x, y + 60)
            e.connect(box, 0, prep)
            e.connect(prep, 0, to_hands)

    # CC out only while Movement is on.
    cc_pak = e.obj("pak 0 0", 1700, 420)
    e.connect(numbox["ccout"], 0, cc_pak, 0)
    e.connect(move_in, 0, cc_pak, 1)
    cc_both = e.obj("expr $i1 && $i2", 1700, 450)
    e.connect(cc_pak, 0, cc_both)
    cc_msg = e.obj("prepend ccout", 1700, 480)
    e.connect(cc_both, 0, cc_msg)
    e.connect(cc_msg, 0, to_hands)

    # When a preset fills the finger table, store it without echoing back.
    info_in = e.obj("r ---mh_info", 1900, 20)
    table = e.obj("route fingertable", 1900, 50)
    e.connect(info_in, 0, table)
    unpack = e._add({"maxclass": "newobj", "text": "unpack " + " ".join(["0"] * 24),
                     "numinlets": 1, "numoutlets": 24, "outlettype": ["int"] * 24,
                     "patching_rect": [1900, 80, 600, 20]})
    e.connect(table, 0, unpack)
    for k in range(8):
        for j, field in enumerate(("mode", "deg", "oct")):
            silent = e.obj("prepend set", 1900 + (k * 3 + j) * 70, 110)
            e.connect(unpack, k * 3 + j, silent)
            e.connect(silent, 0, numbox[f"f{k}{field}"])

    # Key: Live's scale or the manual root/scale settings.
    root_gate = e.obj("gate 1 1", 1900, 200)
    scale_gate = e.obj("gate 1 1", 2000, 200)
    e.connect(numbox["root"], 0, root_gate, 1)
    e.connect(numbox["scale"], 0, scale_gate, 1)
    root_prep = e.obj("prepend root", 1900, 400)
    e.connect(root_gate, 0, root_prep)
    e.connect(root_prep, 0, to_hands)
    scaletype_prep = e.obj("prepend scaletype", 2000, 400)
    e.connect(scale_gate, 0, scaletype_prep)
    e.connect(scaletype_prep, 0, to_hands)
    path_msg = e.msg("path live_set", 2100, 230)
    lpath = e.obj("live.path", 2100, 260)
    e.connect(path_msg, 0, lpath)
    wire = e.obj("t b b l", 2100, 290)
    e.connect(lpath, 0, wire)
    obs_root = e.obj("live.observer", 2100, 350)
    obs_scale = e.obj("live.observer", 2220, 350)
    e.connect(wire, 2, obs_root, 1)
    e.connect(wire, 2, obs_scale, 1)
    prop_root = e.msg("property root_note", 2100, 320)
    prop_scale = e.msg("property scale_intervals", 2220, 320)
    e.connect(wire, 1, prop_scale)
    e.connect(wire, 0, prop_root)
    e.connect(prop_root, 0, obs_root)
    e.connect(prop_scale, 0, obs_scale)
    live_root_gate = e.obj("gate 1 1", 2100, 380)
    live_scale_gate = e.obj("gate 1 1", 2220, 380)
    e.connect(obs_root, 0, live_root_gate, 1)
    e.connect(obs_scale, 0, live_scale_gate, 1)
    e.connect(live_root_gate, 0, root_prep)
    live_scale_prep = e.obj("prepend scale", 2220, 410)
    e.connect(live_scale_gate, 0, live_scale_prep)
    e.connect(live_scale_prep, 0, to_hands)
    follow_split = e.obj("t i i", 1900, 260)
    e.connect(numbox["follow"], 0, follow_split)
    e.connect(follow_split, 1, live_root_gate, 0)
    e.connect(follow_split, 1, live_scale_gate, 0)
    manual = e.obj("== 0", 1960, 290)
    e.connect(follow_split, 1, manual)
    e.connect(manual, 0, root_gate, 0)
    e.connect(manual, 0, scale_gate, 0)
    pick = e.obj("sel 1 0", 1900, 320)
    e.connect(follow_split, 0, pick)
    e.connect(pick, 0, path_msg)
    refresh = e.obj("t b b", 1900, 350)
    e.connect(pick, 1, refresh)
    e.connect(refresh, 1, numbox["scale"])
    e.connect(refresh, 0, numbox["root"])

    # Links: movement -> range/curve -> Live parameter. mh-links.js names the
    # targets and knows the parameter selected in Live, so "link <k>" links to
    # it at once; with nothing selected the link listens for a click instead
    # (live.map). Each target id is kept by a live.object with Live's
    # persistence, so links come back with the Set.
    links_js = e.obj("v8 mh-links.js", 1000, 2040)
    link_cmd = e.obj("prepend link", 1000, 2010)
    e.connect(commands, 1, link_cmd)
    e.connect(link_cmd, 0, links_js)
    links_report = e.msg("report", 1100, 2010)
    e.connect(hello_steps, 0, links_report)
    e.connect(links_report, 0, links_js)
    # Device ready: start the helper, read back stored targets, then let
    # links attach (live.remote~ must not be touched before Live's API is up).
    links_boot = e.obj("t b b b", 1200, 1980)
    e.connect(e.obj("r ---mh_boot", 1200, 1950), 0, links_boot)
    links_init = e.msg("init", 1260, 2010)
    e.connect(links_boot, 2, links_init)
    e.connect(links_init, 0, links_js)
    js_out = e.obj("route setid listen", 1000, 2070)
    e.connect(links_js, 0, js_out)
    e.connect(js_out, 2, ui)  # mapname <k> <text>, selected <text>
    slot_names = " ".join(str(k) for k in range(LINKS))
    setid_route = e.obj("route " + slot_names, 1000, 2100)
    e.connect(js_out, 0, setid_route)
    listen_route = e.obj("route " + slot_names, 1300, 2100)
    e.connect(js_out, 1, listen_route)
    unlink_route = e.obj("route " + slot_names, 400, 2100)
    e.connect(commands, 2, unlink_route)
    for k in range(LINKS):
        bx, by = 20 + 240 * (k % 8), 2200 + 640 * (k // 8)
        # movement value -> shaped 0..1 -> parameter
        index = e.obj("+ 1", bx, by)
        e.connect(numbox[f"m{k}src"], 0, index)
        pick_expr = e.obj("zl nth 1", bx, by + 30)
        e.connect(index, 0, pick_expr, 1)
        e.connect(expr_in, 0, pick_expr)
        shape = e._add({
            "maxclass": "newobj",
            "text": "expr $f5 + ($f6 - $f5) * pow(min(max(($f1 - $f2) / max($f3 - $f2\\, 0.001)\\, 0.)\\, 1.)\\, pow(2.\\, $f4 / 50.))",
            "numinlets": 6, "numoutlets": 1, "outlettype": [""],
            "patching_rect": [bx, by + 60, 220, 20],
        })
        e.connect(pick_expr, 0, shape)
        for inlet, key in enumerate(("lo", "hi", "curve", "min", "max"), start=1):
            e.connect(numbox[f"m{k}{key}"], 0, shape, inlet)
        ramp = e.msg("$1 20", bx, by + 90)
        e.connect(shape, 0, ramp)
        line = e.obj("line~", bx, by + 120)
        e.connect(ramp, 0, line)
        remote = e.obj("live.remote~ @normalized 1", bx, by + 150)
        e.connect(line, 0, remote, 0)

        # target: "id N" -> persistent live.object -> getid -> everyone
        set_target = e.obj("t b l", bx + 120, by + 180)
        e.connect(setid_route, k, set_target)
        target = e._add({"maxclass": "newobj", "text": "live.object", "numinlets": 2, "numoutlets": 1,
                         "outlettype": [""], "patching_rect": [bx + 120, by + 240, 80, 20],
                         "saved_object_attributes": {"_persistence": 1}})
        e.connect(set_target, 1, target, 1)
        getid = e.msg("getid", bx + 120, by + 210)
        e.connect(set_target, 0, getid)
        e.connect(links_boot, 1, getid)
        e.connect(getid, 0, target)
        got = e.obj("t l l", bx + 120, by + 270)
        e.connect(target, 0, got)
        name_req = e.obj(f"prepend label {k}", bx + 120, by + 300)
        e.connect(got, 0, name_req)
        e.connect(name_req, 0, links_js)

        # click-to-link when nothing is selected in Live
        lmap = e._add({"maxclass": "newobj", "text": "live.map @strict 1", "numinlets": 1, "numoutlets": 5,
                       "outlettype": ["", "", "", "", ""], "patching_rect": [bx, by + 210, 110, 20]})
        start_map = e.msg("mapping 1", bx, by + 180)
        e.connect(listen_route, k, start_map)
        e.connect(start_map, 0, lmap)
        clicked = e.obj("route id", bx, by + 240)
        e.connect(lmap, 1, clicked)
        real = e.obj("sel 0", bx, by + 270)  # unmapping is handled below, never by live.map
        e.connect(clicked, 0, real)
        as_id = e.obj("prepend id", bx, by + 300)
        e.connect(real, 1, as_id)
        e.connect(as_id, 0, set_target)
        state_msg = e.obj(f"prepend mapping {k}", bx, by + 330)
        e.connect(lmap, 3, state_msg)
        e.connect(state_msg, 0, ui)
        unlink = e.obj("t b b", bx + 60, by + 360)
        e.connect(unlink_route, k, unlink)
        cancel = e.msg("mapping 0", bx + 60, by + 390)
        e.connect(unlink, 1, cancel)
        e.connect(cancel, 0, lmap)
        clear = e.msg("id 0", bx + 140, by + 390)
        e.connect(unlink, 0, clear)
        e.connect(clear, 0, set_target)

        # A link drives its parameter only while Movement and the link are on.
        # Off detaches live.remote~ (id 0), so the parameter can be turned by
        # hand and its automation plays; on re-attaches the stored target.
        target_id = e.obj("zl reg", bx, by + 420)
        e.connect(got, 1, target_id)
        attach = e.obj("gate 1 0", bx, by + 450)
        e.connect(target_id, 0, attach, 1)
        e.connect(attach, 0, remote, 1)
        on_pak = e.obj("pak 0 0", bx, by + 480)
        e.connect(move_in, 0, on_pak, 0)
        e.connect(numbox[f"m{k}on"], 0, on_pak, 1)
        on_both = e.obj("expr $i1 && $i2", bx, by + 510)
        e.connect(on_pak, 0, on_both)
        ready_gate = e.obj("gate 1 0", bx + 120, by + 510)
        e.connect(on_both, 0, ready_gate, 1)
        ready = e.obj("t b b", bx + 120, by + 480)
        e.connect(links_boot, 0, ready)
        open_ready = e.msg("1", bx + 160, by + 450)
        e.connect(ready, 1, open_ready)
        e.connect(open_ready, 0, ready_gate, 0)
        e.connect(ready, 0, on_pak)  # pak resends its pair on bang
        on_change = e.obj("change -1", bx, by + 540)
        e.connect(ready_gate, 0, on_change)
        on_steps = e.obj("t i i", bx, by + 570)
        e.connect(on_change, 0, on_steps)
        e.connect(on_steps, 1, attach, 0)
        on_pick = e.obj("sel 1 0", bx, by + 600)
        e.connect(on_steps, 0, on_pick)
        e.connect(on_pick, 0, target_id)
        detach = e.msg("id 0", bx + 80, by + 630)
        e.connect(on_pick, 1, detach)
        e.connect(detach, 0, remote, 1)

    window = [80.0, 80.0, W, H]  # x, y, width, height
    return e, e.patcher(window, toolbarvisible=0, statusbarvisible=0, enablehscroll=0, enablevscroll=0,
                        title=f"MidiHands {VERSION}", bgcolor=[0.141, 0.141, 0.141, 1.0],
                        editing_bgcolor=[0.141, 0.141, 0.141, 1.0])


def build_video() -> dict:
    """The video window: camera, effects and hands full size, to watch, put on a
    projector, or record. Its page is mh-video.html."""
    v = Patch()
    W, H = VIDEO_SIZE
    v.obj("inlet", 20, 10)
    ui = v._add({
        "maxclass": "jweb", "numinlets": 1, "numoutlets": 1, "outlettype": [""],
        "patching_rect": [20.0, 300.0, 300.0, 200.0], "presentation": 1,
        "presentation_rect": [0.0, 0.0, W, H], "rendermode": 1, "url": "", "varname": "video",
    })
    tp = v.obj("thispatcher", 400, 600)
    opened = v.obj("r ---mh_vopen", 400, 20)
    steps = v.obj("t b b", 400, 50)
    v.connect(opened, 0, steps)
    # Floating like the editor, so Live's window never hides it.
    flags = v.msg("window flags grow, window flags zoom, window flags float, window exec", 400, 80)
    v.connect(steps, 1, flags)
    v.connect(flags, 0, tp)
    once = v.obj("onebang 1", 620, 80)
    v.connect(steps, 0, once)
    page = v.msg("mh-video.html", 620, 110)
    v.connect(once, 0, page)
    locate = v.obj("absolutepath", 620, 140)
    v.connect(page, 0, locate)
    read = v.obj("prepend readfile", 620, 170)
    v.connect(locate, 0, read)
    v.connect(read, 0, ui)
    # Pcontrol "open" arrives through the inlet; nothing else to do with it.

    for i, name in enumerate(("r ---mh_view", "r ---mh_info", "r ---mh_page", "r ---mh_param", "r ---mh_vreq", "r ---mh_audio")):
        v.connect(v.obj(name, 20 + 110 * i, 220), 0, ui)
    expr_in = v.obj("r ---mh_expr", 580, 220)
    expr_msg = v.obj("prepend expr", 580, 250)
    v.connect(expr_in, 0, expr_msg)
    v.connect(expr_msg, 0, ui)

    # Page commands: fullscreen and place here, everything else to the editor patch.
    cmds = v.obj("route fullscreen place", 20, 530)
    v.connect(ui, 0, cmds)
    v.connect(cmds, 2, v.obj("s ---mh_vcmd", 160, 560))
    place = v.msg("window size $1 $2 $3 $4, window exec", 500, 560)
    v.connect(cmds, 1, place)
    v.connect(place, 0, tp)
    full = v.obj("route 1 0", 20, 560)
    v.connect(cmds, 0, full)
    # Window rectangle (left top right bottom), polled to keep the page filling the window.
    current = v.obj("zl reg", 600, 420)
    saved = v.obj("zl reg", 700, 470)
    enter = v.obj("t l b", 20, 590)
    v.connect(full, 0, enter)
    v.connect(enter, 1, current)              # remember the window before covering the screen
    v.connect(current, 0, saved, 1)
    go_full = v.msg("window flags notitle, window size $1 $2 $3 $4, window exec", 20, 620)
    v.connect(enter, 0, go_full)
    v.connect(go_full, 0, tp)
    v.connect(full, 1, saved)
    leave = v.msg("window flags title, window size $1 $2 $3 $4, window exec", 260, 620)
    v.connect(saved, 0, leave)
    v.connect(leave, 0, tp)

    metro = v.obj("metro 250", 400, 330)
    start = v.msg("1", 400, 300)
    v.connect(v.obj("r ---mh_boot", 400, 270), 0, start)
    v.connect(start, 0, metro)
    ask = v.msg("window getsize", 400, 360)
    v.connect(metro, 0, ask)
    v.connect(ask, 0, tp)
    is_window = v.obj("route window", 400, 650)
    v.connect(tp, 0, is_window)
    is_size = v.obj("route size", 400, 680)
    v.connect(is_window, 0, is_size)
    v.connect(is_size, 0, current, 1)
    split = v.obj("t l l", 400, 710)
    v.connect(is_size, 0, split)
    width = v.obj("expr $i3 - $i1", 400, 740)
    height = v.obj("expr $i4 - $i2", 520, 740)
    v.connect(split, 1, width)
    v.connect(split, 0, height)
    size = v.obj("pak 0 0", 400, 770)
    v.connect(width, 0, size, 0)
    v.connect(height, 0, size, 1)
    changed = v.obj("zl change", 400, 800)
    v.connect(size, 0, changed)
    resize = v.obj("prepend script sendbox video presentation_rect 0 0", 400, 830)
    v.connect(changed, 0, resize)
    v.connect(resize, 0, tp)

    return v.patcher([120.0, 120.0, W, H], toolbarvisible=0, statusbarvisible=0, enablehscroll=0, enablevscroll=0,
                     title="MidiHands Video", bgcolor=[0.0, 0.0, 0.0, 1.0], editing_bgcolor=[0.0, 0.0, 0.0, 1.0])


def build() -> dict:
    Patch._n = 0
    p = Patch()

    # --- core ------------------------------------------------------------
    midiin = p.obj("midiin", 20, 20)
    midiout = p.obj("midiout", 20, 520)
    p.connect(midiin, 0, midiout)
    hands = p.obj("mh.hands", 20, 400)
    it = p.obj("iter", 20, 460)
    p.connect(hands, 0, it)
    p.connect(it, 0, midiout)
    settings_in = p.obj("r ---mh_in", 20, 360)
    p.connect(settings_in, 0, hands)

    expr_out = p.obj("s ---mh_expr", 100, 460)
    p.connect(hands, 1, expr_out)
    view_out = p.obj("s ---mh_view", 300, 460)
    p.connect(hands, 2, view_out)

    # --- strip: hand view + camera ---------------------------------------
    view = p._add({
        "maxclass": "v8ui", "filename": "mh-view.js", "parameter_enable": 0,
        "numinlets": 1, "numoutlets": 1, "outlettype": [""],
        "patching_rect": [400, 460, 220, 156],
        "presentation": 1, "presentation_rect": [4.0, 6.0, 230.0, 156.0],
    })
    info = p.obj("route cameras", 420, 420)
    p.connect(hands, 3, info)
    p.connect(hands, 2, view)
    view_info = p.obj("routepass status stats error", 420, 450)  # all the strip view draws
    p.connect(info, 1, view_info)
    p.connect(view_info, 0, view)
    p.connect(view_info, 1, view)
    p.connect(view_info, 2, view)
    info_out = p.obj("s ---mh_info", 520, 460)
    p.connect(info, 1, info_out)

    device = p.obj("live.thisdevice", 600, 20)
    boot = p.obj("t b b b", 600, 50)
    p.connect(device, 0, boot)
    cams_msg = p.msg("cameras", 700, 80)
    p.connect(boot, 2, cams_msg)
    p.connect(cams_msg, 0, hands)
    boot_out = p.obj("s ---mh_boot", 640, 80)
    p.connect(boot, 1, boot_out)

    x0 = 242.0
    cam_on = p.toggle_text("Camera On", "Camera", "Camera", 1, [x0, 8.0, 56.0, 18.0], 20, 80)
    cam_menu = p.menu("Camera Device", "Device", ["No camera"], 0, [x0 + 60, 8.0, 136.0, 18.0], 120, 80)
    set_range = p.obj("prepend _parameter_range", 420, 470)
    p.connect(info, 0, set_range)
    p.connect(set_range, 0, cam_menu)
    on_store = p.obj("i", 20, 110)
    p.connect(cam_on, 0, on_store, 1)
    pak = p.obj("pak 0 0", 20, 140)
    p.connect(cam_on, 0, pak, 0)
    p.connect(on_store, 0, pak, 0)
    p.connect(cam_menu, 0, pak, 1)
    switch = p.obj("route 0 1", 20, 170)
    p.connect(pak, 0, switch)
    close = p.msg("close", 20, 200)
    p.connect(switch, 0, close)
    p.connect(close, 0, hands)
    opener = p.obj("prepend open", 80, 200)
    p.connect(switch, 1, opener)
    p.connect(opener, 0, hands)
    p.connect(boot, 0, pak)

    # --- the editor window ---------------------------------------------------
    open_button = p._add({
        "maxclass": "live.text", "text": "Open Editor", "texton": "Open Editor", "mode": 0,
        "numinlets": 1, "numoutlets": 2, "outlettype": ["", ""], "parameter_enable": 0,
        "fontsize": 12.0, "patching_rect": [200, 80, 90, 30],
        "presentation": 1, "presentation_rect": [x0, 34.0, 196.0, 44.0],
    })
    # Master switches: notes from fingers, movement to mapped parameters and CC.
    for i, (longname, key, msg) in enumerate((("Notes", "notes", "notes"), ("Movement", "move", None))):
        tog = p.toggle_text(longname, longname, longname, 1, [x0 + 100.0 * i, 84.0, 96.0, 22.0], 800 + 160 * i, 80)
        if msg:
            to_engine = p.obj(f"prepend {msg}", 800 + 160 * i, 110)
            p.connect(tog, 0, to_engine)
            p.connect(to_engine, 0, hands)
        else:
            p.connect(tog, 0, p.obj("s ---mh_move", 800 + 160 * i, 110))
        # Tell the page; an opened page asks again. A bang would flip the
        # toggle, so the report repeats a stored copy instead.
        echo = p.obj(f"prepend param {key}", 800 + 160 * i, 140)
        p.connect(tog, 0, echo)
        p.connect(echo, 0, p.obj("s ---mh_page", 800 + 160 * i, 170))
        last = p.obj("i", 900 + 160 * i, 110)
        p.connect(tog, 0, last, 1)
        p.connect(p.obj("r ---mh_strip_report", 900 + 160 * i, 80), 0, last)
        p.connect(last, 0, echo)
        p.connect(p.obj(f"r ---mh_{key}_set", 800 + 160 * i, 50), 0, tog)
    open_msg = p.msg("open", 200, 120)
    p.connect(open_button, 0, open_msg)
    pcontrol = p.obj("pcontrol", 200, 150)
    p.connect(open_msg, 0, pcontrol)

    editor, editor_patcher = build_editor()
    editor_box = p._add({
        "maxclass": "newobj", "text": "p MidiHands", "numinlets": 1, "numoutlets": 0,
        "patching_rect": [200, 180, 90, 20], "patcher": editor_patcher,
    })
    p.connect(pcontrol, 0, editor_box)

    video_open = p.msg("open", 400, 120)
    p.connect(p.obj("r ---mh_vopen", 400, 90), 0, video_open)
    video_pcontrol = p.obj("pcontrol", 400, 150)
    p.connect(video_open, 0, video_pcontrol)
    video_box = p._add({
        "maxclass": "newobj", "text": "p MidiHandsVideo", "numinlets": 1, "numoutlets": 0,
        "patching_rect": [400, 180, 110, 20], "patcher": build_video(),
    })
    p.connect(video_pcontrol, 0, video_box)

    opened = p.obj("s ---mh_open", 300, 120)
    p.connect(open_button, 0, opened)

    # Sound from MidiHands Audio devices anywhere in the Set, for the effects.
    hub = p.obj("v8 mh-audio-hub.js", 600, 300)
    p.connect(p.obj("r mh_audio", 600, 270), 0, hub)
    p.connect(hub, 0, p.obj("s ---mh_audio", 600, 330))

    params = {**p.params, **editor.params, "inherited_shortname": 1}
    patcher = p.patcher([40.0, 80.0, 1500.0, 700.0], openrect=[0.0, 0.0, 0.0, 169.0],
                        description=f"MidiHands {VERSION}: camera hand tracking to MIDI", title="MidiHands",
                        parameters=params, dependency_cache=[], latency=0, autosave=0,
                        # Without a project entry Max logs "a project without a name ... fatal".
                        project={
                            "version": 1, "creationdate": 3590052786, "modificationdate": 3590052786,
                            "viewrect": [0.0, 0.0, 300.0, 500.0], "autoorganize": 1, "hideprojectwindow": 1,
                            "showdependencies": 1, "autolocalize": 0, "contents": {"patchers": {}},
                            "layout": {}, "searchpath": {}, "detailsvisible": 0,
                            "amxdtype": 1835887981,  # 'mmmm', MIDI effect
                            "readonly": 0, "devpathtype": 0, "devpath": ".", "sortmode": 0, "viewmode": 0,
                        })
    return {"patcher": patcher}


def build_audio() -> dict:
    """MidiHands Audio: an audio effect that passes its track's sound through
    unchanged and sends level, bass, mid, high and beat to every MidiHands
    device as one of eight letters, for the video effects."""
    Patch._n = 0
    p = Patch()
    W = 330.0

    pin = p.obj("plugin~", 20, 20)
    pout = p.obj("plugout~", 20, 300)
    p.connect(pin, 0, pout)
    p.connect(pin, 1, pout, 1)
    analysis = p.obj("mh.audio~", 120, 200)
    p.connect(pin, 0, analysis)
    p.connect(pin, 1, analysis, 1)
    p.connect(analysis, 0, p.obj("s mh_audio", 120, 240))

    # Letter this device sends as, and its meters.
    p.label("Sends as", [8.0, 4.0, 60.0, 14.0], 9.0)
    letter = p.tab("Letter", "Letter", list(AUDIO_LETTERS), 0, [8.0, 18.0, 168.0, 17.0], 300, 20)
    channel = p.obj("prepend channel", 300, 50)
    p.connect(letter, 0, channel)
    p.connect(channel, 0, analysis)
    meters = p._add({
        "maxclass": "multislider", "numinlets": 1, "numoutlets": 2, "outlettype": ["", ""],
        "parameter_enable": 0, "size": 5, "setminmax": [0.0, 1.0], "setstyle": 1, "thickness": 4,
        "slidercolor": [1.0, 0.68, 0.34, 1.0], "candicane2": [1.0, 0.68, 0.34, 1.0],
        "bgcolor": [0.10, 0.10, 0.10, 1.0], "ignoreclick": 1, "spacing": 4,
        "patching_rect": [120, 280, 168, 70], "presentation": 1, "presentation_rect": [8.0, 42.0, 168.0, 72.0],
    })
    p.connect(analysis, 1, meters)
    for i, name in enumerate(AUDIO_FEATURES):
        p.label(name, [8.0 + i * 34.0, 115.0, 34.0, 14.0], 9.0)
    name_label = p._add({
        "maxclass": "live.comment", "text": "", "numinlets": 1, "numoutlets": 0, "fontsize": 9.0,
        "patching_rect": [520, 200, 168, 18], "presentation": 1, "presentation_rect": [8.0, 134.0, 168.0, 16.0],
        "textjustification": 0,
    })

    # Settings, right column.
    x = 186.0
    controls = [
        ("Gain", "gain", lambda r, xx, yy: p.dial("Gain", "Gain", -24, 24, 0, 4, r, xx, yy)),
        ("Smooth", "smooth", lambda r, xx, yy: p.dial("Smooth", "Smooth", 20, 1000, 120, 2, r, xx, yy)),
        ("Beat Sensitivity", "sensitivity", lambda r, xx, yy: p.dial("Beat Sensitivity", "Beat Sens", 0, 100, 50, 5, r, xx, yy)),
        ("Beat Decay", "decay", lambda r, xx, yy: p.dial("Beat Decay", "Decay", 50, 1500, 250, 2, r, xx, yy)),
    ]
    for i, (_, msg, make) in enumerate(controls):
        rect = [x + (i % 2) * 70.0, 4.0 + (i // 2) * 56.0, 64.0, 52.0]
        dial = make(rect, 500 + 110 * i, 20)
        src = dial
        if msg == "sensitivity":
            src = p.obj("/ 100.", 500 + 110 * i, 50)
            p.connect(dial, 0, src)
        prep = p.obj(f"prepend {msg}", 500 + 110 * i, 80)
        p.connect(src, 0, prep)
        p.connect(prep, 0, analysis)
    auto = p.toggle_text("Auto Level", "Auto Level", "Auto Level", 1, [x, 124.0, 134.0, 18.0], 950, 20)
    auto_prep = p.obj("prepend autolevel", 950, 50)
    p.connect(auto, 0, auto_prep)
    p.connect(auto_prep, 0, analysis)

    # The track's name, followed live, so MidiHands can list "A · Kick".
    device = p.obj("live.thisdevice", 520, 120)
    path_msg = p.msg("path this_device canonical_parent", 520, 145)
    p.connect(device, 0, path_msg)
    lpath = p.obj("live.path", 520, 170)
    p.connect(path_msg, 0, lpath)
    observer = p.obj("live.observer", 520, 230)
    prop = p.msg("property name", 640, 200)
    wire = p.obj("t l b", 520, 200)
    p.connect(lpath, 0, wire)
    p.connect(wire, 1, prop)
    p.connect(prop, 0, observer)
    p.connect(wire, 0, observer, 1)
    name_prep = p.obj("prepend name", 520, 260)
    p.connect(observer, 0, name_prep)
    p.connect(name_prep, 0, analysis)
    label_prep = p.obj("prepend set", 640, 260)
    p.connect(observer, 0, label_prep)
    p.connect(label_prep, 0, name_label)

    patcher = p.patcher([40.0, 80.0, 1200.0, 600.0], openrect=[0.0, 0.0, W, 169.0],
                        description=f"MidiHands Audio {VERSION}: this track's sound for MidiHands' video effects",
                        title="MidiHands Audio", parameters={**p.params, "inherited_shortname": 1},
                        dependency_cache=[], latency=0, autosave=0, devicewidth=W,
                        project={
                            "version": 1, "creationdate": 3590052786, "modificationdate": 3590052786,
                            "viewrect": [0.0, 0.0, 300.0, 500.0], "autoorganize": 1, "hideprojectwindow": 1,
                            "showdependencies": 1, "autolocalize": 0, "contents": {"patchers": {}},
                            "layout": {}, "searchpath": {}, "detailsvisible": 0,
                            "amxdtype": 1633771873,  # 'aaaa', audio effect
                            "readonly": 0, "devpathtype": 0, "devpath": ".", "sortmode": 0, "viewmode": 0,
                        })
    return {"patcher": patcher}


def write_amxd(patch: dict, path: Path, kind: bytes = b"mmmm") -> None:
    """kind: b"mmmm" MIDI effect, b"aaaa" audio effect."""
    data = json.dumps(patch, indent="\t").encode("utf-8") + b"\n\x00"
    header = b"ampf" + struct.pack("<I", 4) + kind
    meta = b"meta" + struct.pack("<I", 4) + b"\x00\x00\x00\x00"
    ptch = b"ptch" + struct.pack("<I", len(data)) + data
    path.write_bytes(header + meta + ptch)


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("MidiHands.amxd")
    write_amxd(build(), out)
    print(f"wrote {out}")
    audio_out = out.with_name("MidiHands Audio.amxd")
    write_amxd(build_audio(), audio_out, b"aaaa")
    print(f"wrote {audio_out}")
