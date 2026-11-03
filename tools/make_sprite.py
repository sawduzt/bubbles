#!/usr/bin/env python3
"""Generate src/sprite.bin for the multi-monitor bubbles screen saver.

Two 200x200 8-bit masks, written back to back:
  [0 : 40000)    rim intensity   (drives hue * val)
  [40000 : 80000) specular mask  (drives pure white)

The rim profile is the angular-median radial profile measured from 154 captured
frames of the Microsoft saver (radius 92 px), then corrected by the residual error
measured on rendered frames so the final composite matches the original within
~4/255 mean absolute error. Regenerate with:  python tools/make_sprite.py
"""
import os
import numpy as np

S = 200
HALF = 100.0
OUT = os.path.join(os.path.dirname(__file__), "..", "src", "sprite.bin")

# radius (px) -> luminance (0..255), measured on the real saver
KNOTS_R = np.array([44, 45, 48, 51, 54, 57, 60, 63, 66, 69, 71, 72, 75, 78,
                    81, 84, 86, 87, 88, 89, 90, 91.0, 92.0], float)
KNOTS_I = np.array([0.0, 1.0, 1.0, 2.0, 2.2, 3.8, 6.0, 8.7, 12.7, 18.3, 21.0,
                    36.5, 50.1, 62.0, 72.8, 104.9, 133.0, 157.9, 175.0, 181.6,
                    176.0, 60.0, 0.0], float)

# radial correction applied after the first render pass (orig/mine, sampled)
CORR_R = np.array([40, 50, 55, 60, 65, 70, 75, 80, 85, 87, 88, 89, 90, 91, 92, 93], float)
CORR_F = np.array([1.0, 1.0, 0.90, 1.00, 0.93, 0.83, 0.78, 0.79, 0.81, 0.78,
                   0.88, 0.88, 0.90, 0.76, 0.45, 1.0])

# specular: ellipse at 10:30, tilted -37 deg
SCX, SCY, SPANG = -43.0, -57.0, np.deg2rad(-37.0)
SIG_MAJ, SIG_MIN, GAIN = 9.0, 4.2, 1.9


def main():
    yy, xx = np.mgrid[0:S, 0:S]
    X = xx + 0.5 - HALF
    Y = yy + 0.5 - HALF
    r = np.hypot(X, Y)

    # supersample the rim profile so the silhouette edge is clean
    SS = 4
    acc = np.zeros((S, S))
    for oy in range(SS):
        for ox in range(SS):
            sx = X + (ox - (SS - 1) / 2) / SS
            sy = Y + (oy - (SS - 1) / 2) / SS
            rr = np.hypot(sx, sy)
            th = np.arctan2(sy, sx)
            prof = np.interp(rr, KNOTS_R, KNOTS_I / KNOTS_I.max(), left=0.0, right=0.0)
            acc += prof * (1.0 + 0.15 * np.sin(th))   # brighter toward the bottom
    I = np.clip(acc / (SS * SS), 0, 1)

    # residual correction measured against rendered frames
    rad = np.arange(0, S, 0.25)
    prof = np.array([I[(r >= a) & (r < a + 0.25)].mean() for a in rad])
    I2 = np.interp(r - 0.45, rad, prof, left=0.0, right=0.0) * \
        np.interp(r, CORR_R, CORR_F, left=1.0, right=1.0) * 1.07
    I = np.clip(I2, 0, 1)
    I[r > 92.0] = 0.0

    # specular blob
    dx, dy = X - SCX, Y - SCY
    u = dx * np.cos(SPANG) + dy * np.sin(SPANG)
    v = -dx * np.sin(SPANG) + dy * np.cos(SPANG)
    W = np.clip(GAIN * np.exp(-0.5 * ((u / SIG_MAJ) ** 2 + (v / SIG_MIN) ** 2)), 0, 1)
    W[W < 0.02] = 0.0

    blob = (I * 255).astype(np.uint8).tobytes() + (W * 255).astype(np.uint8).tobytes()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(blob)
    print("wrote", os.path.abspath(OUT), len(blob), "bytes")


if __name__ == "__main__":
    main()
