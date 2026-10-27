"""Kallsup base: 3D-printable plinth for the ESP32-S3 + MAX98357A electronics.

Proof of concept. The Kallsup (70 x 70 mm) sits on top, flush with the front
and sides; the base is deeper (74.5 mm) to fit a 50 x 70 mm perfboard, so it
sticks out 4.5 mm at the back. A short harness runs
through a grommet in the lid and a matching 8 mm hole drilled into the Kallsup
bottom. Inside, a perfboard carrier holds the devkit in female headers plus the
amp, the bulk cap and a JST-XH connector. See docs/WIRING.md.

A 0.96" OLED (26 x 26 mm module) looks out of a window in the front wall. It
slides into two rails on the wall's inside from below, before the plate goes on;
the rails' lips hold it against the wall, a stop under the lid ends the slide,
and the perfboard below keeps it from dropping out. Its glass sits in a 1 mm
pocket in the wall. A 4-pin JST-XH on the back at the top edge; there's room
above the board for it and room behind it for the plug.

The Kallsup's four feet sit in shallow pockets in the lid. To reach the
devkit's BOOT/RST buttons, lift the Kallsup off and poke through the lid holes.

Measured: Kallsup 70 x 70, feet 8.3 mm, devkit 65 x 28 with buttons and LED
positions, pins 8 mm below the PCB (3 mm spacer), amp 17.5 x 19, cap 10 x 17.5,
OLED PCB 26 x 26, glass 24.7 x 17.6.
Still assumed: Kallsup corner radius, header row spacing (0.9"), female header
height (8.5 mm), USB-C positions on the devkit end, foot spacing measured over
the outer edges, OLED glass thickness, position and lit area (typical 0.96" panel).

Parts (print orientation):
  shell   - upside down, lid on the bed, no supports
  plate   - flat, posts up
  grommet - flange down
  lid_test - just the lid (pockets + holes), quick fit check on the Kallsup
  oled_test - the front wall around the OLED window with its rails, front face
              down; slide the module in to check the fit before printing the shell

Assembly: 4x M3 heat-set inserts (6 mm long, 5 mm OD) into the shell bosses, 4x M3x6 screws
through the plate; the 50 x 70 perfboard on the plate posts through its own
corner holes (3 mm, 65 x 45 mm apart) with 4x M2.5x6 self-tapping screws.

Run:  python kallsup_base.py   (needs build123d)
Writes STEP + STL files to ./out/.
"""

from pathlib import Path

from build123d import (
    Align,
    Axis,
    Box,
    Compound,
    Cylinder,
    Location,
    Pos,
    Rectangle,
    RectangleRounded,
    Rot,
    export_step,
    export_stl,
    extrude,
    fillet,
)

# --- Outer envelope ---------------------------------------------------------
KALLSUP = 70.0  # Kallsup footprint
OUTER_X = KALLSUP  # flush with the Kallsup's sides
CORNER_R = 0.0  # outer corner radius; 0 = sharp corners
WALL = 2.0
LID_T = 2.0
PLATE_T = 2.0
TOTAL_H = 43.0  # plate + shell; tall enough for the OLED standing behind the front wall
TOP_FILLET = 0.0  # 0 = sharp top edge

SHELL_Z0 = PLATE_T  # shell sits on the plate
LID_Z = TOTAL_H - LID_T  # underside of the lid

# --- Perfboard carrier (50 x 70 prototype board, long side front-to-back) -----
PERF_W, PERF_L, PERF_T = 50.0, 70.0, 1.6  # x, y
PERF_HOLE_DX, PERF_HOLE_DY = 45.0, 65.0  # corner mounting holes, centre to centre
PERF_CLEAR = 0.25  # each side, board to wall

INNER_X = OUTER_X - 2 * WALL
INNER_Y = PERF_L + 2 * PERF_CLEAR
OUTER_Y = INNER_Y + 2 * WALL
KALLSUP_CY = -OUTER_Y / 2 + KALLSUP / 2  # Kallsup flush with the front

