"""IKEA-style line drawings of the Kallsup on its base, as inline SVG strings.

Model coordinates in mm: u across the grille face (left to right, seen from the
front), d from the grille face to the back, z up. The Kallsup is a 70 mm cube
on the 29 mm base (hardware/enclosure/kallsup_base.py), which is flush at the
front and sticks out 4.5 mm at the back. Faces are drawn back to front with a
white fill (painter's algorithm), so no hidden-line removal is needed.
"""

import math

CUBE = 70.0
BASE_H = 29.0
BASE_D = 74.5
STROKE = "#1d1d1b"

# Screen direction of one mm along each axis (y grows downwards).
U = (0.98, -0.11)
D = (-0.40, -0.21)
Z = (0.0, -1.0)


def proj(u, d, z):
    return (u * U[0] + d * D[0] + z * Z[0], u * U[1] + d * D[1] + z * Z[1])


def face_matrix(axis1, axis2, origin):
    """SVG matrix mapping a face's 2D coordinates onto the screen."""
    (a, b), (c, e) = axis1, axis2
    ox, oy = origin
    return f"matrix({a:.4f},{b:.4f},{c:.4f},{e:.4f},{ox:.3f},{oy:.3f})"


def poly(points, extra=""):
    pts = " ".join(f"{x:.2f},{y:.2f}" for x, y in points)
    return f'<polygon points="{pts}" fill="#fff" {extra}/>'


def box(u0, u1, d0, d1, z0, z1):
    """Top, left and front faces of an axis-aligned box, back to front."""
    top = [proj(u0, d0, z1), proj(u1, d0, z1), proj(u1, d1, z1), proj(u0, d1, z1)]
    left = [proj(u0, d0, z0), proj(u0, d1, z0), proj(u0, d1, z1), proj(u0, d0, z1)]
    front = [proj(u0, d0, z0), proj(u1, d0, z0), proj(u1, d0, z1), proj(u0, d0, z1)]
    return poly(top) + poly(left) + poly(front)


def grille(cx, cz, radius, pitch=4.0, dot=1.3):
    """Staggered holes on the front face (d = 0), as <use> references."""
    uses = []
    row = 0
    z = -radius
    while z <= radius:
        offset = pitch / 2 if row % 2 else 0.0
        u = -radius + offset
        while u <= radius:
            if math.hypot(u, z) <= radius - dot:
                uses.append(f'<use href="#hole" x="{cx + u:.2f}" y="{-(cz + z):.2f}"/>')
            u += pitch
        z += pitch * math.sqrt(3) / 2
        row += 1
    # Face coordinates: x = u, y = -z, so the matrix flips z.
    m = face_matrix(U, (-Z[0], -Z[1]), proj(0, 0, 0))
    return (
        f'<defs><circle id="hole" r="{dot}" fill="none" stroke="{STROKE}" '
        f'vector-effect="non-scaling-stroke"/></defs><g transform="{m}">{"".join(uses)}</g>'
    )


PLAY = "M-1.6,-2.4 L2.6,0 L-1.6,2.4 Z"
BLUETOOTH = "M-1.7,-1.4 L1.7,1.4 L0,2.9 L0,-2.9 L1.7,-1.4 L-1.7,1.4"
BUTTONS = {"play": 22.0, "bt": 48.0}  # u of each button centre on top
BUTTON_D = 33.0


def top_buttons():
    """Buttons and LED on the Kallsup's top, drawn in the top face's plane."""
    m = face_matrix(U, D, proj(0, 0, CUBE))
    s = f'vector-effect="non-scaling-stroke" stroke="{STROKE}"'
    parts = []
    for name, u in BUTTONS.items():
        parts.append(f'<circle cx="{u}" cy="{BUTTON_D}" r="6.5" fill="#fff" {s}/>')
        parts.append(f'<circle cx="{u}" cy="{BUTTON_D}" r="5" fill="none" {s}/>')
        symbol = PLAY if name == "play" else BLUETOOTH
        fill = 'fill="none"'
        parts.append(f'<path transform="translate({u},{BUTTON_D}) scale(0.9)" d="{symbol}" {fill} {s}/>')
    parts.append(f'<circle cx="35" cy="{BUTTON_D}" r="0.9" fill="{STROKE}"/>')
    return f'<g transform="{m}">{"".join(parts)}</g>'


def assembly():
    """Base, then the Kallsup on top, with grille and top buttons."""
    return (
        box(0, CUBE, 0, BASE_D, -BASE_H, 0)
        + box(0, CUBE, 0, CUBE, 0, CUBE)
        + grille(35, 35, 29)
        + top_buttons()
    )


def svg(content, view, width, label):
    x0, y0, w, h = view
    return (
        f'<svg viewBox="{x0:.1f} {y0:.1f} {w:.1f} {h:.1f}" width="{width}" role="img" aria-label="{label}" '
        f'class="sk" xmlns="http://www.w3.org/2000/svg" fill="none" stroke="{STROKE}" stroke-width="1.3" '
        f'stroke-linejoin="round" stroke-linecap="round">'
        # Inline <style> applies to the whole page; scope it to these drawings.
        f"<style>.sk :is(polygon,rect,circle,path,line){{vector-effect:non-scaling-stroke}}</style>{content}</svg>"
    )


def bounds(points, pad):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return (min(xs) - pad, min(ys) - pad, max(xs) - min(xs) + 2 * pad, max(ys) - min(ys) + 2 * pad)


