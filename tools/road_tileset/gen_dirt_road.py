#!/usr/bin/env python3
#
#  This file is part of Permafrost Engine.
#  Copyright (C) 2026 Eduard Permyakov
#
#  Permafrost Engine is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  Permafrost Engine is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

"""
Generates a dirt road tileset: one RGBA decal per road link mask (0-15) and
variant, named road-<mask>-<variant>.png. Image right is the ROAD_LINK_RIGHT
direction and image down is ROAD_LINK_DOWN.

Every pair of tiles stitches seamlessly: all noise and scatter is periodic
over one tile, every arm has the same cross-section where it crosses a
border, and variants differ only away from the borders.

The cross-section, from the centreline out: a greyer crown with loose
gravel, two pale compacted wheel tracks, windrows of heaped gravel, a soft
soil shoulder and a ragged fringe where the terrain's grass shows through.
"""

import argparse
import os

import numpy as np
from PIL import Image

UP, RIGHT, DOWN, LEFT = 1, 2, 4, 8

RES = 512
PAD = 24            # pixels evaluated past each border, so shading sees the neighbour
MARGIN = 0.08       # variants agree with each other this close to a border
WIDE_MARGIN = 0.3   # the same for broad features, which need a longer fade to hide it

# Distances across the road are fractions of HALF_WIDTH
HALF_WIDTH = 0.43   # from the centreline to the middle of the alpha falloff, in tiles
FEATHER = 0.05
CROWN = 0.15
TRACK_OFFSET = 0.36
TRACK_HALF = 0.14
WINDROW = 0.6
WINDROW_SIGMA = 0.1
SHOULDER = 0.71
STRAY_LIMIT = 1.04

# Albedo; the engine's lighting brightens these by about 12%
WAY_LIGHT     = np.array([153, 139, 130], np.float32)
WAY_MID       = np.array([137, 124, 116], np.float32)
CROWN_GREY    = np.array([134, 127, 121], np.float32)
GRAVEL_BED    = np.array([144, 139, 134], np.float32)
SOIL_LIGHT    = np.array([133, 117, 101], np.float32)
SOIL_DARK     = np.array([114,  99,  85], np.float32)
GRASS_GREEN   = np.array([ 88, 110,  50], np.float32)
GRASS_DRY     = np.array([116, 110,  70], np.float32)

GRIT_COLOURS   = np.array([[178, 169, 160], [156, 148, 140], [132, 124, 118], [118, 112, 108],
                           [166, 155, 145]], np.float32)
PEBBLE_COLOURS = np.array([[178, 175, 170], [168, 164, 158], [160, 156, 151], [142, 138, 134],
                           [122, 119, 116], [100,  98,  96], [156, 145, 134]], np.float32)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def periodic_noise(rng, beta, fmin=1.0, stretch=(1.0, 1.0)):
    """Unit-variance noise with a 1/f^beta spectrum, periodic over the tile.
    A 'stretch' above 1 along an axis draws the features out along it."""
    f = np.fft.fftfreq(RES) * RES
    fx, fy = np.meshgrid(f, f)
    r = np.hypot(fx * stretch[0], fy * stretch[1])
    amp = np.where(r >= fmin, 1.0 / np.maximum(r, 1.0) ** beta, 0.0)
    spec = np.fft.fft2(rng.standard_normal((RES, RES))) * amp
    out = np.real(np.fft.ifft2(spec))
    return (out - out.mean()) / (out.std() + 1e-9)


def box_blur(a, r):
    """Three box passes along each axis, close to a gaussian of sigma ~r."""
    def axis_pass(a, axis):
        n = a.shape[axis]
        c = np.cumsum(np.pad(a, [(r + 1, r) if i == axis else (0, 0) for i in range(a.ndim)], mode="edge"), axis=axis)
        hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
        lo = np.take(c, np.arange(0, n), axis=axis)
        return (hi - lo) / (2 * r + 1)
    for _ in range(3):
        a = axis_pass(axis_pass(a, 0), 1)
    return a


