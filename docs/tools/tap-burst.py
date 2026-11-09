#!/usr/bin/env python3
"""Grab a burst of framebuffer frames immediately after a SpringBoard tap.

usage: tap-burst.py X Y [N] [TAG]
Captures N frames (about 0.43 s apart) and reports each frame's red-pixel
share, so a transient red launch/splash frame can be caught and inspected.
"""
import os
import subprocess
import sys
import time
from PIL import Image

ROOT = r'C:/Users/Administrator/WorkBuddy/2026-11-06-17-23-24'
FB = ROOT + '/.tools/platform-tools/fastboot.exe'
x = sys.argv[1] if len(sys.argv) > 1 else '0.135'
y = sys.argv[2] if len(sys.argv) > 2 else '0.736'
n = int(sys.argv[3]) if len(sys.argv) > 3 else 14
tag = sys.argv[4] if len(sys.argv) > 4 else 'burst'
OUT = ROOT + '/build-out/tap-' + tag
os.makedirs(OUT, exist_ok=True)


def fb(*args):
    return subprocess.run([FB, '-s', 'xnuloader', *args], capture_output=True)


def shot(i, t0):
    fb('oem', 'screenshot')
    raw = OUT + '/f%02d.raw' % i
    fb('get_staged', raw)
    d = open(raw, 'rb').read()
    need = 1280 * 2772 * 4
    if len(d) < need:
        print('%02d t=%.3fs  NO DATA %d B' % (i, time.time() - t0, len(d)), flush=True)
        return
    im = Image.frombytes('RGBA', (1280, 2772), d[:need], 'raw', 'BGRA', 1280 * 4, 1).convert('RGB')
    im.resize((320, 693)).save(OUT + '/f%02d.png' % i)
    sm = im.resize((160, 346))
    px = list(sm.getdata())
    red = sum(1 for r, g, b in px if r > 110 and r > g * 1.45 and r > b * 1.45)
    avg = tuple(sum(c[j] for c in px) // len(px) for j in range(3))
    print('%02d t=%.3fs  red=%5.1f%%  avg=%s' %
          (i, time.time() - t0, 100.0 * red / len(px), avg), flush=True)


print('baseline before tap:', flush=True)
t0 = time.time()
shot(99, t0)
r = subprocess.run([FB, '-s', 'xnuloader', 'oem', 'sh',
                    '/redwood/bin/rwtouch tap %s %s' % (x, y)], capture_output=True)
print('tap (%s,%s): %s' % (x, y, (r.stdout + r.stderr).decode(errors='replace').strip()[:160]), flush=True)
t0 = time.time()
for i in range(n):
    shot(i, t0)
print('wrote', OUT)