# --- Kallsup feet (pockets in the lid) --------------------------------------
FOOT_D = 8.3
FOOT_SPAN_X = 56.0  # left-right, over the outer edges of the feet
FOOT_SPAN_Y = 62.0  # front-back, over the outer edges of the feet
FOOT_H = 3.5
FOOT_CLEAR = 0.6  # added to the pocket diameter
FOOT_POCKET_DEPTH = FOOT_H - 0.3  # feet carry the weight, body clears the lid
FOOT_FLOOR = 1.0  # plastic under each pocket
FOOT_BOSS_R = (FOOT_D + FOOT_CLEAR) / 2 + 1.2  # thickened lid around each pocket
FOOT_X = (FOOT_SPAN_X - FOOT_D) / 2
FOOT_Y = (FOOT_SPAN_Y - FOOT_D) / 2  # relative to the Kallsup centre

# --- Screw bosses (corners, beside the perfboard) ----------------------------
BOSS_R = 3.75
BOSS_X = INNER_X / 2 - BOSS_R + 0.25  # reaches 0.25 mm into the side walls
BOSS_Y = INNER_Y / 2 - BOSS_R + 0.25  # and into the front/back walls
INSERT_D = 4.2  # M3 heat-set insert, 6 mm long, 5 mm OD
INSERT_DEPTH = 7.0  # 1 mm deeper than the insert, for displaced plastic
SCREW_CLEAR_D = 3.4
SCREW_HEAD_D = 6.2  # M3 pan or socket head
SCREW_HEAD_DEPTH = 1.4

# --- Perfboard mounting ------------------------------------------------------
PERF_CX, PERF_CY = 0.0, 0.0
POST_H = 7.0  # room for solder joints and wiring under the board
POST_R = 3.0
POST_PILOT_D = 2.2  # M2.5 self-tapping into PLA
POST_PILOT_DEPTH = 5.0  # the M2.5x6 screw reaches 4.4 mm into the post
PERF_SCREW_HEAD_D, PERF_SCREW_HEAD_H = 4.6, 1.8  # M2.5 pan head, on top of the perfboard
PERF_Z = PLATE_T + POST_H  # underside of the perfboard

# --- Devkit (long axis along Y, antenna at the front -Y, USB at the back +Y) --
DEV_W, DEV_L, DEV_T = 28.0, 65.0, 1.6
DEV_CX = PERF_CX - PERF_W / 2 + 1.0 + DEV_W / 2
DEV_BACK_INSET = 1.0  # USB end, in from the perfboard's back edge
DEV_CY = PERF_CY + PERF_L / 2 - DEV_BACK_INSET - DEV_L / 2
HEADER_H = 8.5  # female header socket height
HEADER_SPACER = 3.0  # plastic spacer on the devkit's own pins
HEADER_ROW_SPACING = 22.86  # 0.9", verify
DEV_Z = PERF_Z + PERF_T + HEADER_H + HEADER_SPACER  # underside of the devkit PCB
DEV_TOP_PARTS = 3.5  # tallest part on top (module shield, USB-C, buttons)

# Positions measured on the devkit, looking down with the antenna on the left:
# (mm from the antenna end, mm from the far ("top") header row).
DEV_BUTTONS = [(30.0, 9.0), (35.0, 9.0)]
DEV_LED = (45.0, 19.0)
BUTTON_HOLE_D = 3.0


def dev_xy(from_antenna: float, from_top_row: float) -> tuple[float, float]:
    """Devkit-measured position -> housing XY. The top row is on the -X side."""
    top_row_x = DEV_CX - HEADER_ROW_SPACING / 2
    return top_row_x + from_top_row, DEV_CY - DEV_L / 2 + from_antenna


# Perfboard screw posts, under the board's corner holes
POSTS = [(PERF_CX + sx * PERF_HOLE_DX / 2, PERF_CY + sy * PERF_HOLE_DY / 2)
         for sx in (-1, 1) for sy in (-1, 1)]

# --- Amp, cap, JST (on the perfboard, right of the devkit) -------------------
AMP_W, AMP_L = 17.5, 19.0  # x, y; mounting holes 13 mm apart on the 17.5 side
AMP_STANDOFF = 2.5  # header spacer under the breakout
AMP_CX = PERF_CX + PERF_W / 2 - 0.25 - AMP_W / 2
AMP_CY = -20.4  # speaker terminal at the front, pin row at the back next to the cap
CAP_D, CAP_H = 10.0, 17.5  # laid flat, axis along Y
CAP_CY = -1.0
JST_W, JST_L, JST_H = 17.4, 5.75, 7.0  # 6-pin JST-XH, vertical
JST_CY = 17.5

