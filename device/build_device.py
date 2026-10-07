#!/usr/bin/env python3
"""Generates MidiHands.amxd, the Max for Live MIDI effect.

The patch is described in code so changes are reviewable in git. Run
`make device` (or this script) after editing; open the result in Live.

Two views:
  strip   the device on the track: hand view, camera on/off and choice,
          and a button that opens the editor
  editor  a separate, movable window (subpatcher "MidiHands"): camera
          picture, every setting, movement meters and eight map slots

The two halves talk through device-local send/receive names; Max for Live
replaces the "---" prefix with an id unique to each device instance:
  ---mh_in       settings and commands -> mh.hands
  ---mh_boot     device finished loading (resend stored settings)
  ---mh_expr     ten hand-movement values, every frame
  ---mh_picture  camera picture (jit_matrix)
  ---mh_info     status, stats, error
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
MAP_SLOTS = 8
MAP_DEFAULTS = [5, 7, 0, 3, 6, 9, 1, 4]  # R Height, R Pinch, L Height, L Fist, R X, R Tilt, L X, L Tilt

EDITOR_SIZE = (1210.0, 610.0)
PICTURE = (16.0, 40.0, 640.0, 360.0)

# (inlets, outlets, outlet types) for object classes used below.
PORTS = {
    "midiin": (1, 1, ["int"]), "midiout": (1, 0, []), "iter": (1, 1, [""]),
    "mh.hands": (1, 5, ["list", "list", "", "jit_matrix", ""]),
    "live.thisdevice": (1, 3, ["bang", "int", "int"]),
    "prepend": (1, 1, [""]), "pak": (2, 1, [""]), "gate": (2, 1, [""]),
    "==": (2, 1, ["int"]), "/": (2, 1, ["float"]), "+": (2, 1, ["int"]),
    "i": (2, 1, ["int"]), "line~": (2, 2, ["signal", "bang"]),
    "live.remote~": (2, 0, []), "live.path": (1, 3, ["", "", ""]),
    "live.observer": (2, 2, ["", ""]), "pcontrol": (1, 1, [""]),
    "s": (1, 0, []), "r": (0, 1, [""]), "inlet": (0, 1, [""]),
    "expr": (2, 1, [""]), "change": (1, 3, ["", "int", "int"]),
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


def setting(p: Patch, src: str, outlet: int, text: str, x: float, y: float, to_hands: str) -> str:
    """src -> [prepend text] -> mh.hands (through ---mh_in)."""
    prep = p.obj(f"prepend {text}", x, y)
    p.connect(src, outlet, prep)
    p.connect(prep, 0, to_hands)
    return prep


def stored_toggle(p: Patch, toggle: str, x: float, y: float) -> tuple[str, str]:
    """live.text toggles flip on bang, so their value is kept in [i] for resending.
    Returns (store, out): bang `store` to resend; `out` carries every value."""
    store = p.obj("i", x, y)
    out = p.obj("t i", x, y + 30)
    p.connect(toggle, 0, store, 1)
    p.connect(toggle, 0, out)
    p.connect(store, 0, out)
    return store, out


def build_editor() -> tuple[Patch, dict]:
    e = Patch()
    W, H = EDITOR_SIZE
    e.panel([0.0, 0.0, W, H], [0.13, 0.13, 0.13, 1.0])
    for rect in ([8.0, 8.0, 656.0, H - 16], [672.0, 8.0, 216.0, H - 16], [896.0, 8.0, W - 904, H - 16]):
        e.panel(rect, [0.17, 0.17, 0.17, 1.0])

    inlet = e.obj("inlet", 20, 20)  # for pcontrol; carries nothing
    to_hands = e.obj("s ---mh_in", 20, 640)
    boot = e.obj("r ---mh_boot", 300, 20)
    resend = e.obj("t " + " ".join(["b"] * 30), 300, 50)
    p_resend = iter(range(30))
    e.connect(boot, 0, resend)

    def resend_to(target: str) -> None:
        e.connect(resend, next(p_resend), target)

    # --- camera picture + status/meters ------------------------------------
    e.label("CAMERA", [16.0, 14.0, 80.0, 18.0], 12.0)
    picture_toggle = e.toggle_text("Show Picture", "Picture", "Show Picture", 1,
                                   [566.0, 14.0, 90.0, 18.0], 20, 60)
    pic_store, pic_out = stored_toggle(e, picture_toggle, 20, 90)
    resend_to(pic_store)
    pic_prep = e.obj("s ---mh_picture_on", 20, 150)
    e.connect(pic_out, 0, pic_prep)
    picture_in = e.obj("r ---mh_picture", 120, 60)
    pwindow = e._add({
        "maxclass": "jit.pwindow", "numinlets": 1, "numoutlets": 2, "outlettype": ["jit_matrix", ""],
        "patching_rect": [120, 90, 160, 90], "presentation": 1, "presentation_rect": list(PICTURE),
        "border": 0.0, "sync": 1,
    })
    e.connect(picture_in, 0, pwindow)
    panel_view = e._add({
        "maxclass": "v8ui", "filename": "mh-editor.js", "parameter_enable": 0,
        "numinlets": 1, "numoutlets": 1, "outlettype": [""],
        "patching_rect": [120, 200, 320, 100],
        "presentation": 1, "presentation_rect": [16.0, 408.0, 640.0, H - 424],
    })
    info_in = e.obj("r ---mh_info", 120, 320)
    e.connect(info_in, 0, panel_view)

    # --- play -------------------------------------------------------------
    x0 = 684.0
    e.label("PLAY", [x0, 14.0, 80.0, 18.0], 12.0)
    e.label("Layout", [x0, 40.0, 192.0, 15.0])
    layout = e.tab("Layout", "Layout", ["Keys", "Chords", "Split"], 2, [x0, 56.0, 192.0, 20.0], 500, 60)
    setting(e, layout, 0, "layout", 500, 90, to_hands)
    resend_to(layout)
    e.label("Hands that play notes", [x0, 82.0, 192.0, 15.0])
    hands = e.tab("Hands", "Hands", ["Both", "Left", "Right"], 0, [x0, 98.0, 192.0, 20.0], 600, 60)
    setting(e, hands, 0, "hands", 600, 90, to_hands)
    resend_to(hands)

    e.label("Key", [x0, 124.0, 192.0, 15.0])
    follow = e.toggle_text("Follow Live Scale", "Live Scale", "Live Scale", 1,
                           [x0, 140.0, 64.0, 18.0], 700, 60)
    root = e.menu("Root", "Root", NOTES, 0, [x0 + 68, 140.0, 38.0, 18.0], 800, 60)
    scale = e.menu("Scale", "Scale", SCALES, 0, [x0 + 110, 140.0, 82.0, 18.0], 880, 60)

    sens = e.dial("Sensitivity", "Sens", 0, 100, 50.0, 5, [x0, 168.0, 44.0, 48.0], 500, 140)
    sens_scale = e.obj("/ 100.", 500, 200)
    e.connect(sens, 0, sens_scale)
    setting(e, sens_scale, 0, "sensitivity", 500, 230, to_hands)
    resend_to(sens)
    velocity = e.dial("Velocity", "Velocity", 1, 127, 100, 0, [x0 + 50, 168.0, 44.0, 48.0], 580, 140, integer=True)
    setting(e, velocity, 0, "velocity", 580, 230, to_hands)
    resend_to(velocity)
    e.label("Vel mode", [x0 + 100, 168.0, 92.0, 15.0])
    velmode = e.menu("Velocity Mode", "Vel Mode", ["Fixed", "Height", "Speed"], 0,
                     [x0 + 100, 184.0, 92.0, 18.0], 660, 140)
    setting(e, velmode, 0, "velmode", 660, 230, to_hands)
    resend_to(velmode)
    e.label("Octave", [x0 + 100, 206.0, 44.0, 15.0])
    octave = e.numbox("Octave", "Octave", -3, 3, 0, 0, [x0 + 146, 206.0, 46.0, 16.0], 740, 140)
    setting(e, octave, 0, "octave", 740, 230, to_hands)
    resend_to(octave)

    # --- timing -----------------------------------------------------------
    e.label("TIMING", [x0, 240.0, 120.0, 18.0], 12.0)
    for k, (longname, shortname, lo, hi, init, msg) in enumerate([
        ("Min Note Length", "Min Note", 0, 1000, 180.0, "minnote"),
        ("Dropout Hold", "Hold", 0, 2000, 300.0, "hold"),
        ("Movement Smoothing", "Smooth", 0, 500, 50.0, "smoothing"),
    ]):
        d = e.dial(longname, shortname, lo, hi, init, 2, [x0 + 64 * k, 262.0, 54.0, 48.0], 500 + 80 * k, 280)
        setting(e, d, 0, msg, 500 + 80 * k, 340, to_hands)
        resend_to(d)

    # --- midi -------------------------------------------------------------
    e.label("MIDI", [x0, 324.0, 120.0, 18.0], 12.0)
    e.label("Channel", [x0, 348.0, 60.0, 15.0])
    channel = e.numbox("MIDI Channel", "Channel", 1, 16, 1, 0, [x0 + 64, 348.0, 46.0, 16.0], 760, 280)
    setting(e, channel, 0, "channel", 760, 340, to_hands)
    resend_to(channel)
    cc_out = e.toggle_text("CC Out", "CC Out", "Send CC", 0, [x0, 372.0, 60.0, 18.0], 840, 280)
    cc_store, cc_val = stored_toggle(e, cc_out, 840, 310)
    setting(e, cc_val, 0, "ccout", 840, 370, to_hands)
    resend_to(cc_store)
    e.label("from CC", [x0 + 64, 374.0, 44.0, 15.0])
    cc_base = e.numbox("CC Base", "CC Base", 0, 118, 20, 0, [x0 + 110, 372.0, 46.0, 16.0], 920, 280)
    setting(e, cc_base, 0, "ccbase", 920, 340, to_hands)
    resend_to(cc_base)
    e.label("Sends the ten movements as CCs, e.g. for MIDI learn.", [x0, 396.0, 196.0, 30.0])

    # --- scale: Live's scale or the manual menus --------------------------
    root_gate = e.obj("gate 1 1", 800, 110)
    scale_gate = e.obj("gate 1 1", 880, 110)
    e.connect(root, 0, root_gate, 1)
    e.connect(scale, 0, scale_gate, 1)
    root_prep = e.obj("prepend root", 800, 400)
    e.connect(root_gate, 0, root_prep)
    e.connect(root_prep, 0, to_hands)
    scaletype_prep = e.obj("prepend scaletype", 880, 400)
    e.connect(scale_gate, 0, scaletype_prep)
    e.connect(scaletype_prep, 0, to_hands)

    path_msg = e.msg("path live_set", 1000, 330)
    lpath = e.obj("live.path", 1000, 360)
    e.connect(path_msg, 0, lpath)
    wire = e.obj("t b b l", 1000, 390)
    e.connect(lpath, 0, wire)
    obs_root = e.obj("live.observer", 1000, 450)
    obs_scale = e.obj("live.observer", 1120, 450)
    e.connect(wire, 2, obs_root, 1)
    e.connect(wire, 2, obs_scale, 1)
    prop_root = e.msg("property root_note", 1000, 420)
    prop_scale = e.msg("property scale_intervals", 1120, 420)
    e.connect(wire, 1, prop_scale)
    e.connect(wire, 0, prop_root)
    e.connect(prop_root, 0, obs_root)
    e.connect(prop_scale, 0, obs_scale)
    live_root_gate = e.obj("gate 1 1", 1000, 480)
    live_scale_gate = e.obj("gate 1 1", 1120, 480)
    e.connect(obs_root, 0, live_root_gate, 1)
    e.connect(obs_scale, 0, live_scale_gate, 1)
    e.connect(live_root_gate, 0, root_prep)
    live_scale_prep = e.obj("prepend scale", 1120, 510)
    e.connect(live_scale_gate, 0, live_scale_prep)
    e.connect(live_scale_prep, 0, to_hands)

    follow_store, follow_val = stored_toggle(e, follow, 700, 110)
    resend_to(follow_store)
    follow_split = e.obj("t i i", 700, 170)
    e.connect(follow_val, 0, follow_split)
    # Right outlet first: set gates, gray out the manual menus.
    e.connect(follow_split, 1, live_root_gate, 0)
    e.connect(follow_split, 1, live_scale_gate, 0)
    manual = e.obj("== 0", 760, 200)
    e.connect(follow_split, 1, manual)
    e.connect(manual, 0, root_gate, 0)
    e.connect(manual, 0, scale_gate, 0)
    active = e.obj("prepend active", 760, 230)
    e.connect(manual, 0, active)
    e.connect(active, 0, root)
    e.connect(active, 0, scale)
    # Then pull current values from the chosen source.
    pick = e.obj("sel 1 0", 700, 230)
    e.connect(follow_split, 0, pick)
    e.connect(pick, 0, path_msg)
    refresh_menus = e.obj("t b b", 700, 260)
    e.connect(pick, 1, refresh_menus)
    e.connect(refresh_menus, 1, scale)
    e.connect(refresh_menus, 0, root)

    # --- map slots ----------------------------------------------------------
    mx = 908.0
    e.label("MAP HAND MOVEMENT", [mx, 14.0, 200.0, 18.0], 12.0)
    e.label("Pick a movement, click Map, then click any parameter in Live.", [mx, 34.0, 290.0, 15.0])
    e.label("Movement", [mx, 54.0, 76.0, 15.0])
    e.label("Target", [mx + 80, 54.0, 76.0, 15.0])
    # Columns follow liveui.map's layout: Map 1-75, X 76-91, Min 92-126, Max 125-159.
    e.label("Min", [mx + 82 + 96, 54.0, 30.0, 15.0])
    e.label("Max", [mx + 82 + 129, 54.0, 30.0, 15.0])
    e.label("Curve", [mx + 252, 54.0, 40.0, 15.0])
    expr_in = e.obj("r ---mh_expr", 1300, 20)
    for k in range(MAP_SLOTS):
        y = 72.0 + 30.0 * k
        bx = 1300 + 170 * k
        src = e.menu(f"Map {k + 1} Source", f"Src {k + 1}", EXPRESSIONS, MAP_DEFAULTS[k],
                     [mx, y, 76.0, 18.0], bx, 60)
        resend_to(src)
        index = e.obj("+ 1", bx, 90)
        e.connect(src, 0, index)
        pick_expr = e.obj("zl nth 1", bx, 120)
        e.connect(index, 0, pick_expr, 1)
        e.connect(expr_in, 0, pick_expr)
        curve = e.numbox(f"Map {k + 1} Curve", f"Curve {k + 1}", -100, 100, 0, 5,
                         [mx + 250, y, 40.0, 16.0], bx + 80, 60)
        resend_to(curve)
        shaped = e.obj("expr pow($f1\\, pow(2.\\, $f2 / 50.))", bx, 150)
        e.connect(curve, 0, shaped, 1)
        e.connect(pick_expr, 0, shaped)
        ramp = e.msg("$1 20", bx, 180)
        e.connect(shaped, 0, ramp)
        line = e.obj("line~", bx, 210)
        e.connect(ramp, 0, line)
        mapper = e._add({
            "maxclass": "bpatcher", "name": "liveui.map.maxpat", "embed": 0,
            "numinlets": 1, "numoutlets": 2, "outlettype": ["signal", ""],
            "offset": [0.0, 0.0], "bgmode": 0, "border": 0, "clickthrough": 0,
            "enablehscroll": 0, "enablevscroll": 0, "lockeddragscroll": 0, "viewvisibility": 1,
            "patching_rect": [bx, 240, 160, 16],
            "presentation": 1, "presentation_rect": [mx + 82, y, 160.0, 16.0],
            "varname": f"map{k + 1}",
        })
        e.connect(line, 0, mapper)
        remote = e.obj("live.remote~ @normalized 1", bx, 280)
        e.connect(mapper, 0, remote, 0)
        e.connect(mapper, 1, remote, 1)

    # Meters get the same values, prefixed so the v8ui knows what they are.
    expr_msg = e.obj("prepend expr", 1300, 340)
    e.connect(expr_in, 0, expr_msg)
    e.connect(expr_msg, 0, panel_view)

    _ = inlet
    window = [80.0, 80.0, W, H]  # x, y, width, height
    return e, e.patcher(window, toolbarvisible=0, statusbarvisible=0, enablehscroll=0, enablevscroll=0,
                        title="MidiHands")


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
    picture_out = p.obj("s ---mh_picture", 300, 460)
    p.connect(hands, 3, picture_out)

    # --- strip: hand view + camera ---------------------------------------
    view = p._add({
        "maxclass": "v8ui", "filename": "mh-view.js", "parameter_enable": 0,
        "numinlets": 1, "numoutlets": 1, "outlettype": [""],
        "patching_rect": [400, 460, 220, 156],
        "presentation": 1, "presentation_rect": [4.0, 6.0, 230.0, 156.0],
    })
    info = p.obj("route cameras", 420, 420)
    p.connect(hands, 4, info)
    p.connect(hands, 2, view)
    p.connect(info, 1, view)  # status, stats, error
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
    p.label("Settings, camera picture and\nmapping open in their own window.", [x0, 84.0, 196.0, 30.0])
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

    # The picture is rendered once the editor has been opened and "Show
    # Picture" is on; before that nobody can see it.
    opened = p.obj("i", 300, 150)
    one = p.msg("1", 300, 120)
    p.connect(open_button, 0, one)
    p.connect(one, 0, opened)
    picture_on = p.obj("r ---mh_picture_on", 380, 90)
    picture_split = p.obj("t b i", 380, 120)
    p.connect(picture_on, 0, picture_split)
    both = p.obj("expr $i1 && $i2", 300, 210)
    p.connect(opened, 0, both)
    p.connect(picture_split, 1, both, 1)
    p.connect(picture_split, 0, opened)  # re-evaluate with the new toggle value
    changed = p.obj("change", 300, 240)
    p.connect(both, 0, changed)
    preview = p.obj("prepend preview", 300, 270)
    p.connect(changed, 0, preview)
    p.connect(preview, 0, hands)

    params = {**p.params, **editor.params, "inherited_shortname": 1}
    patcher = p.patcher([40.0, 80.0, 1500.0, 700.0], openrect=[0.0, 0.0, 0.0, 169.0],
                        description="Camera hand tracking to MIDI", title="MidiHands",
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