class Canvas:
    """The padded pixel grid, in tile coordinates (x right, y down)."""

    def __init__(self):
        n = RES + 2 * PAD
        idx = (np.arange(n) - PAD + 0.5) / RES
        self.x, self.y = np.meshgrid(idx, idx)
        wrap = (np.arange(n) - PAD) % RES
        self.wy, self.wx = np.meshgrid(wrap, wrap, indexing="ij")
        # 0 at the borders, 1 in the interior where variants may differ
        self.window = self.make_window(MARGIN)
        self.wide_window = self.make_window(WIDE_MARGIN)

    def make_window(self, margin):
        return (smoothstep(0.0, margin, self.x) * smoothstep(0.0, margin, 1.0 - self.x)
              * smoothstep(0.0, margin, self.y) * smoothstep(0.0, margin, 1.0 - self.y))

    def sample(self, field):
        return field[self.wy, self.wx]

    def blend(self, base, extra, wide=False):
        """'base' at the borders, turning into 'extra' inside; keeps the variance."""
        theta = (self.wide_window if wide else self.window) * (np.pi / 2.0)
        return self.sample(base) * np.cos(theta) + self.sample(extra) * np.sin(theta)


def seg_dist(cv, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = np.clip(((cv.x - ax) * dx + (cv.y - ay) * dy) / (dx * dx + dy * dy), 0.0, 1.0)
    return np.hypot(cv.x - (ax + t * dx), cv.y - (ay + t * dy))


def arc_dist(cv, centre, radius=0.5):
    return np.abs(np.hypot(cv.x - centre[0], cv.y - centre[1]) - radius)


# Where each arm crosses its border, extended past it into the padding
ARM_END = {
    UP:    (0.5, -0.5),
    RIGHT: (1.5,  0.5),
    DOWN:  (0.5,  1.5),
    LEFT:  (-0.5, 0.5),
}
OPPOSITE = {UP: DOWN, DOWN: UP, LEFT: RIGHT, RIGHT: LEFT}
# The tile corner a bend between two arms curves around
CORNER = {
    (UP, RIGHT): (1.0, 0.0), (RIGHT, DOWN): (1.0, 1.0),
    (DOWN, LEFT): (0.0, 1.0), (LEFT, UP): (0.0, 0.0),
}


def smin(a, b, k):
    k = np.maximum(k, 1e-6)
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b * (1.0 - h) + a * h - k * h * (1.0 - h)


def geometry(cv, mask):
    """Distance to the road's centreline, the wheel track distance fields
    with their weights, how much of the pixel is junction, and how much the
    road runs along x there rather than along y."""
    arms = [a for a in (UP, RIGHT, DOWN, LEFT) if mask & a]
    centre = (0.5, 0.5)
    r_centre = np.hypot(cv.x - 0.5, cv.y - 0.5)
    junction = np.zeros_like(cv.x)

    if len(arms) == 0:
        dist = r_centre + 0.02
        return dist, [], junction, np.full_like(cv.x, 0.5)

    bends = [pair for pair in CORNER if pair[0] in arms and pair[1] in arms]

    if len(arms) == 2 and len(bends) == 1:
        cx, cy = CORNER[bends[0]]
        d = arc_dist(cv, (cx, cy))
        rx, ry = cv.x - cx, cv.y - cy
        along_x = ry * ry / np.maximum(rx * rx + ry * ry, 1e-9)
        return d, [(d, 1.0)], junction, along_x

    arm_dists = [seg_dist(cv, centre, ARM_END[a]) for a in arms]
    if len(arms) <= 2:
        dist = np.minimum.reduce(arm_dists)
    else:
        # Fillets in the junction, none near the borders so the cross-sections match
        border = np.minimum(np.minimum(cv.x, 1.0 - cv.x), np.minimum(cv.y, 1.0 - cv.y))
        k = 0.16 * smoothstep(0.02, 0.2, border)
        dist = arm_dists[0]
        for d in arm_dists[1:]:
            dist = smin(dist, d, k)
        junction = 1.0 - smoothstep(0.12, 0.38, r_centre)

    weights = [np.exp(-(d / 0.08) ** 2) + 1e-6 for d in arm_dists]
    along_x = sum(w for a, w in zip(arms, weights) if a in (LEFT, RIGHT)) / sum(weights)

    tracks = []
    done = set()
    for a in arms:
        if a in done:
            continue
        if OPPOSITE[a] in arms:
            tracks.append((seg_dist(cv, ARM_END[a], ARM_END[OPPOSITE[a]]), 1.0))
            done.update((a, OPPOSITE[a]))
        else:
            # A dead end's tracks fade out before the cap instead of looping round it
            fade = 1.0 if len(arms) > 1 else smoothstep(0.04, 0.3, r_centre)
            tracks.append((seg_dist(cv, centre, ARM_END[a]), fade))
            done.add(a)
    if len(arms) >= 3:
        for pair in bends:
            tracks.append((arc_dist(cv, CORNER[pair]), 0.55 * (1.0 - smoothstep(0.0, 0.1, -np.minimum(cv.x, cv.y)))))
    return dist, tracks, junction, along_x


def scatter(rng, count, rmin, rmax):
    return {
        "x": rng.random(count), "y": rng.random(count),
        "r": rmin + (rmax - rmin) * rng.random(count) ** 2.0,
        "shade": rng.random(count),
        "phase": rng.random((count, 3)) * 2.0 * np.pi,
        "lumps": rng.uniform(0.1, 0.3, (count, 2)),
        "jitter": rng.standard_normal((count, 3)),
        "keep": rng.random(count),
    }


class Specks:
    """Stones drawn into a colour layer, a coverage layer and a height layer."""

    def __init__(self, shape):
        self.color = np.zeros(shape + (3,), np.float32)
        self.alpha = np.zeros(shape, np.float32)
        self.height = np.zeros(shape, np.float32)

    def stamp(self, specks, interior, density, palette, relief):
        """Draws the specks inside the margin or the ones in it, as 'interior'
        says, each kept where 'density' at its centre beats its own draw.
        Specks near a border are drawn wrapped round as well."""
        n = self.alpha.shape[0]
        reach = (PAD + 10) / RES
        for i in range(len(specks["x"])):
            sx, sy = specks["x"][i], specks["y"][i]
            inside = MARGIN < sx < 1 - MARGIN and MARGIN < sy < 1 - MARGIN
            if inside != interior:
                continue
            r = specks["r"][i]
            col = palette[int(specks["shade"][i] * len(palette)) % len(palette)]
            col = col * (1.0 + 0.06 * specks["jitter"][i][0]) + 4.0 * specks["jitter"][i]
            rpx = int(np.ceil(r)) + 2
            oxs = [0] + ([-1] if sx > 1 - reach else []) + ([1] if sx < reach else [])
            oys = [0] + ([-1] if sy > 1 - reach else []) + ([1] if sy < reach else [])
            for ox in oxs:
                for oy in oys:
                    cx = (sx + ox) * RES + PAD
                    cy = (sy + oy) * RES + PAD
                    if not (-rpx < cx < n + rpx and -rpx < cy < n + rpx):
                        continue
                    if density[min(max(int(cy), 0), n - 1), min(max(int(cx), 0), n - 1)] <= specks["keep"][i]:
                        continue
                    x0, x1 = max(int(cx) - rpx, 0), min(int(cx) + rpx + 1, n)
                    y0, y1 = max(int(cy) - rpx, 0), min(int(cy) + rpx + 1, n)
                    yy, xx = np.mgrid[y0:y1, x0:x1]
                    # Angular outlines: the radius wobbles with the angle
                    ph, lm = specks["phase"][i], specks["lumps"][i]
                    theta = np.arctan2(yy + 0.5 - cy, xx + 0.5 - cx)
                    rt = r * (1.0 + lm[0] * np.sin(3 * theta + ph[0]) + lm[1] * np.sin(5 * theta + ph[1])
                              + 0.15 * np.sin(2 * theta + ph[2]))
                    d = np.hypot(xx + 0.5 - cx, yy + 0.5 - cy)
                    a = np.clip(rt + 0.5 - d, 0.0, 1.0)
                    dome = np.sqrt(np.clip(1.0 - (d / (rt + 0.5)) ** 2, 0.0, 1.0)) * r * relief
                    sl = (slice(y0, y1), slice(x0, x1))
                    self.color[sl] = self.color[sl] * (1 - a[..., None]) + col * a[..., None]
                    self.alpha[sl] = np.maximum(self.alpha[sl], a)
                    self.height[sl] = np.maximum(self.height[sl], dome)


def shade(height, strength):
    gy, gx = np.gradient(height)
    light = np.array([-0.55, -0.6, 0.58])
    light /= np.linalg.norm(light)
    nx, ny, nz = -gx * strength, -gy * strength, np.ones_like(height)
    norm = np.sqrt(nx * nx + ny * ny + nz * nz)
    lambert = (nx * light[0] + ny * light[1] + nz * light[2]) / norm
    return 1.0 + 1.6 * (lambert - light[2])


class Fields:
    """The noise and scatter of one variant. The shared 'base' set shows only
    near the borders, so any two variants stitch."""

    # name: (beta, fmin, stretch)
    NOISE = {
        "tone":     (1.6, 1.0, (1, 1)),
        "mottle":   (1.0, 4.0, (1, 1)),
        "grain":    (0.5, 16.0, (1, 1)),
        "streak_x": (1.0, 4.0, (8, 1)),
        "streak_y": (1.0, 4.0, (1, 8)),
        "edge":     (1.6, 3.0, (1, 1)),
        "fray":     (0.9, 10.0, (1, 1)),
        "windrow":  (1.3, 3.0, (1, 1)),
        "soil":     (1.2, 3.0, (1, 1)),
        "grass":    (1.0, 6.0, (1, 1)),
        "tuft":     (0.6, 14.0, (1, 1)),
        "dry":      (1.4, 2.0, (1, 1)),
        "meander":  (1.5, 2.0, (1, 1)),
        "wander":   (1.3, 3.0, (1, 1)),
        "fuzz":     (0.8, 12.0, (1, 1)),
        "patch":    (1.2, 5.0, (1, 1)),
        "wear":     (1.4, 2.0, (1, 1)),
        "jag":      (0.7, 24.0, (1, 1)),
        "crisp":    (1.3, 2.0, (1, 1)),
    }
    BROAD = {"tone", "mottle", "edge", "windrow", "soil", "dry", "meander", "wander", "wear", "crisp"}

    def __init__(self, seed, variant):
        self.base = self.make(np.random.default_rng(seed))
        self.extra = self.make(np.random.default_rng(seed * 7919 + variant))

    @classmethod
    def make(cls, rng):
        out = {name: periodic_noise(rng, beta, fmin, stretch) for name, (beta, fmin, stretch) in cls.NOISE.items()}
        out["grit"] = scatter(rng, 22000, 0.8, 1.7)
        out["gravel"] = scatter(rng, 4000, 1.6, 3.0)
        out["pebbles"] = scatter(rng, 1400, 2.5, 5.5)
        return out

    def get(self, cv, name):
        return cv.blend(self.base[name], self.extra[name], name in self.BROAD)

    def stamp(self, specks, name, density, palette, relief):
        specks.stamp(self.base[name], False, density, palette, relief)
        specks.stamp(self.extra[name], True, density, palette, relief)


def mix(a, b, t):
    return a + (b - a) * t[..., None]


def render_tile(cv, fields, mask):
    dist, tracks, junction, along_x = geometry(cv, mask)

    edge = fields.get(cv, "edge")
    fray = fields.get(cv, "fray")
    # Bounded, so the edge never reaches an unlinked border
    wobble = 0.026 * np.tanh((0.018 * edge + 0.003 * fray) / 0.026)
    s = dist / HALF_WIDTH
    s_edge = (dist + wobble) / HALF_WIDTH
    s_mid = (dist + 0.5 * wobble) / HALF_WIDTH

    cover = smoothstep(1.0 + FEATHER, 1.0 - FEATHER, s_edge)
    # Every band boundary wanders on its own and is broken up finely, so 
    # none of them reads as a painted line from afar
    # The edges are crisp in some stretches and soft in others
    fuzz = fields.get(cv, "fuzz")
    jag = fields.get(cv, "jag")
    crisp = smoothstep(-0.8, 0.8, fields.get(cv, "crisp"))
    soil_n = fields.get(cv, "soil")
    s_way = s_edge + 0.12 * soil_n + 0.07 * fuzz + 0.03 * jag
    way_soft = 0.16 - 0.13 * crisp
    way = smoothstep(SHOULDER + way_soft, SHOULDER - way_soft, s_way)
    shoulder = (1.0 - way) * cover

    track = np.zeros_like(dist)
    crown = np.zeros_like(dist)
    track_shift = 0.1 * fields.get(cv, "wander") + 0.06 * fuzz + 0.03 * jag
    track_soft = 0.12 - 0.1 * crisp
    for d, w in tracks:
        off = np.abs(d / HALF_WIDTH + track_shift - TRACK_OFFSET)
        track = np.maximum(track, w * smoothstep(TRACK_HALF + track_soft, TRACK_HALF - track_soft, off))
        crown = np.maximum(crown, w * np.exp(-((d / HALF_WIDTH) / CROWN) ** 2))
    # Worn in some stretches more than others
    track *= (1.0 - 0.6 * junction) * smoothstep(-1.2, 0.8, fields.get(cv, "wear"))
    crown *= (1.0 - junction)
    track *= way
    crown *= way

    # Heaped gravel meanders and swells along the road
    windrow_n = fields.get(cv, "windrow")
    wr_centre = WINDROW + 0.11 * fields.get(cv, "meander") - 0.06 * fuzz
    wr_sigma = WINDROW_SIGMA * (1.0 + 0.35 * np.tanh(windrow_n))
    windrow = np.minimum(1.0, 1.5 * np.exp(-((s_mid - wr_centre) / wr_sigma) ** 2)
                         * smoothstep(-0.2, 0.7, windrow_n))

    streak_x = fields.get(cv, "streak_x")
    streak_y = fields.get(cv, "streak_y")
    streak = (along_x * streak_x + (1.0 - along_x) * streak_y) / np.sqrt(along_x ** 2 + (1.0 - along_x) ** 2)
    grain = fields.get(cv, "grain")

    # The travelled surface: pale compacted tracks, a greyer crown
    light = np.clip(0.33 + 0.5 * track + 0.14 * fields.get(cv, "tone")
                    + 0.1 * fields.get(cv, "mottle") + 0.2 * streak, 0.0, 1.0)
    color = mix(WAY_MID, WAY_LIGHT, light)
    color = mix(color, CROWN_GREY, 0.45 * crown)
    color = mix(color, GRAVEL_BED, 0.3 * box_blur(windrow, 7))

    # The soil shoulder, darker and redder, its edge soft
    soil = mix(SOIL_LIGHT, SOIL_DARK, np.clip(0.5 + 0.35 * soil_n, 0.0, 1.0))
    color = mix(soil, color, way)
    color *= (1.0 + 0.025 * grain)[..., None]

    # Grass creeping in from the edge: the terrain shows through the holes,
    # tinted at their rims so they read as tufts rather than cut-outs
    outward = smoothstep(SHOULDER + 0.06, 1.03, s_edge)
    tuft = fields.get(cv, "tuft")
    grass_amt = smoothstep(0.5, 0.85, outward + 0.25 * fields.get(cv, "grass") + 0.03 * tuft)
    grass_col = mix(GRASS_GREEN, GRASS_DRY, smoothstep(-0.6, 0.8, fields.get(cv, "dry")))
    color = mix(color, grass_col, 0.7 * smoothstep(0.1, 0.6, grass_amt))

    # Stones: fine grit everywhere, gravel on the crown, heaped in the windrows
    bare = cover * (1.0 - grass_amt)
    stray = smoothstep(STRAY_LIMIT, STRAY_LIMIT - 0.04, s) * smoothstep(0.8, 1.6, fray)
    patch = smoothstep(-0.3, 1.2, fields.get(cv, "patch"))
    grit_d = bare * (way * (0.35 + 0.5 * patch - 0.3 * track + 0.3 * crown) + 0.95 * windrow + 0.1 * shoulder)
    gravel_d = bare * (way * (0.01 + 0.12 * patch - 0.03 * track + 0.15 * crown) + 0.9 * windrow + 0.02 * shoulder)
    pebble_d = bare * (0.008 * way + 0.9 * windrow + 0.01 * shoulder) + 0.1 * stray * (1.0 - cover)

    sp = Specks(dist.shape)
    fields.stamp(sp, "grit", grit_d, GRIT_COLOURS, 0.25)
    fields.stamp(sp, "gravel", gravel_d, PEBBLE_COLOURS, 0.35)
    fields.stamp(sp, "pebbles", pebble_d, PEBBLE_COLOURS, 0.45)

    # Contact shadow round each stone
    occl = np.clip(box_blur(sp.height, 2) - sp.height, 0.0, None)
    color *= (1.0 - np.clip(0.18 * occl, 0.0, 0.3))[..., None]
    color = mix(color, sp.color, sp.alpha)

    # Baked relief: tracks pressed down, windrows heaped up, stones proud
    height = (-0.6 * track + 0.9 * windrow - 0.3 * shoulder + 0.06 * grain + 0.1 * streak
              + sp.height)
    lit = np.clip(shade(height, 0.9), 0.72, 1.28)
    color *= lit[..., None]

    alpha = cover * (1.0 - 0.8 * grass_amt)
    alpha = np.maximum(alpha, sp.alpha * smoothstep(STRAY_LIMIT, STRAY_LIMIT - 0.02, s))

    rgba = np.dstack([np.clip(color, 0, 255), np.clip(alpha * 255, 0, 255)])
    return rgba[PAD:PAD + RES, PAD:PAD + RES].astype(np.uint8)


def contact_sheet(tiles, layout, path, scale=0.5):
    """Composites a road layout over flat grass to check the seams."""
    rows, cols = len(layout), len(layout[0])
    tile = int(RES * scale)
    sheet = Image.new("RGBA", (cols * tile, rows * tile), (94, 119, 55, 255))
    for r, row in enumerate(layout):
        for c, mask in enumerate(row):
            if mask is None:
                continue
            variants = tiles[mask]
            img = Image.fromarray(variants[(r * 7 + c * 3) % len(variants)], "RGBA")
            sheet.alpha_composite(img.resize((tile, tile), Image.LANCZOS), (c * tile, r * tile))
    sheet.convert("RGB").save(path)


def links_layout(cells):
    """Masks for a set of (r, c) cells linked to every orthogonal neighbour in the set."""
    rows = max(r for r, _ in cells) + 1
    cols = max(c for _, c in cells) + 1
    out = [[None] * cols for _ in range(rows)]
    for r, c in cells:
        m = 0
        m |= UP if (r - 1, c) in cells else 0
        m |= DOWN if (r + 1, c) in cells else 0
        m |= LEFT if (r, c - 1) in cells else 0
        m |= RIGHT if (r, c + 1) in cells else 0
        out[r][c] = m
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("outdir")
    parser.add_argument("--variants", type=int, default=4)
    parser.add_argument("--seed", type=int, default=1709)
    parser.add_argument("--masks", help="comma separated masks to render, all by default")
    parser.add_argument("--preview", help="path of a contact sheet to write")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    masks = [int(m) for m in args.masks.split(",")] if args.masks else list(range(16))
    cv = Canvas()
    tiles = {m: [] for m in range(16)}
    for v in range(args.variants):
        fields = Fields(args.seed, v)
        for mask in masks:
            rgba = render_tile(cv, fields, mask)
            tiles[mask].append(rgba)
            Image.fromarray(rgba, "RGBA").save(os.path.join(args.outdir, "road-%02d-%d.png" % (mask, v)), optimize=True)
        print("variant %d done" % v)

    if args.preview and len(masks) == 16:
        cells = set()
        cells.update((1, c) for c in range(0, 8))
        cells.update((r, 3) for r in range(1, 6))
        cells.update((5, c) for c in range(3, 7))
        cells.update((r, 6) for r in range(3, 8))
        cells.update((3, c) for c in range(6, 9))
        cells.add((7, 1))
        contact_sheet(tiles, links_layout(cells), args.preview)


if __name__ == "__main__":
    main()
