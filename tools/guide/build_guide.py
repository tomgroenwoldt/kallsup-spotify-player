"""Builds firmware/main/www/anleitung.html, the printable German user guide.

Fills anleitung.tpl.html with the line drawings from sketches.py and QR codes
from qrencode (must be installed). Run after changing either:

    python3 tools/guide/build_guide.py
"""

import re
import subprocess
from pathlib import Path

import sketches

HERE = Path(__file__).parent
OUT = HERE.parent.parent / "firmware/main/www/anleitung.html"

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


if __name__ == "__main__":
    main()