# --- OLED (behind the front wall, centred, pins at the top) -----------------
OLED_W, OLED_H, OLED_PCB_T = 26.0, 26.0, 1.6  # PCB, x and z
OLED_BACK_PARTS = 1.0  # tallest part on the PCB's back, outside the header
GLASS_W, GLASS_H, GLASS_T = 24.7, 17.6, 1.45  # measured; thickness assumed
GLASS_TOP = 3.5  # glass top edge below the PCB's top edge, assumed
AA_W, AA_H = 21.74, 10.86  # lit area, centred left-right
AA_TOP = 5.0  # lit area top edge below the PCB's top edge, assumed
WINDOW_MARGIN = 1.0  # window beyond the lit area, each side, covers the guesses above
GLASS_POCKET = 1.0  # glass sunk this far into the 2 mm wall
GLASS_CLEAR = 0.3  # pocket beyond the glass, each side
OLED_CLEAR = 0.3  # rails beyond the PCB/glass, each side and behind
OLED_SLIDE = 3.0  # extra room above the PCB, for the JST connector
OLED_SLOT = 3.0  # wall face to the lips; 2.35 was too tight on the test print
OLED_JST_W, OLED_JST_D, OLED_JST_H = 12.5, 7.0, 6.0  # 4-pin JST-XH, vertical, on the back at the top edge
OLED_PLUG_D = 12.0  # mated plug and wire bend behind the connector
OLED_LIP = 1.2  # lips overlap the PCB's side edges from behind
RAIL_W, LIP_T = 1.6, 1.4
OLED_Z = PLATE_T + POST_H + PERF_T + 0.8  # PCB bottom edge, just above the perfboard
OLED_TOP = OLED_Z + OLED_H
FRONT_IN_Y = -OUTER_Y / 2 + WALL  # inside face of the front wall
GLASS_FRONT_Y = FRONT_IN_Y - GLASS_POCKET
OLED_PCB_Y = GLASS_FRONT_Y + GLASS_T  # PCB front face
OLED_BACK_Y = OLED_PCB_Y + OLED_PCB_T  # PCB back face

# --- Openings ---------------------------------------------------------------

CABLE_X, CABLE_Y = 14.0, KALLSUP_CY  # harness through the lid, above the amp side
GROMMET_OD = 7.6  # fits an 8 mm hole in the lid and in the Kallsup bottom
GROMMET_ID = 5.5
GROMMET_FLANGE_D = 11.0
GROMMET_FLANGE_T = 1.2
GROMMET_PROTRUDE = 3.0  # above the lid, into the Kallsup
LID_HOLE_D = 8.0

VENT_W, VENT_L, VENT_PITCH, VENT_N = 2.0, 26.0, 5.0, 5

OUT_DIR = Path(__file__).parent / "out"

CORNERS = [(sx, sy) for sx in (-1, 1) for sy in (-1, 1)]


def rounded_slab(w: float, l: float, r: float, h: float, z0: float):
    profile = RectangleRounded(w, l, r) if r > 0 else Rectangle(w, l)
    return Pos(0, 0, z0) * extrude(profile, h)


def make_shell():
    shell_h = TOTAL_H - SHELL_Z0
    outer = rounded_slab(OUTER_X, OUTER_Y, CORNER_R, shell_h, SHELL_Z0)
    if TOP_FILLET > 0:
        outer = fillet(outer.edges().group_by(Axis.Z)[-1], TOP_FILLET)
    cavity = rounded_slab(INNER_X, INNER_Y, max(CORNER_R - WALL, 0), shell_h - LID_T, SHELL_Z0)
    shell = outer - cavity

    for sx, sy in CORNERS:
        x, y = sx * BOSS_X, sy * BOSS_Y
        shell += Pos(x, y, SHELL_Z0) * Cylinder(
            BOSS_R, LID_Z - SHELL_Z0, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )
        shell -= Pos(x, y, SHELL_Z0) * Cylinder(
            INSERT_D / 2, INSERT_DEPTH, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )

    # Cable hole through the lid
    shell -= Pos(CABLE_X, CABLE_Y, LID_Z - 1) * Cylinder(
        LID_HOLE_D / 2, LID_T + 2, align=(Align.CENTER, Align.CENTER, Align.MIN)
    )

    # Pockets for the Kallsup feet, deeper than the lid, so each gets a boss below it
    boss_h = FOOT_POCKET_DEPTH + FOOT_FLOOR - LID_T
    for sx, sy in CORNERS:
        shell += Pos(sx * FOOT_X, KALLSUP_CY + sy * FOOT_Y, LID_Z - boss_h) * Cylinder(
            FOOT_BOSS_R, boss_h + 0.01, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )
    for sx, sy in CORNERS:
        shell -= Pos(sx * FOOT_X, KALLSUP_CY + sy * FOOT_Y, TOTAL_H - FOOT_POCKET_DEPTH) * Cylinder(
            (FOOT_D + FOOT_CLEAR) / 2, FOOT_POCKET_DEPTH + 1,
            align=(Align.CENTER, Align.CENTER, Align.MIN),
        )

    # Poke holes above BOOT/RST
    for b in DEV_BUTTONS:
        x, y = dev_xy(*b)
        shell -= Pos(x, y, LID_Z - 1) * Cylinder(
            BUTTON_HOLE_D / 2, LID_T + 2, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )

    shell = add_oled_mount(shell)
    return shell


