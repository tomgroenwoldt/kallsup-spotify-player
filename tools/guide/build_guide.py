"""Builds firmware/main/www/anleitung.html, the printable German user guide,
and the English drawings for the README's user guide (docs/img/guide/).

Fills anleitung.tpl.html with the line drawings from sketches.py and QR codes
from qrencode (must be installed). Run after changing either:

    python3 tools/guide/build_guide.py
"""

import re
import subprocess
from pathlib import Path

import sketches

HERE = Path(__file__).parent
ROOT = HERE.parent.parent
OUT = ROOT / "firmware/main/www/anleitung.html"
IMG = ROOT / "docs/img/guide"

# Standalone SVG files for the README, with English labels.
README_IMAGES = {
    "clock": lambda: sketches.hero(),
    "plug-in": lambda: sketches.plug_in(),
    "buttons": lambda: sketches.buttons("Snooze", "Stop"),
    "snooze-1": lambda: sketches.press("play", "1×"),
    "snooze-2": lambda: sketches.press("play", "2×"),
    "snooze-3": lambda: sketches.press("play", "3×"),
    "stop-1": lambda: sketches.press("bt", "1×"),
    "stop-2": lambda: sketches.press("bt", "2×"),
    "stop-hold": lambda: sketches.press("bt", "hold"),
    "stop-1-hold": lambda: sketches.press("bt", "1× + hold"),
}

QR = {
    "QR_WIFI": "WIFI:S:Kallsup-Setup;T:nopass;;",
    "QR_AP": "http://192.168.4.1",
    "QR_HOME": "http://kallsup-alarm.local",
}


def qr(text):
    svg = subprocess.run(
        ["qrencode", "-t", "SVG", "--svg-path", "--inline", "-m", "2", "-l", "M", "-o", "-", text],
        check=True, capture_output=True, text=True,
    ).stdout
    svg = re.sub(r"<!--.*?-->\s*", "", svg)
    svg = re.sub(r'\s+width="[^"]*" height="[^"]*"', "", svg, count=1)  # sized by CSS
    return re.sub(r">\s+<", "><", svg).strip()


def main():
    parts = {key: qr(text) for key, text in QR.items()}
    parts.update(
        HERO=sketches.hero(),
        PLUG=sketches.plug_in(),
        BUTTONS=sketches.buttons(),
        SNOOZE_1=sketches.press("play", "1×"),
        SNOOZE_2=sketches.press("play", "2×"),
        SNOOZE_3=sketches.press("play", "3×"),
        STOP_1=sketches.press("bt", "1×"),
        STOP_2=sketches.press("bt", "2×"),
        STOP_HOLD=sketches.press("bt", "halten"),
        STOP_1_HOLD=sketches.press("bt", "1× + halten"),
    )
    html = (HERE / "anleitung.tpl.html").read_text()
    for key, value in parts.items():
        html = html.replace("{{%s}}" % key, value)
    missing = re.findall(r"\{\{(\w+)\}\}", html)
    if missing:
        raise SystemExit(f"unfilled placeholders: {missing}")
    OUT.write_text(html)
    print(f"wrote {OUT} ({len(html)} bytes)")

    # As <img> on GitHub: white background, or the black lines vanish in dark mode.
    IMG.mkdir(parents=True, exist_ok=True)
    for name, draw in README_IMAGES.items():
        svg = draw().replace("<svg ", '<svg style="background:#fff" ', 1)
        (IMG / f"{name}.svg").write_text(svg + "\n")
    print(f"wrote {len(README_IMAGES)} drawings to {IMG}")


if __name__ == "__main__":
    main()