def corners():
    return [proj(u, d, z) for u in (0, CUBE) for d in (0, BASE_D) for z in (-BASE_H, CUBE)]


def hero():
    """The clock as a whole, for the title."""
    return svg(assembly(), bounds(corners(), 4), 220, "Kallsup Wecker auf seinem Sockel")


def plug_in():
    """Seen from the back: USB cable into the Kallsup's own socket, adapter dashed."""
    # Mirror u and d so the back faces the viewer; the base now sticks out at the front.
    off = BASE_D - CUBE
    body = (
        box(0, CUBE, 0, BASE_D, -BASE_H, 0)
        + box(0, CUBE, off, off + CUBE, 0, CUBE)
    )
    # Recessed panel with the USB-C socket on the Kallsup's back, as in IKEA's manual.
    # The base's own opening stays unused, so it isn't drawn.
    port_u, port_z = 35.0, 42.0
    m_back = face_matrix(U, (-Z[0], -Z[1]), proj(0, off, 0))
    panel = (
        f'<g transform="{m_back}"><rect x="5" y="-66" width="60" height="62" rx="1.5"/>'
        f'<rect x="{port_u - 5.5}" y="{-port_z - 2.4}" width="11" height="4.8" rx="2.4" fill="#fff"/>'
        f'<rect x="{port_u - 3.5}" y="{-port_z - 0.9}" width="7" height="1.8" rx="0.9"/></g>'
    )
    # Plug and cable come in from the right, IKEA-style dashed for "your" parts.
    x, y = proj(port_u + 7, off, port_z)
    plug_l = 16
    plug = (
        f'<g stroke-dasharray="2.2 1.6">'
        f'<rect x="{x}" y="{y - 3.2}" width="{plug_l}" height="6.4" rx="2" fill="#fff"/>'
        f'<path d="M{x + plug_l},{y} C{x + 40},{y + 2} {x + 30},{y - 40} {x + 62},{y - 50}"/>'
        f'<rect x="{x + 58}" y="{y - 70}" width="22" height="16" rx="3" fill="#fff"/>'
        f'<rect x="{x + 80}" y="{y - 67}" width="5" height="10" rx="1" fill="#fff"/></g>'
    )
    arrow = (
        f'<path d="M{x + 30},{y + 12} l-10,0 l0,-4 l-8,7 l8,7 l0,-4 l10,0 z" '
        f'fill="{STROKE}" stroke="none"/>'
    )
    content = body + panel + plug + arrow
    pts = corners() + [(x + 90, y - 74), (x + 90, y + 24)]
    return svg(content, bounds(pts, 4), 260, "USB-Kabel hinten in den Lautsprecher stecken")


BOX_W = 124


def buttons():
    """Top of the Kallsup with callouts naming the two buttons."""
    clock = assembly()
    # Callout boxes above the clock, each over its own button, so lines don't cross.
    tops = {name: proj(u, BUTTON_D, CUBE) for name, u in BUTTONS.items()}
    xs = [p[0] for p in corners()]
    ys = [p[1] for p in corners()]
    by = min(ys) - 56
    gap = 10
    mid = (min(xs) + max(xs)) / 2
    boxes = {"play": (mid - gap / 2 - BOX_W, by), "bt": (mid + gap / 2, by)}
    labels = {"play": "Schlummern", "bt": "Aus"}
    parts = [clock]
    for name, (bx, by) in boxes.items():
        tx, ty = tops[name]
        parts.append(f'<line x1="{tx:.1f}" y1="{ty:.1f}" x2="{bx + BOX_W / 2:.1f}" y2="{by + 36:.1f}" stroke-width="0.9"/>')
        parts.append(f'<circle cx="{tx:.1f}" cy="{ty:.1f}" r="1.6" fill="{STROKE}" stroke="none"/>')
        parts.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{BOX_W}" height="36" rx="5" fill="#fff"/>')
        cx, cy = bx + 22, by + 18
        parts.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="12" fill="#fff"/>')
        parts.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="9.5"/>')
        symbol = PLAY if name == "play" else BLUETOOTH
        parts.append(f'<path transform="translate({cx:.1f},{cy:.1f}) scale(1.5)" d="{symbol}"/>')
        parts.append(
            f'<text x="{bx + 42:.1f}" y="{cy + 4.5:.1f}" fill="{STROKE}" stroke="none" '
            f'font-family="system-ui,sans-serif" font-size="12" font-weight="700">{labels[name]}</text>'
        )
    pts = corners() + [(boxes["play"][0], by), (boxes["bt"][0] + BOX_W, by)]
    return svg("".join(parts), bounds(pts, 4), 300, "Links der Schlummern-Knopf, rechts der Aus-Knopf")


def press(symbol, label):
    """A button seen from above with a press arrow, like IKEA's close-ups."""
    sym = PLAY if symbol == "play" else BLUETOOTH
    width = 30 + 7 * len(label)
    parts = [
        '<circle cx="0" cy="11.5" r="10.5" fill="#fff"/>',
        '<circle cx="0" cy="11.5" r="8.2"/>',
        f'<path transform="translate(0,11.5) scale(1.4)" d="{sym}"/>',
        f'<path d="M-3,-16 h6 v9 h4 l-7,8 l-7,-8 h4 z" fill="{STROKE}" stroke="none"/>',
        f'<text x="9" y="-7" fill="{STROKE}" stroke="none" font-family="system-ui,sans-serif" '
        f'font-size="11" font-weight="700">{label}</text>',
    ]
    return svg("".join(parts), (-16, -19, width, 43), width, f"{label} drücken")