def add_oled_mount(shell):
    # Window and glass pocket
    win_cz = OLED_TOP - AA_TOP - AA_H / 2
    win = Box(AA_W + 2 * WINDOW_MARGIN, WALL + 2, AA_H + 2 * WINDOW_MARGIN)
    shell -= Pos(0, FRONT_IN_Y - WALL / 2, win_cz) * win
    glass_cz = OLED_TOP - GLASS_TOP - GLASS_H / 2
    pocket = Box(GLASS_W + 2 * GLASS_CLEAR, GLASS_POCKET + 0.01, GLASS_H + 2 * GLASS_CLEAR,
                 align=(Align.CENTER, Align.MIN, Align.CENTER))
    shell -= Pos(0, GLASS_FRONT_Y, glass_cz) * pocket

    # Rails with lips behind the PCB's side edges, from the perfboard up to the lid
    slot_half = max(OLED_W, GLASS_W) / 2 + OLED_CLEAR
    lip_y = FRONT_IN_Y + OLED_SLOT
    rail_z0 = PLATE_T + POST_H + PERF_T + 0.3
    for sx in (-1, 1):
        rail_x = sx * (slot_half + RAIL_W / 2)
        shell += box_at(RAIL_W, lip_y + LIP_T - FRONT_IN_Y + 0.01, LID_Z - rail_z0 + 0.01,
                        rail_x, (FRONT_IN_Y + lip_y + LIP_T) / 2 - 0.005, rail_z0)
        lip_w = slot_half - (OLED_W / 2 - OLED_LIP)
        lip_x = sx * (slot_half - lip_w / 2)
        shell += box_at(lip_w + 0.01, LIP_T, LID_Z - rail_z0 + 0.01, lip_x, lip_y + LIP_T / 2, rail_z0)
        # Stop under the lid, in front of the lip
        stop_z = OLED_TOP + OLED_CLEAR + OLED_SLIDE
        shell += box_at(lip_w + 0.01, lip_y - FRONT_IN_Y + 0.01, LID_Z - stop_z + 0.01,
                        lip_x, (FRONT_IN_Y + lip_y) / 2, stop_z)
    # The devkit's antenna end passes just behind the left lip: trim it there
    dev_front = DEV_CY - DEV_L / 2
    dev_top = DEV_Z + DEV_T + 3.2  # module shield
    shell -= box_at(DEV_W + 1.0, 10.0, dev_top - DEV_Z + 1.0, DEV_CX, dev_front - 0.5 + 5.0, DEV_Z - 0.5)
    return shell


def make_plate():
    plate = rounded_slab(OUTER_X, OUTER_Y, CORNER_R, PLATE_T, 0)

    for sx, sy in CORNERS:
        x, y = sx * BOSS_X, sy * BOSS_Y
        plate -= Pos(x, y, -1) * Cylinder(
            SCREW_CLEAR_D / 2, PLATE_T + 2, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )
        plate -= Pos(x, y, -1) * Cylinder(
            SCREW_HEAD_D / 2, SCREW_HEAD_DEPTH + 1, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )

    for x, y in POSTS:
        plate += Pos(x, y, PLATE_T) * Cylinder(
            POST_R, POST_H, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )
        plate -= Pos(x, y, PLATE_T + POST_H - POST_PILOT_DEPTH) * Cylinder(
            POST_PILOT_D / 2, POST_PILOT_DEPTH + 0.1, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )

    # Vent slots, between the posts
    for i in range(VENT_N):
        x = (i - (VENT_N - 1) / 2) * VENT_PITCH
        plate -= Pos(x, 0, -1) * extrude(
            RectangleRounded(VENT_W, VENT_L, VENT_W / 2 - 0.01), PLATE_T + 2
        )

    return plate


