"""Render the exact shared 320-pixel Boot chooser drawing on the host."""
from pathlib import Path
import hashlib
import html
import json
import os
import subprocess

from tools.simulator.runner import png
from tools.studio.build import ROOT, toolchain


SCREENS = ("checking", "linnet", "apple", "rockbox", "starting-linnet", "starting-apple")
TARGETS = ("classic6g-n25", "video5g-m25")


def simulate_boot(output, target):
    if target not in TARGETS:
        raise ValueError("Boot chooser simulator supports A1238 and A1136")
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = toolchain()["compiler"]["path"]
    executable = output / ("boot-menu-simulator.exe" if os.name == "nt" else "boot-menu-simulator")
    result = subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT), str(ROOT / "tools/simulator/boot_menu.c"), "-o", str(executable)],
        capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("Boot simulator compilation failed:\n" + result.stdout + result.stderr)
    subprocess.run([str(executable), str(output)], check=True, timeout=30)
    frames = []
    cards = []
    for name in SCREENS:
        raw = (output / (name + ".rgb565")).read_bytes()
        if len(raw) != 320 * 240 * 2:
            raise ValueError("Boot simulator framebuffer size mismatch")
        (output / (name + ".png")).write_bytes(png(raw, 320, 240))
        frames.append(dict(screen=name, sha256=hashlib.sha256(raw).hexdigest()))
        cards.append(f'<figure><img src="{html.escape(name)}.png" width="640" height="480">'
                     f'<figcaption>{html.escape(name)}</figcaption></figure>')
    page = ('<!doctype html><meta charset="utf-8"><title>Linnet Boot Chooser</title>'
            '<style>body{font:15px system-ui;background:#20262b;color:white;margin:32px}'
            'main{display:flex;flex-wrap:wrap;gap:24px}figure{margin:0}img{image-rendering:pixelated}'
            'figcaption{padding:8px 0}</style><h1>Linnet Boot Chooser</h1><main>'
            + ''.join(cards) + '</main>')
    (output / "index.html").write_text(page, encoding="utf-8")
    report = dict(target_id=target, width=320, height=240, frames=frames,
                  renderer="ui/profiles/color320/boot_menu.h", preview=str(output / "index.html"))
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report
