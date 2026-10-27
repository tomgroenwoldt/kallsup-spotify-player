"""Render the perfboard layout: 3D view + annotated top view (perfboard mm).

Usage: python render_perfboard.py <dir with kallsup_base.py> <out.png>  (needs build123d, pyvista, matplotlib)
"""
import sys

sys.path.insert(0, sys.argv[1])
out_path = sys.argv[2]

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, FancyBboxPatch, Rectangle
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

import kallsup_base as k

# perfboard frame: origin at front-left corner, component side up
OX, OY = -(k.PERF_CX - k.PERF_W / 2), -(k.PERF_CY - k.PERF_L / 2)


def P(x, y):
    return x + OX, y + OY


printed, elec = k.build()
# 3D only: the cap as a 12-sided prism (matplotlib sorts its few big facets cleanly)
from build123d import Pos as _Pos, RegularPolygon, Rot as _Rot, extrude as _extrude

_cap = _Rot(-90, 0, 0) * _extrude(RegularPolygon(k.CAP_D / 2, 12), k.CAP_H)
elec["cap"] = _Pos(k.AMP_CX, k.CAP_CY - k.CAP_H / 2, k.PERF_Z + k.PERF_T + k.CAP_D / 2) * _cap

fig = plt.figure(figsize=(17, 10.5))

# ---------------------------------------------------------------- 3D view
import numpy as np
import pyvista as pv

_, elec_real = k.build()  # true cylinder for the cap


def to_mesh(solid, dz=0.0):
    verts, tris = solid.tessellate(0.05, 0.1)
    pts = np.array([(v.X + OX, v.Y + OY, v.Z + dz) for v in verts])
    faces = np.hstack([[3, a, b, c] for a, b, c in tris])
    return pv.PolyData(pts, faces)


LIFT = 18.0
colors = {
    "perfboard": "#c9a86a", "devkit": "#1f3d7a", "module": "#b8bcc2",
    "amp": "#6a3d9a", "amp_terminal": "#2e8b57", "cap": "#22325c",
    "jst": "#f2f2f2", "led": "#3cff6e",
}
pl = pv.Plotter(off_screen=True, window_size=(1400, 1300))
pl.set_background("white")
for name, solid in elec_real.items():
    if name.startswith("oled"):
        continue  # stands in the front wall, not on the perfboard
    lifted = name in ("devkit", "module", "led") or name.startswith(("usb", "button"))
    c = colors.get(name)
    if c is None:
        c = ("#222222" if name.startswith("header") else
             "#9aa0a6" if name.startswith(("usb", "screw")) else
             "#333333" if name.startswith("button") else "#888888")
    pl.add_mesh(to_mesh(solid, LIFT if lifted else 0.0), color=c,
                opacity=0.35 if name in ("devkit", "module") else 1.0,
                smooth_shading=name == "cap", specular=0.2)

hz = k.PERF_Z + k.PERF_T
labels = {
    "devkit (lifted 18 mm)": (*P(k.DEV_CX, k.DEV_CY + 20), k.DEV_Z + LIFT + 6),
    "female headers": (*P(k.DEV_CX - k.HEADER_ROW_SPACING / 2, k.DEV_CY - 22), hz + k.HEADER_H + 1),
    "MAX98357A": (*P(k.AMP_CX + 4, k.AMP_CY + 4), hz + 5),
    "cap (flat)": (*P(k.AMP_CX, k.CAP_CY), hz + k.CAP_D + 1),
    "JST-XH 6": (*P(k.AMP_CX, k.JST_CY), hz + k.JST_H + 1),
    "M2.5 screw": (*P(*k.POSTS[2]), hz + 1.5),
}
pl.add_point_labels(np.array(list(labels.values())), list(labels.keys()), font_size=22,
                    point_size=1, shape_opacity=0.75, always_visible=True, text_color="black",
                    shape_color="white")
pl.camera_position = [(115, -75, 130), (25, 35, 16), (0, 0, 1)]
pl.camera.zoom(1.0)
img = pl.screenshot(return_img=True)
pl.close()

ax = fig.add_subplot(1, 2, 1)
ax.imshow(img)
ax.set_axis_off()
ax.set_title("Perfboard, devkit lifted 18 mm (translucent); front = antenna end", fontsize=12)

