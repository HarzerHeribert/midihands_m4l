#!/usr/bin/env python3
"""Generates MidiHands.amxd, the Max for Live MIDI effect.

The patch is described in code so changes are reviewable in git. Run
`make device` (or this script) after editing; open the result in Live.

Signal flow:
  midiin -> midiout                         (incoming MIDI passes through)
  mh.hands -> iter -> midiout               (finger notes)
  mh.hands expressions -> 4 map slots       (liveui.map + live.remote~)
  live.* controls -> mh.hands settings
  Live's scale (root_note, scale_intervals) -> mh.hands when "Live Scale" is on
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

EXPRESSIONS = ["L Height", "L X", "L Pinch", "L Fist", "L Tilt",
               "R Height", "R X", "R Pinch", "R Fist", "R Tilt"]  # order = core/engine.hpp Expr
SCALES = ["Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
          "Harmonic Minor", "Melodic Minor", "Major Pentatonic", "Minor Pentatonic",
          "Blues", "Chromatic"]  # order = max/mh.hands.mm scaleTypes()
NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MAP_DEFAULTS = [5, 7, 0, 3]  # R Height, R Pinch, L Height, L Fist

# (inlets, outlets, outlet types) for object classes used below.
PORTS = {
    "midiin": (1, 1, ["int"]), "midiout": (1, 0, []), "iter": (1, 1, [""]),
    "mh.hands": (1, 4, ["list", "list", "", ""]),
    "live.thisdevice": (1, 3, ["bang", "int", "int"]),
    "prepend": (1, 1, [""]), "pak": (2, 1, [""]), "gate": (2, 1, [""]),
    "==": (2, 1, ["int"]), "/": (2, 1, ["float"]), "+": (2, 1, ["int"]),
    "i": (2, 1, ["int"]), "line~": (2, 2, ["signal", "bang"]),
    "live.remote~": (2, 0, []), "live.path": (1, 3, ["", "", ""]),
    "live.observer": (2, 2, ["", ""]),
}


class Patch:
    def __init__(self) -> None:
        self.boxes: list[dict] = []
        self.lines: list[dict] = []
        self.params: dict[str, list] = {}
        self._n = 0

    def _id(self) -> str:
        self._n += 1
        return f"obj-{self._n}"

    def _add(self, box: dict) -> str:
        box["id"] = self._id()
        box.setdefault("fontname", "Arial Bold")
        box.setdefault("fontsize", 10.0)
        self.boxes.append({"box": box})
        return box["id"]

    def obj(self, text: str, x: float, y: float) -> str:
        name = text.split()[0]
        if name in ("t", "trigger"):
            outs = len(text.split()) - 1
            ports = (1, outs, [""] * outs)
        elif name in ("route", "sel"):
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

    def label(self, text: str, rect: list[float]) -> str:
        return self._add({
            "maxclass": "live.comment", "text": text, "numinlets": 1, "numoutlets": 0,
            "patching_rect": [rect[0] + 800, rect[1] + 600, rect[2], rect[3]],
            "presentation": 1, "presentation_rect": rect, "textjustification": 0,
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

    def toggle_text(self, longname, shortname, text_off, text_on, initial, rect, x, y) -> str:
        return self.param("live.text", longname, shortname, rect, x, y, {
            "parameter_type": 2, "parameter_enum": ["off", "on"], "parameter_mmax": 1,
            "parameter_initial_enable": 1, "parameter_initial": [initial],
        }, (1, 2, ["", ""]), text=text_off, texton=text_on, mode=1)

    def connect(self, src: str, outlet: int, dst: str, inlet: int = 0) -> None:
        self.lines.append({"patchline": {"source": [src, outlet], "destination": [dst, inlet]}})


def build() -> dict:
    p = Patch()

    # --- core ------------------------------------------------------------
    midiin = p.obj("midiin", 20, 20)
    midiout = p.obj("midiout", 20, 520)
    p.connect(midiin, 0, midiout)
    hands = p.obj("mh.hands", 20, 400)
    it = p.obj("iter", 20, 460)
    p.connect(hands, 0, it)
    p.connect(it, 0, midiout)

    # --- hand view + status ---------------------------------------------
    view = p._add({
        "maxclass": "v8ui", "filename": "mh-view.js", "parameter_enable": 0,
        "numinlets": 1, "numoutlets": 1, "outlettype": [""],
        "patching_rect": [400, 460, 196, 150],
        "presentation": 1, "presentation_rect": [4.0, 6.0, 196.0, 156.0],
    })
    info = p.obj("route cameras", 300, 440)
    p.connect(hands, 3, info)
    p.connect(hands, 2, view)
    p.connect(info, 1, view)  # status, stats, error

    # --- lifecycle: device loaded -> list cameras, resend settings, open --
    device = p.obj("live.thisdevice", 600, 20)
    boot = p.obj("t b b b", 600, 50)
    p.connect(device, 0, boot)
    cams_msg = p.msg("cameras", 700, 80)
    p.connect(boot, 2, cams_msg)
    p.connect(cams_msg, 0, hands)

    # Camera on/off + camera menu -> open <index> / close.
    x0 = 210.0
    cam_on = p.toggle_text("Camera On", "Camera", "Camera", "Camera", 1,
                           [x0, 8.0, 46.0, 15.0], 20, 80)
    cam_menu = p.menu("Camera Device", "Device", ["No camera"], 0,
                      [x0 + 50, 8.0, 126.0, 15.0], 120, 80)
    set_range = p.obj("prepend _parameter_range", 300, 470)
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

    # --- playing settings -------------------------------------------------
    layout = p.param("live.tab", "Layout", "Layout", [x0, 28.0, 176.0, 15.0], 200, 80, {
        "parameter_type": 2, "parameter_enum": ["Keys", "Chords", "Split"], "parameter_mmax": 2,
        "parameter_initial_enable": 1, "parameter_initial": [2],
    }, (1, 3, ["", "", "float"]), num_lines_patching=1, num_lines_presentation=1)
    settings: list[tuple[str, str]] = [(layout, "prepend layout")]

    follow = p.toggle_text("Follow Live Scale", "Live Scale", "Live Scale", "Live Scale", 1,
                           [x0, 48.0, 58.0, 15.0], 300, 80)
    root = p.menu("Root", "Root", NOTES, 0, [x0 + 62, 48.0, 34.0, 15.0], 400, 80)
    scale = p.menu("Scale", "Scale", SCALES, 0, [x0 + 100, 48.0, 76.0, 15.0], 480, 80)

    sens = p.param("live.dial", "Sensitivity", "Sens", [x0, 70.0, 44.0, 48.0], 200, 140, {
        "parameter_type": 0, "parameter_mmin": 0.0, "parameter_mmax": 100.0,
        "parameter_unitstyle": 5, "parameter_initial_enable": 1, "parameter_initial": [50.0],
    }, (1, 2, ["", "float"]))
    sens_scale = p.obj("/ 100.", 200, 200)
    p.connect(sens, 0, sens_scale)
    settings.append((sens_scale, "prepend sensitivity"))

    velocity = p.param("live.dial", "Velocity", "Velocity", [x0 + 46, 70.0, 44.0, 48.0], 260, 140, {
        "parameter_type": 1, "parameter_mmin": 1.0, "parameter_mmax": 127.0,
        "parameter_unitstyle": 0, "parameter_initial_enable": 1, "parameter_initial": [100],
    }, (1, 2, ["", "float"]))
    settings.append((velocity, "prepend velocity"))

    p.label("Vel", [x0 + 94, 70.0, 24.0, 15.0])
    velmode = p.menu("Velocity Mode", "Vel Mode", ["Fixed", "Height", "Speed"], 0,
                     [x0 + 118, 70.0, 58.0, 15.0], 320, 140)
    settings.append((velmode, "prepend velmode"))
    p.label("Oct", [x0 + 94, 88.0, 24.0, 15.0])
    octave = p.param("live.numbox", "Octave", "Octave", [x0 + 118, 88.0, 58.0, 15.0], 400, 140, {
        "parameter_type": 1, "parameter_mmin": -3.0, "parameter_mmax": 3.0,
        "parameter_unitstyle": 0, "parameter_initial_enable": 1, "parameter_initial": [0],
    }, (1, 2, ["", "float"]))
    settings.append((octave, "prepend octave"))
    cc_out = p.toggle_text("CC Out", "CC Out", "CC Out", "CC Out", 0,
                           [x0 + 118, 106.0, 58.0, 15.0], 480, 140)
    cc_store = p.obj("i", 480, 170)
    p.connect(cc_out, 0, cc_store, 1)
    cc_prep = p.obj("prepend ccout", 480, 200)
    p.connect(cc_out, 0, cc_prep)
    p.connect(cc_store, 0, cc_prep)
    p.connect(cc_prep, 0, hands)
    p.label("CC Out sends the movements as CC 20-29", [x0, 124.0, 180.0, 15.0])

    resend = p.obj("t " + " ".join(["b"] * 11), 200, 260)  # one outlet per stored setting
    p.connect(boot, 1, resend)
    for src, prep_text in settings:
        prep = p.obj(prep_text, 200 + 80 * settings.index((src, prep_text)), 230)
        p.connect(src, 0, prep)
        p.connect(prep, 0, hands)
    for n, target in enumerate([layout, sens, velocity, velmode, octave, cc_store]):
        p.connect(resend, n, target)
    p.connect(boot, 0, pak)

    # --- scale: Live's scale or the manual menus --------------------------
    root_gate = p.obj("gate 1 1", 400, 110)
    scale_gate = p.obj("gate 1 1", 480, 110)
    p.connect(root, 0, root_gate, 1)
    p.connect(scale, 0, scale_gate, 1)
    root_prep = p.obj("prepend root", 400, 300)
    p.connect(root_gate, 0, root_prep)
    p.connect(root_prep, 0, hands)
    scaletype_prep = p.obj("prepend scaletype", 480, 300)
    p.connect(scale_gate, 0, scaletype_prep)
    p.connect(scaletype_prep, 0, hands)

    path_msg = p.msg("path live_set", 600, 330)
    lpath = p.obj("live.path", 600, 360)
    p.connect(path_msg, 0, lpath)
    wire = p.obj("t b b l", 600, 390)
    p.connect(lpath, 0, wire)
    obs_root = p.obj("live.observer", 600, 450)
    obs_scale = p.obj("live.observer", 720, 450)
    p.connect(wire, 2, obs_root, 1)
    p.connect(wire, 2, obs_scale, 1)
    prop_root = p.msg("property root_note", 600, 420)
    prop_scale = p.msg("property scale_intervals", 720, 420)
    p.connect(wire, 1, prop_scale)
    p.connect(wire, 0, prop_root)
    p.connect(prop_root, 0, obs_root)
    p.connect(prop_scale, 0, obs_scale)
    live_root_gate = p.obj("gate 1 1", 600, 480)
    live_scale_gate = p.obj("gate 1 1", 720, 480)
    p.connect(obs_root, 0, live_root_gate, 1)
    p.connect(obs_scale, 0, live_scale_gate, 1)
    p.connect(live_root_gate, 0, root_prep)
    live_scale_prep = p.obj("prepend scale", 720, 510)
    p.connect(live_scale_gate, 0, live_scale_prep)
    p.connect(live_scale_prep, 0, hands)

    follow_store = p.obj("i", 300, 110)
    p.connect(follow, 0, follow_store, 1)
    follow_split = p.obj("t i i", 300, 140)
    p.connect(follow, 0, follow_split)
    p.connect(follow_store, 0, follow_split)
    p.connect(resend, 6, follow_store)
    # Right outlet first: set gates, gray out the manual menus.
    p.connect(follow_split, 1, live_root_gate, 0)
    p.connect(follow_split, 1, live_scale_gate, 0)
    manual = p.obj("== 0", 360, 170)
    p.connect(follow_split, 1, manual)
    p.connect(manual, 0, root_gate, 0)
    p.connect(manual, 0, scale_gate, 0)
    active = p.obj("prepend active", 360, 200)
    p.connect(manual, 0, active)
    p.connect(active, 0, root)
    p.connect(active, 0, scale)
    # Then pull current values from the chosen source.
    pick = p.obj("sel 1 0", 300, 200)
    p.connect(follow_split, 0, pick)
    p.connect(pick, 0, path_msg)
    refresh_menus = p.obj("t b b", 300, 230)
    p.connect(pick, 1, refresh_menus)
    p.connect(refresh_menus, 1, scale)
    p.connect(refresh_menus, 0, root)

    # --- expression map slots ---------------------------------------------
    mx = 396.0
    p.label("Map hand movement to any parameter", [mx, 8.0, 230.0, 15.0])
    for k in range(4):
        y = 28.0 + 22.0 * k
        src = p.menu(f"Map {k + 1} Source", f"Src {k + 1}", EXPRESSIONS, MAP_DEFAULTS[k],
                     [mx, y, 64.0, 15.0], 800 + 160 * k, 80)
        index = p.obj("+ 1", 800 + 160 * k, 110)
        p.connect(src, 0, index)
        pick_expr = p.obj("zl nth 1", 800 + 160 * k, 140)
        p.connect(index, 0, pick_expr, 1)
        p.connect(hands, 1, pick_expr)
        ramp = p.msg("$1 20", 800 + 160 * k, 170)
        p.connect(pick_expr, 0, ramp)
        line = p.obj("line~", 800 + 160 * k, 200)
        p.connect(ramp, 0, line)
        mapper = p._add({
            "maxclass": "bpatcher", "name": "liveui.map.maxpat", "embed": 0,
            "numinlets": 1, "numoutlets": 2, "outlettype": ["signal", ""],
            "offset": [0.0, 0.0], "bgmode": 0, "border": 0, "clickthrough": 0,
            "enablehscroll": 0, "enablevscroll": 0, "lockeddragscroll": 0, "viewvisibility": 1,
            "patching_rect": [800 + 160 * k, 230, 160, 16],
            "presentation": 1, "presentation_rect": [mx + 68, y, 160.0, 16.0],
            "varname": f"map{k + 1}",
        })
        p.connect(line, 0, mapper)
        remote = p.obj("live.remote~ @normalized 1", 800 + 160 * k, 270)
        p.connect(mapper, 0, remote, 0)
        p.connect(mapper, 1, remote, 1)
        p.connect(resend, 7 + k, src)

    return {
        "patcher": {
            "fileversion": 1,
            "appversion": {"major": 9, "minor": 1, "revision": 4, "architecture": "x64", "modernui": 1},
            "classnamespace": "box",
            "rect": [40.0, 80.0, 1500.0, 700.0],
            "openrect": [0.0, 0.0, 0.0, 169.0],
            "bglocked": 0, "openinpresentation": 1,
            "default_fontsize": 10.0, "default_fontface": 0, "default_fontname": "Arial Bold",
            "gridonopen": 1, "gridsize": [8.0, 8.0], "gridsnaponopen": 1, "objectsnaponopen": 1,
            "statusbarvisible": 2, "toolbarvisible": 1,
            "lefttoolbarpinned": 0, "toptoolbarpinned": 0, "righttoolbarpinned": 0,
            "bottomtoolbarpinned": 0, "toolbars_unpinned_last_save": 0, "tallnewobj": 0,
            "boxanimatetime": 500, "enablehscroll": 1, "enablevscroll": 1, "devicewidth": 0.0,
            "description": "Camera hand tracking to MIDI", "digest": "", "tags": "",
            "style": "", "subpatcher_template": "", "title": "MidiHands",
            "boxes": p.boxes, "lines": p.lines,
            "parameters": {**p.params, "inherited_shortname": 1},
            "dependency_cache": [], "latency": 0, "autosave": 0,
        }
    }


def write_amxd(patch: dict, path: Path) -> None:
    data = json.dumps(patch, indent="\t").encode("utf-8") + b"\n\x00"
    header = b"ampf" + struct.pack("<I", 4) + b"mmmm"  # mmmm = MIDI effect
    meta = b"meta" + struct.pack("<I", 4) + b"\x00\x00\x00\x00"
    ptch = b"ptch" + struct.pack("<I", len(data)) + data
    path.write_bytes(header + meta + ptch)


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("MidiHands.amxd")
    write_amxd(build(), out)
    print(f"wrote {out}")