def make_grommet():
    flange_z = LID_Z - GROMMET_FLANGE_T
    tube_len = GROMMET_FLANGE_T + LID_T + GROMMET_PROTRUDE
    g = Pos(CABLE_X, CABLE_Y, flange_z) * Cylinder(
        GROMMET_FLANGE_D / 2, GROMMET_FLANGE_T, align=(Align.CENTER, Align.CENTER, Align.MIN)
    )
    g += Pos(CABLE_X, CABLE_Y, flange_z) * Cylinder(
        GROMMET_OD / 2, tube_len, align=(Align.CENTER, Align.CENTER, Align.MIN)
    )
    g -= Pos(CABLE_X, CABLE_Y, flange_z - 1) * Cylinder(
        GROMMET_ID / 2, tube_len + 2, align=(Align.CENTER, Align.CENTER, Align.MIN)
    )
    return g


def box_at(w, l, h, x, y, z0):
    return Pos(x, y, z0) * Box(w, l, h, align=(Align.CENTER, Align.CENTER, Align.MIN))


def make_electronics():
    """Placeholder volumes for clearance checks only; not printed."""
    parts = {}
    parts["perfboard"] = box_at(PERF_W, PERF_L, PERF_T, PERF_CX, PERF_CY, PERF_Z)
    hz = PERF_Z + PERF_T
    for i, sx in enumerate((-1, 1)):
        parts[f"header_{i}"] = box_at(
            2.5, 22 * 2.54, HEADER_H, DEV_CX + sx * HEADER_ROW_SPACING / 2, DEV_CY, hz
        )
    parts["devkit"] = box_at(DEV_W, DEV_L, DEV_T, DEV_CX, DEV_CY, DEV_Z)
    top = DEV_Z + DEV_T
    parts["module"] = box_at(18.0, 25.5, 3.2, DEV_CX, DEV_CY - DEV_L / 2 + 25.5 / 2, top)
    usb_y = DEV_CY + DEV_L / 2 - 7.5 / 2 + 0.5
    for i, dx in enumerate((-6.0, 6.0)):
        parts[f"usb_{i}"] = box_at(9.0, 7.5, 3.2, DEV_CX + dx, usb_y, top)
    for i, b in enumerate(DEV_BUTTONS):
        x, y = dev_xy(*b)
        parts[f"button_{i}"] = box_at(3.5, 3.5, 2.5, x, y, top)
    x, y = dev_xy(*DEV_LED)
    parts["led"] = box_at(2.0, 2.0, 1.0, x, y, top)

    amp_z = hz + AMP_STANDOFF
    parts["amp"] = box_at(AMP_W, AMP_L, 1.6, AMP_CX, AMP_CY, amp_z)
    parts["amp_terminal"] = box_at(8.0, 5.0, 8.5, AMP_CX, AMP_CY - AMP_L / 2 + 2.5, amp_z + 1.6)
    cap = Rot(90, 0, 0) * Cylinder(CAP_D / 2, CAP_H)
    parts["cap"] = Pos(AMP_CX, CAP_CY, hz + CAP_D / 2) * cap
    parts["jst"] = box_at(JST_W, JST_L, JST_H, AMP_CX, JST_CY, hz)
    parts["oled_glass"] = box_at(GLASS_W, GLASS_T, GLASS_H, 0, GLASS_FRONT_Y + GLASS_T / 2,
                                 OLED_TOP - GLASS_TOP - GLASS_H)
    parts["oled_pcb"] = box_at(OLED_W, OLED_PCB_T, OLED_H, 0, OLED_PCB_Y + OLED_PCB_T / 2, OLED_Z)
    parts["oled_back_parts"] = box_at(OLED_W - 2 * OLED_LIP - 1.0, OLED_BACK_PARTS, OLED_H - 6.0, 0,
                                      OLED_BACK_Y + OLED_BACK_PARTS / 2, OLED_Z)
    parts["oled_jst"] = box_at(OLED_JST_W, OLED_JST_D, OLED_JST_H, 0, OLED_BACK_Y + OLED_JST_D / 2,
                               OLED_TOP - OLED_JST_H + 3.0)
    parts["oled_plug"] = box_at(OLED_JST_W, OLED_PLUG_D, OLED_JST_H, 0,
                                OLED_BACK_Y + OLED_JST_D + OLED_PLUG_D / 2, OLED_TOP - OLED_JST_H + 3.0)
    for i, (x, y) in enumerate(POSTS):
        parts[f"screw_{i}"] = Pos(x, y, hz) * Cylinder(
            PERF_SCREW_HEAD_D / 2, PERF_SCREW_HEAD_H, align=(Align.CENTER, Align.CENTER, Align.MIN)
        )
    return parts