# ---------------------------------------------------------------- top view
ax2 = fig.add_subplot(1, 2, 2)
ax2.set_aspect("equal")
ax2.set_xlim(-4, k.PERF_W + 18); ax2.set_ylim(-7, k.PERF_L + 8)
ax2.set_title("Top view, component side (mm from front-left corner)\n"
              "dashed lines = wires on the underside", fontsize=12)

ax2.add_patch(Rectangle((0, 0), k.PERF_W, k.PERF_L, fc="#e8d3a3", ec="#8a6d3b", lw=1.5))
row_l = P(k.DEV_CX - k.HEADER_ROW_SPACING / 2, 0)[0]
row_r = P(k.DEV_CX + k.HEADER_ROW_SPACING / 2, 0)[0]
x0 = row_l % 2.54
for i in range(int(k.PERF_W / 2.54) + 1):
    for j in range(int(k.PERF_L / 2.54)):
        x, y = x0 + i * 2.54, 1.27 + j * 2.54
        if x < k.PERF_W - 0.8:
            ax2.add_patch(Circle((x, y), 0.35, fc="#a8875a", ec="none"))

# headers
n_pins = 22
hdr_y0 = P(0, k.DEV_CY)[1] - n_pins * 2.54 / 2


def pin_y(n):
    return hdr_y0 + 1.27 + (n - 1) * 2.54


for x in (row_l, row_r):
    ax2.add_patch(Rectangle((x - 1.25, hdr_y0), 2.5, n_pins * 2.54, fc="#222", ec="none"))
    for n in range(1, n_pins + 1):
        ax2.add_patch(Circle((x, pin_y(n)), 0.45, fc="#d4af37", ec="none"))

# devkit outline (above the headers)
dx0, dy0 = P(k.DEV_CX - k.DEV_W / 2, k.DEV_CY - k.DEV_L / 2)
ax2.add_patch(Rectangle((dx0, dy0), k.DEV_W, k.DEV_L, fc="#1f3d7a", alpha=0.10,
                        ec="#1f3d7a", lw=1.5, ls="--"))
ax2.text(dx0 + k.DEV_W / 2 - 1, dy0 + 2.5, "antenna end", ha="center", fontsize=8, color="#1f3d7a")
ax2.text(dx0 + k.DEV_W / 2 - 1, dy0 + k.DEV_L - 3.5, "USB-C end", ha="center", fontsize=8, color="#1f3d7a")
ax2.text(dx0 + k.DEV_W / 2 - 1, dy0 + 14,
         "devkit plugs in on top\n(11.5 mm above the board)", ha="center", fontsize=8,
         color="#1f3d7a", style="italic")
for b in k.DEV_BUTTONS:
    bx, by = P(*k.dev_xy(*b))
    ax2.add_patch(Rectangle((bx - 1.6, by - 1.6), 3.2, 3.2, fc="none", ec="#1f3d7a", lw=1))
bx, by = P(*k.dev_xy(*k.DEV_BUTTONS[1]))
ax2.text(bx + 2.3, by - 0.8, "BOOT/RST", fontsize=7, color="#1f3d7a")
lx, ly = P(*k.dev_xy(*k.DEV_LED))
ax2.add_patch(Circle((lx, ly), 1.0, fc="#3cff6e", ec="#1f3d7a", lw=0.8))
ax2.annotate("LED", (lx, ly), xytext=(12, ly + 6), fontsize=7, color="#1f3d7a",
             arrowprops=dict(arrowstyle="-", color="#1f3d7a", lw=0.8))

# J1 labels (standard DevKitC-1; verify on your board)
j1 = {1: "3V3", 5: "GPIO5", 6: "GPIO6", 7: "GPIO7", 12: "GPIO8", 15: "GPIO9", 16: "GPIO10",
      17: "GPIO11", 18: "GPIO12", 21: "5V", 22: "GND"}
for n, lab in j1.items():
    ax2.text(row_r - 1.8, pin_y(n), lab, ha="right", va="center", fontsize=7.5,
             fontweight="bold", color="#b00020" if lab in ("5V",) else "#111")
ax2.text(row_l + 1.8, pin_y(1), "GND", ha="left", va="center", fontsize=7.5, fontweight="bold", color="#111")
ax2.text(row_r, hdr_y0 - 1.6, "J1", ha="center", va="top", fontsize=9, fontweight="bold")
ax2.text(row_l, hdr_y0 - 1.6, "J3", ha="center", va="top", fontsize=9, fontweight="bold")

# screw posts
for x, y in k.POSTS:
    px, py = P(x, y)
    ax2.add_patch(Circle((px, py), k.PERF_SCREW_HEAD_D / 2, fc="#cfd3d6", ec="#555", lw=1))
    ax2.add_patch(Circle((px, py), 0.6, fc="#555"))
    ax2.text(px + 2.7, py, "M2.5", fontsize=7, va="center")

# amp
ax0, ay0 = P(k.AMP_CX - k.AMP_W / 2, k.AMP_CY - k.AMP_L / 2)
ax2.add_patch(Rectangle((ax0, ay0), k.AMP_W, k.AMP_L, fc="#6a3d9a", alpha=0.85, ec="#3f1f66"))
ax2.add_patch(Rectangle((ax0 + k.AMP_W / 2 - 4, ay0 + 0.3), 8, 5, fc="#2e8b57", ec="#1b5e3a"))
ax2.text(ax0 + k.AMP_W / 2, ay0 + 2.8, "OUT+ OUT−", ha="center", va="center",
         fontsize=6.5, color="white", fontweight="bold")
ax2.text(ax0 + k.AMP_W / 2, ay0 + 10, "MAX98357A", ha="center", fontsize=9,
         color="white", fontweight="bold")
amp_pin_y = ay0 + k.AMP_L - 1.3
# left to right as seen from above, terminal at the front (as on the user's breakout)
amp_pins = ["VIN", "GND", "SD", "GAIN", "DIN", "BCLK", "LRC"]
amp_x = {}
for i, lab in enumerate(amp_pins):
    x = ax0 + k.AMP_W / 2 + (i - 3) * 2.54
    amp_x[lab] = x
    ax2.add_patch(Circle((x, amp_pin_y), 0.5, fc="#d4af37", ec="none"))
    ax2.text(x, amp_pin_y - 1.3, lab, rotation=90, ha="center", va="top", fontsize=6,
             color="white")

# cap, lying flat, legs at the front end next to the amp pin row
cx, cy0 = P(k.AMP_CX, k.CAP_CY - k.CAP_H / 2)
ax2.add_patch(FancyBboxPatch((cx - k.CAP_D / 2, cy0), k.CAP_D, k.CAP_H,
                             boxstyle="round,pad=0,rounding_size=1.5", fc="#22325c", ec="#111"))
plus_left = amp_x["VIN"] < amp_x["GND"]  # + leg on the VIN side, stripe (−) on the other
stripe_x = cx + k.CAP_D / 2 - 2.2 if plus_left else cx - k.CAP_D / 2
ax2.add_patch(Rectangle((stripe_x, cy0), 2.2, k.CAP_H, fc="#9fb3d9", ec="none"))
ax2.text(cx - 0.8, cy0 + k.CAP_H / 2, "1000 µF\n(flat)", ha="center", va="center",
         fontsize=7.5, color="white")
cap_plus = (cx - 1.3 if plus_left else cx + 1.3, cy0 - 1.2)
cap_minus = (cx + 1.3 if plus_left else cx - 1.3, cy0 - 1.2)
for (x, y), s in ((cap_plus, "+"), (cap_minus, "−")):
    ax2.plot([x, x], [y, cy0], color="#777", lw=1.2)
    ax2.add_patch(Circle((x, y), 0.5, fc="#d4af37", ec="none"))
ax2.text(cap_plus[0], cy0 + 1.6, "+", fontsize=9, fontweight="bold", color="white", ha="center")
ax2.text(cap_minus[0], cy0 + 1.6, "−", fontsize=9, fontweight="bold", color="white", ha="center")

# JST
jx0, jy0 = P(k.AMP_CX - k.JST_W / 2, k.JST_CY - k.JST_L / 2)
ax2.add_patch(Rectangle((jx0, jy0), k.JST_W, k.JST_L, fc="#f7f7f7", ec="#444", lw=1.2))
jst_lab = ["5V", "GND", "SPK+", "SPK−", "S1", "S2"]
jst_x = {}
for i, lab in enumerate(jst_lab):
    x = jx0 + k.JST_W / 2 + (i - 2.5) * 2.5
    jst_x[lab] = x
    ax2.add_patch(Circle((x, jy0 + k.JST_L / 2), 0.5, fc="#d4af37", ec="none"))
    ax2.text(x, jy0 + k.JST_L + 0.6, lab, rotation=90, ha="center", va="bottom", fontsize=6.5)