def build():
    shell = make_shell()
    shell.label = "shell"
    plate = make_plate()
    plate.label = "plate"
    grommet = make_grommet()
    grommet.label = "grommet"
    printed = [shell, plate, grommet]

    electronics = make_electronics()
    for name, solid in electronics.items():
        solid.label = name

    return printed, electronics


def check_clearance(printed, electronics):
    housing = printed[0] + printed[1]
    ok = True
    for name, part in electronics.items():
        clash = (housing & part).volume
        if clash > 1e-3:
            ok = False
            print(f"CLASH  {name}: {clash:.2f} mm^3 overlaps the housing")
    names = list(electronics)
    for i, a in enumerate(names):
        for b in names[i + 1 :]:
            v = (electronics[a] & electronics[b]).volume
            if v > 1e-3 and not {a, b} <= {"perfboard", "header_0", "header_1"}:
                print(f"note   {a} overlaps {b}: {v:.2f} mm^3")
    tallest = max(p.bounding_box().max.Z for p in electronics.values())
    print(f"tallest electronics top z={tallest:.2f}, lid underside z={LID_Z:.2f}, "
          f"headroom {LID_Z - tallest:.2f} mm")
    return ok


def gen_step():
    printed, electronics = build()
    return Compound(children=printed + list(electronics.values()), label="kallsup_base")


def main():
    printed, electronics = build()
    OUT_DIR.mkdir(exist_ok=True)

    shell, plate, grommet = printed
    export_step(gen_step(), str(OUT_DIR / "kallsup_base_assembly.step"))
    # STLs in print orientation
    export_stl(Rot(180, 0, 0) * shell, str(OUT_DIR / "shell.stl"))
    export_stl(plate, str(OUT_DIR / "plate.stl"))
    export_stl(grommet, str(OUT_DIR / "grommet.stl"))
    lid_t = FOOT_POCKET_DEPTH + FOOT_FLOOR
    lid = shell & box_at(OUTER_X + 2, OUTER_Y + 2, lid_t + 1, 0, 0, TOTAL_H - lid_t)
    export_stl(Rot(180, 0, 0) * lid, str(OUT_DIR / "lid_test.stl"))
    test_x = max(OLED_W, GLASS_W) + 2 * (OLED_CLEAR + RAIL_W + 3.0)
    test_y = WALL + OLED_SLOT + LIP_T + 0.5
    test_z0 = PLATE_T + POST_H + PERF_T - 1.0
    oled_test = shell & box_at(test_x, test_y, TOTAL_H - test_z0, 0, -OUTER_Y / 2 + test_y / 2, test_z0)
    export_stl(Rot(90, 0, 0) * oled_test, str(OUT_DIR / "oled_test.stl"))
    bb = oled_test.bounding_box()
    print(f"oled_test valid={oled_test.is_valid} vol={oled_test.volume:7.1f} mm^3  "
          f"bbox {bb.size.X:.1f} x {bb.size.Z:.1f} x {bb.size.Y:.1f} (as printed)")

    for p in printed:
        bb = p.bounding_box()
        print(f"{p.label:8s} valid={p.is_valid} vol={p.volume:9.1f} mm^3  "
              f"bbox {bb.size.X:.1f} x {bb.size.Y:.1f} x {bb.size.Z:.1f}")
    ok = check_clearance(printed, electronics)
    print("clearance OK" if ok else "clearance FAILED")


if __name__ == "__main__":
    main()