ax2.text(jx0 - 0.4, jy0 + k.JST_L / 2, "1", ha="right", va="center", fontsize=8, fontweight="bold")
ax2.text(jx0 + k.JST_W / 2, jy0 + k.JST_L + 6.8, "JST-XH 6 (to Kallsup)", ha="center", fontsize=7.5)
jy = jy0 + k.JST_L / 2

# grommet position (above, in the lid)
gx, gy = P(k.CABLE_X, k.CABLE_Y)
ax2.add_patch(Circle((gx, gy), k.LID_HOLE_D / 2, fc="none", ec="#e07a5f", lw=1.3, ls=":"))
ax2.annotate("cable hole\n(in the lid above)", (gx + k.LID_HOLE_D / 2, gy), xytext=(k.PERF_W + 3, gy),
             fontsize=7, color="#e07a5f", va="center",
             arrowprops=dict(arrowstyle="-", color="#e07a5f", lw=0.8))

# wires (underside), schematic routing
def wire(pts, color, lw=1.8):
    xs, ys = zip(*pts)
    ax2.plot(xs, ys, ls="--", color=color, lw=lw, alpha=0.9, zorder=5)


RED, BLK = "#d62728", "#111111"
wire([(jst_x["5V"], jy), (jst_x["5V"], jy0 - 4.6), (cap_plus[0] + 0.2, jy0 - 4.6),
      (cap_plus[0] + 0.2, cap_plus[1])], RED, 2.6)
wire([(cap_plus[0], cap_plus[1]), (amp_x["VIN"], cap_plus[1]), (amp_x["VIN"], amp_pin_y)], RED, 2.6)
wire([(jst_x["5V"], jy), (jst_x["5V"], pin_y(21)), (row_r, pin_y(21))], RED, 2.6)
wire([(jst_x["GND"], jy), (jst_x["GND"], jy0 - 3.0), (cap_minus[0] - 0.3, jy0 - 3.0),
      (cap_minus[0] - 0.3, cap_minus[1])], BLK, 2.6)
wire([(cap_minus[0], cap_minus[1]), (cap_minus[0], cap_minus[1] - 1.0),
      (amp_x["GND"], cap_minus[1] - 1.0), (amp_x["GND"], amp_pin_y)], BLK, 2.6)
wire([(jst_x["GND"], jy), (jst_x["GND"], pin_y(22)), (row_r, pin_y(22))], BLK, 2.6)

i2s = [(5, "BCLK", "#1f77b4"), (6, "LRC", "#ff7f0e"), (7, "DIN", "#2ca02c"), (12, "SD", "#9467bd")]
for n, lab, c in i2s:
    y = pin_y(n)
    wire([(row_r, y), (amp_x[lab], y), (amp_x[lab], amp_pin_y)], c)
wire([(jst_x["S1"], jy), (jst_x["S1"], jy0 + k.JST_L + 1.4), (28.5, jy0 + k.JST_L + 1.4), (28.5, pin_y(15)), (row_r, pin_y(15))], "#8c564b")
wire([(jst_x["S2"], jy), (jst_x["S2"], jy0 + k.JST_L + 2.2), (29.2, jy0 + k.JST_L + 2.2), (29.2, pin_y(16)), (row_r, pin_y(16))], "#e377c2")
# OLED cable: comes down beside the devkit's right edge, through four holes, then
# on the underside to the header pins
oled_x = x0 + 11 * 2.54  # first hole column right of J1
oled_holes = {lab: 1.27 + (3 + i) * 2.54 for i, lab in enumerate(["GND", "VCC", "SCL", "SDA"])}
for lab, y in oled_holes.items():
    ax2.add_patch(Circle((oled_x, y), 0.6, fc="#d4af37", ec="#444", lw=0.8, zorder=7))
OLED_C = {"GND": BLK, "VCC": "#e6550d", "SCL": "#bcbd22", "SDA": "#393b79"}
wire([(oled_x, oled_holes["GND"]), (oled_x - 1.3, oled_holes["GND"]), (oled_x - 1.3, pin_y(1) - 1.0),
      (row_l, pin_y(1) - 1.0), (row_l, pin_y(1))], OLED_C["GND"], 1.4)
wire([(oled_x, oled_holes["VCC"]), (oled_x - 0.6, oled_holes["VCC"]), (oled_x - 0.6, pin_y(1)),
      (row_r, pin_y(1))], OLED_C["VCC"], 1.4)
for lab, n, wx in (("SCL", 18, row_r + 1.3), ("SDA", 17, row_r + 0.8)):
    wire([(oled_x, oled_holes[lab]), (wx, oled_holes[lab]), (wx, pin_y(n)), (row_r, pin_y(n))],
         OLED_C[lab], 1.4)
ax2.annotate("OLED cable\n(from above, beside\nthe devkit)", (oled_x + 0.6, oled_holes["SCL"]),
             xytext=(k.PERF_W + 3, 13), fontsize=7, color="#393b79", va="center",
             arrowprops=dict(arrowstyle="-", color="#393b79", lw=0.8))


def top_wire(pts, color):
    xs, ys = zip(*pts)
    ax2.plot(xs, ys, ls="-", color=color, lw=1.6, zorder=6)


tx = ax0 + k.AMP_W / 2
top_wire([(jst_x["SPK+"], jy), (jst_x["SPK+"], jy0 - 1.0), (30.9, jy0 - 1.0), (30.9, 3.6),
          (tx - 1.8, 3.6), (tx - 1.8, ay0 + 2.5)], "#17becf")
top_wire([(jst_x["SPK−"], jy), (jst_x["SPK−"], jy0 - 1.8), (31.7, jy0 - 1.8), (31.7, 2.8),
          (tx + 1.8, 2.8), (tx + 1.8, ay0 + 2.5)], "#0b7285")

# legend + edge labels
from matplotlib.lines import Line2D

legend = [Line2D([], [], ls="--", color=c, lw=lw, label=l) for c, lw, l in [
    (RED, 2.6, "5V (22–24 AWG)"), (BLK, 2.6, "GND (22–24 AWG)"),
    ("#1f77b4", 1.8, "GPIO5 → BCLK"), ("#ff7f0e", 1.8, "GPIO6 → LRC"),
    ("#2ca02c", 1.8, "GPIO7 → DIN"), ("#9467bd", 1.8, "GPIO8 → SD"),
    ("#8c564b", 1.8, "S1 → GPIO9 (Snooze)"), ("#e377c2", 1.8, "S2 → GPIO10 (Stop)"),
    ("#e6550d", 1.4, "OLED VCC → 3V3"), (BLK, 1.4, "OLED GND → GND (J3)"),
    ("#bcbd22", 1.4, "OLED SCL → GPIO12"), ("#393b79", 1.4, "OLED SDA → GPIO11"),
    ]]
legend += [Line2D([], [], ls="-", color="#17becf", lw=1.6, label="SPK+ → OUT+ (top side)"),
           Line2D([], [], ls="-", color="#0b7285", lw=1.6, label="SPK− → OUT− (top side)")]
ax2.legend(handles=legend, loc="upper left", bbox_to_anchor=(1.0, 1.0), fontsize=8, frameon=False)
ax2.text(k.PERF_W / 2, -4.5, "FRONT (Kallsup grille)", ha="center", fontsize=9, fontweight="bold")
ax2.text(k.PERF_W / 2, k.PERF_L + 5.5, "BACK", ha="center", fontsize=9, fontweight="bold")
ax2.text(k.PERF_W + 3, 4, "J1 pinout = standard\nESP32-S3-DevKitC-1.\nAmp pin order varies\nbetween breakouts.\nCheck your labels.",
         fontsize=7.5, color="#555", va="bottom")
ax2.set_xticks(range(0, int(k.PERF_W) + 1, 10)); ax2.set_yticks(range(0, int(k.PERF_L) + 1, 10))
ax2.tick_params(labelsize=7)
for sp in ax2.spines.values():
    sp.set_visible(False)

plt.tight_layout()
plt.savefig(out_path, dpi=110)
print("wrote", out_path)
