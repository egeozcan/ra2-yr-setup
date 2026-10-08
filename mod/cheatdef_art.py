#!/usr/bin/env python3
"""Build the Cheat Defense art from one procedural 3D scene (signed distance functions).

usage: cheatdef_art.py [OUT_DIR]      (default: mod/assets; needs numpy + Pillow)

Writes (ART = art/image ID, second letter replaced per theater like stock NewTheater art):
  ggchdf.shp            base: normal, damaged, damaged + 3 shadow frames (generic 'G' theater file)
  g?chdfmk.shp          12-frame build-up + 12 shadow frames, one identical copy per theater letter
  chdfglow.shp          16-frame looping active animation (glow ring, crystals, arcs)
  chdftur.vxl/.hva      rotating turret voxel (the building's TurretAnim)
  chdficon.shp          60x48 sidebar cameo
and previews to mod/previews/cheatdef-*.png.

World units: 1 unit = 1 screen pixel horizontally (the stock voxel scale), x/y on the ground,
z up, origin at the centre of the 2x2 foundation (a cell is 256 leptons = 42.43 units).
Screen projection (RA2 dimetric): sx = cx + (x-y)/sqrt2, sy = cy + (x+y)/sqrt8 - z*cos30.
"""
import math, os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import render as R, vxl, ra2paths

GAME = ra2paths.GAME
FA2 = os.path.join(GAME, "FinalAlert2")
THEATER_LETTERS = "TAUDLNG"      # temperate, snow, urban, desert, lunar, new urban, generic fallback
BASE_SHP = "ggchdf.shp"
BUILDUP_SHPS = [f"g{c.lower()}chdfmk.shp" for c in THEATER_LETTERS]
ANIM_SHP = "chdfglow.shp"
TURRET = "chdftur"
CAMEO = "chdficon.shp"
FILES = [BASE_SHP, ANIM_SHP, TURRET + ".vxl", TURRET + ".hva", CAMEO] + BUILDUP_SHPS

W, H = 232, 168                  # stock Grand Cannon canvas
CX, CY = 116.0, 112.0            # screen position of the foundation centre on the ground
C30, S30, R2 = math.cos(math.radians(30)), 0.5, math.sqrt(2)
LIGHT = np.array([-0.7, 1.3, 1.6]); LIGHT /= np.linalg.norm(LIGHT)   # stock shadows fall up-right
VIEW = -np.array([-1, -1, -0.8165]) / np.linalg.norm([1, 1, 0.8165])  # towards the viewer
TURRET_Z0 = 23.0                 # deck height; the turret voxel starts here
DESIGN_Z0 = 13.0                 # the same deck height in turret design units
EMITTER = (35.0, 0.0, 24.0)      # prong tips, where the bolt should start (turret design units)
RING_R, RING_Z = 23.5, 22.5      # glow channel in the deck
CRYSTAL_Z = 38.0                 # pylon emitter crystals

# ---- materials and their palette ramps (unit palettes; every index here is identical in all six).
# Proportions follow the stock Allied buildings: mostly dark gunmetal, grime and steel,
# house colour on panels, white only as chrome highlight bands, small icy-blue lights.
STEEL, DARK, REMAP, COPPER, GLOW, CONCRETE, CHROME, HOT, REMAP_SIDES, GRIME = range(1, 11)
NMAT = 11
RAMPS = {
    STEEL: list(range(80, 96)),
    DARK: list(range(44, 64)),
    CONCRETE: list(range(64, 80)),
    REMAP: list(range(16, 32)),
    COPPER: list(range(176, 192)),
    GLOW: [15, 251, 192, 252, 193, 194, 195, 196, 197, 198],
    CHROME: list(range(32, 56)),
    HOT: [15, 176, 177, 178, 180, 184, 247, 248],
    GRIME: list(range(112, 128)),
}
BASE_INDEX = {STEEL: 87, DARK: 54, REMAP: 20, COPPER: 183, GLOW: 251, CONCRETE: 67, CHROME: 38, HOT: 178,
              GRIME: 113}
# Voxel colours. The engine shades each voxel through voxels.vpl: 32 light levels x 256 indices. On a
# building turret it mostly uses the dark levels (VPL_LIGHT, fitted to an in-game screenshot), so the
# base indices sit towards the light end of each ramp. Glow uses 192/81/193, which stay blue-white
# there; 15 turns grey and 251 turns magenta (204-207). Baked AO/edges shift within the ramp.
VOXEL_RAMP = {STEEL: (list(range(80, 96)), 91), DARK: (list(range(40, 64)), 55), REMAP: (list(range(16, 32)), 22),
              COPPER: (list(range(176, 192)), 187), CHROME: (list(range(32, 48)), 40),
              GRIME: (list(range(112, 128)), 114)}
GLOW_VOXEL, GLOW_VOXEL_HOT, GLOW_VOXEL_RIM = 192, 81, 193
# light level = a + w . normal (world axes), fitted to two in-game screenshots of earlier turrets at
# different facings (rmse ~5 of 32 levels). Faces towards world +y (screen lower-left) are lit;
# up-facing tops get only level ~4, so tops use lighter indices (see turret_voxel).
VPL_LIGHT = (6.17, 0.63, 6.14, -2.35)


# ------------------------------------------------------------------ SDF primitives
def length2(a, b):
    return np.sqrt(a * a + b * b)


def sd_box(x, y, z, cx, cy, cz, hx, hy, hz, r=0.0):
    qx, qy, qz = np.abs(x - cx) - hx + r, np.abs(y - cy) - hy + r, np.abs(z - cz) - hz + r
    out = np.sqrt(np.maximum(qx, 0) ** 2 + np.maximum(qy, 0) ** 2 + np.maximum(qz, 0) ** 2)
    return out + np.minimum(np.maximum(np.maximum(qx, qy), qz), 0) - r


def sd_cyl_z(x, y, z, cx, cy, r, z0, z1):
    dr = length2(x - cx, y - cy) - r
    dz = np.maximum(z0 - z, z - z1)
    return np.minimum(np.maximum(dr, dz), 0) + length2(np.maximum(dr, 0), np.maximum(dz, 0))


def sd_cyl_x(x, y, z, cy, cz, r, x0, x1):
    return sd_cyl_z(y, z, x, cy, cz, r, x0, x1)


def sd_sphere(x, y, z, cx, cy, cz, r):
    return np.sqrt((x - cx) ** 2 + (y - cy) ** 2 + (z - cz) ** 2) - r


def sd_torus_z(x, y, z, R_, r, cz):
    return length2(length2(x, y) - R_, z - cz) - r


def sd_capsule(x, y, z, a, b, r):
    a, b = np.asarray(a, float), np.asarray(b, float)
    ba = b - a
    px, py, pz = x - a[0], y - a[1], z - a[2]
    h = np.clip((px * ba[0] + py * ba[1] + pz * ba[2]) / ba.dot(ba), 0, 1)
    return np.sqrt((px - ba[0] * h) ** 2 + (py - ba[1] * h) ** 2 + (pz - ba[2] * h) ** 2) - r


def sd_octagon(x, y, z, a, z0, z1, taper=0.0, corner=None):
    """Square prism (faces along x and y) with chamfered corners, half-width a at z0 shrinking by
    `taper` per unit up; the chamfer faces sit `corner` from the axis (default: a regular-ish octagon)."""
    k = 1 / math.sqrt(1 + taper * taper)
    ins = taper * (np.clip(z, z0, z1) - z0)
    corner = a / 1.04 if corner is None else corner
    d2 = np.maximum(np.maximum(np.abs(x), np.abs(y)) - (a - ins), (np.abs(x) + np.abs(y)) * 0.7071 - (corner - ins))
    dz = np.maximum(z0 - z, z - z1)
    return np.maximum(d2 * k, dz)


class Acc:
    """Running union of (distance, material)."""

    def __init__(self, n):
        self.d = np.full(n, 1e9)
        self.m = np.zeros(n, np.int8)

    def add(self, d, mat):
        w = d < self.d
        self.d = np.where(w, d, self.d)
        self.m = np.where(w, mat, self.m)


# ------------------------------------------------------------------ the scene
PYLONS = [(sx * 33.0, sy * 33.0) for sx in (-1, 1) for sy in (-1, 1)]


def base_sdf(x, y, z, acc, clip=None, pylon_clip=None):
    """Dirt/concrete pad, gunmetal foundation, a ribbed house-colour drum with chrome bands,
    a deck with a glow channel, and four tall banded emitter pylons on the corners."""
    def c(d, h):
        return d if h is None else np.maximum(d, z - h)
    acc.add(c(sd_box(x, y, z, 0, 0, 0.3, 41.5, 41.5, 0.45, 0.4), clip), GRIME)             # ground pad
    acc.add(c(sd_octagon(x, y, z, 35, 0.4, 4.2, taper=0.6, corner=45), clip), CONCRETE)      # foundation
    acc.add(c(sd_octagon(x, y, z, 35.8, 0.4, 1.4, corner=46), clip), DARK)                   # kerb
    for px_, py_ in PYLONS:
        acc.add(c(sd_cyl_z(x, y, z, px_, py_, 6.5, 0.4, 3.2), clip), CONCRETE)               # pylon pads
    # drum
    acc.add(c(sd_cyl_z(x, y, z, 0, 0, 29.0, 4.0, 6.6), clip), DARK)                          # skirt
    acc.add(c(sd_cyl_z(x, y, z, 0, 0, 28.6, 6.6, 7.5), clip), CHROME)                        # chrome band
    acc.add(c(sd_cyl_z(x, y, z, 0, 0, 27.0, 7.5, 19.7), clip), REMAP)                        # house panels
    ang = np.arctan2(y, x)
    k = np.round(ang / (math.pi / 8)) * (math.pi / 8)                                        # 16 ribs
    rx, ry = x * np.cos(k) + y * np.sin(k), -x * np.sin(k) + y * np.cos(k)
    acc.add(c(sd_box(rx, ry, z, 27.2, 0, 13.6, 1.1, 0.9, 6.1, 0.3), clip), DARK)
    acc.add(c(sd_cyl_z(x, y, z, 0, 0, 28.3, 19.7, 21.1), clip), STEEL)                       # top rim
    deck = sd_cyl_z(x, y, z, 0, 0, 26.5, 20.9, 22.4)
    deck = np.maximum(deck, -sd_torus_z(x, y, z, RING_R, 1.7, RING_Z))                      # glow channel
    acc.add(c(deck, clip), STEEL)
    acc.add(c(sd_torus_z(x, y, z, RING_R, 1.05, RING_Z - 0.5), clip), GLOW)
    acc.add(c(sd_cyl_z(x, y, z, 0, 0, 21.0, 22.1, TURRET_Z0), clip), DARK)                  # turret race
    ph = pylon_clip if pylon_clip is not None else clip
    for px_, py_ in PYLONS:
        acc.add(c(sd_cyl_z(x, y, z, px_, py_, 4.6, 3.0, 5.5), ph), DARK)                     # plinth
        acc.add(c(sd_cyl_z(x, y, z, px_, py_, 3.2, 5.5, 35.0), ph), DARK)                    # shaft
        acc.add(c(sd_cyl_z(x, y, z, px_, py_, 3.6, 13.0, 20.0), ph), REMAP)                  # house sleeve
        for bz in (8.5, 22.5, 29.5):
            acc.add(c(sd_cyl_z(x, y, z, px_, py_, 3.9, bz, bz + 1.3), ph), CHROME)          # chrome bands
        acc.add(c(sd_cyl_z(x, y, z, px_, py_, 4.3, 34.0, 35.7), ph), STEEL)                  # cap
        for j in range(3):                                                                   # emitter fins
            a = j * 2 * math.pi / 3 + math.atan2(py_, px_)
            fx, fy = px_ + 2.6 * math.cos(a), py_ + 2.6 * math.sin(a)
            acc.add(c(sd_capsule(x, y, z, (fx, fy, 35.0), (fx + 0.4 * math.cos(a), fy + 0.4 * math.sin(a), 40.0), 0.6),
                      ph), STEEL)
        acc.add(c(sd_sphere(x, y, z, px_, py_, CRYSTAL_Z, 2.1), ph), GLOW)                   # crystal
        # power conduit into the drum
        f0, f1 = 0.86, 27.5 / math.hypot(px_, py_)
        acc.add(c(sd_capsule(x, y, z, (px_ * f0, py_ * f0, 5.5), (px_ * f1, py_ * f1, 9.5), 1.5), ph), DARK)
        acc.add(c(sd_cyl_z(x, y, z, px_ * 0.78, py_ * 0.78, 2.2, 6.0, 7.8), ph), STEEL)     # joint


TS = 1.3    # turret design units -> world units (collar radius 16 -> 20.8, inside the deck's race)


def turret_sdf(x, y, z, acc, clip=None, collar_only=False):
    """The turret, scaled by TS around the deck centre."""
    inner = Acc(x.shape[0])
    dclip = None if clip is None else DESIGN_Z0 + (clip - TURRET_Z0) / TS
    turret_design(x / TS, y / TS, DESIGN_Z0 + (z - TURRET_Z0) / TS, inner, dclip, collar_only)
    acc.add(inner.d * TS, inner.m)


def sd_prism(x, y, z, poly, z0, z1, taper=0.0):
    """Convex polygon (counter-clockwise xy points) extruded z0..z1, edges moving in by `taper` per unit up."""
    k = 1 / math.sqrt(1 + taper * taper)
    ins = taper * (np.clip(z, z0, z1) - z0)
    d2 = None
    for (ax, ay), (bx, by) in zip(poly, poly[1:] + poly[:1]):
        ex, ey = bx - ax, by - ay
        ln = math.hypot(ex, ey)
        d = -(ex * (y - ay) - ey * (x - ax)) / ln + ins
        d2 = d if d2 is None else np.maximum(d2, d)
    return np.maximum(d2 * k, np.maximum(z0 - z, z - z1))


def sd_halfspace(x, y, z, n, p0):
    """Signed distance to the plane through p0 with outward normal n (positive outside)."""
    n = np.asarray(n, float) / np.linalg.norm(n)
    return (x - p0[0]) * n[0] + (y - p0[1]) * n[1] + (z - p0[2]) * n[2]


def turret_design(x, y, z, acc, clip=None, collar_only=False):
    """Turret in local design units (+x forward, mirror-symmetric in y), deck at z = 13.
    Built like the stock voxel turrets: a tall angular housing of large faces at distinct angles
    (sloped front plate, leaning side armour, stepped rear), few small details."""
    def c(d):
        return d if clip is None else np.maximum(d, z - clip)
    acc.add(c(sd_octagon(x, y, z, 15.0, 13.0, 15.2, corner=15.6)), DARK)          # rotating collar
    acc.add(c(sd_octagon(x, y, z, 14.2, 15.2, 16.0, corner=14.8)), CHROME)
    if collar_only:
        return
    # main housing, its top sliced off towards the front as a sloped glacis
    body = [(-11, -9.5), (5, -11), (11, -7.5), (13.5, -4), (13.5, 4), (11, 7.5), (5, 11), (-11, 9.5)]
    hous = sd_prism(x, y, z, body, 16.0, 31.0, taper=0.12)
    glacis = sd_halfspace(x, y, z, (0.75, 0, 1), (2.0, 0, 31.0))
    acc.add(c(np.maximum(hous, glacis)), REMAP_SIDES)
    # roof plate and a hatch, set back behind the glacis
    acc.add(c(np.maximum(sd_prism(x, y, z, [(-10, -8), (1.5, -8), (1.5, 8), (-10, 8)], 30.6, 31.6), glacis - 0.4)),
            STEEL)
    acc.add(c(sd_octagon(x + 4.5, y, z, 2.6, 31.2, 32.6)), DARK)
    # leaning side armour slabs with a dark gap to the housing
    for sy in (-1, 1):
        slab = sd_prism(x, y * sy, z, [(-9, 10.6), (7, 11.8), (7, 14.0), (-9, 12.8)], 17.0, 28.5, taper=0.18)
        acc.add(c(slab), REMAP_SIDES)
        acc.add(c(sd_box(x, y, z, -1.0, 11.0 * sy, 22.8, 7.5, 0.7, 5.0)), DARK)
    # stepped rear block with capacitor drums
    acc.add(c(sd_prism(x, y, z, [(-15, -8), (-10, -8), (-10, 8), (-15, 8)], 17.0, 27.0, taper=0.2)), DARK)
    for sy in (-1, 1):
        acc.add(c(sd_cyl_x(x, y, z, 5.3 * sy, 21.5, 2.4, -17.5, -14.5)), STEEL)
        acc.add(c(sd_cyl_x(x, y, z, 5.3 * sy, 21.5, 1.5, -18.5, -17.5)), GLOW)
    # emitter nose: the eye under the glacis, twin prongs with bronze coils and glowing tips
    acc.add(c(sd_prism(x, y, z, [(10, -6), (15.5, -4.5), (15.5, 4.5), (10, 6)], 19.5, 26.0)), DARK)
    acc.add(c(sd_sphere(x, y, z, 15.6, 0, 22.8, 2.6)), GLOW)
    for sy in (-1, 1):
        yy = 5.0 * sy
        acc.add(c(sd_capsule(x, y, z, (14, yy, 22.8), (EMITTER[0] - 1, yy * 0.85, EMITTER[2]), 1.4)), STEEL)
        for rx in (19.0, 22.5, 26.0, 29.5):
            acc.add(c(sd_cyl_x(x, y, z, yy * 0.92, 23.2 + (rx - 14) * 0.06, 2.4, rx - 0.7, rx + 0.7)), COPPER)
        acc.add(c(sd_sphere(x, y, z, EMITTER[0], yy * 0.85, EMITTER[2], 1.9)), GLOW)
    # sensor mast on the roof: coil rings and a caged orb
    mx = -6.0
    acc.add(c(sd_octagon(x - mx, y, z, 3.0, 31.0, 33.5)), DARK)
    acc.add(c(sd_cyl_z(x, y, z, mx, 0, 1.3, 33.5, 44.5)), STEEL)
    for rz in (36.0, 40.0):
        acc.add(c(sd_cyl_z(x, y, z, mx, 0, 2.6, rz - 0.6, rz + 0.6)), COPPER)
    acc.add(c(sd_cyl_z(x, y, z, mx, 0, 3.4, 43.6, 44.6)), CHROME)
    acc.add(c(sd_sphere(x, y, z, mx, 0, 47.6, 2.8)), GLOW)
    for a in (0.0, 2 * math.pi / 3, -2 * math.pi / 3):
        rx_, ry_ = mx + 3.7 * math.cos(a), 3.7 * math.sin(a)
        acc.add(c(sd_capsule(x, y, z, (rx_, ry_, 44.3), (rx_, ry_, 51.0), 0.6)), DARK)
    acc.add(c(sd_cyl_z(x, y, z, mx, 0, 4.1, 51.0, 51.9)), DARK)
    acc.add(c(sd_cyl_z(x, y, z, mx, 0, 0.5, 51.9, 54.5)), STEEL)


def rotate(x, y, deg):
    """World -> turret-local for a turret turned `deg` degrees counter-clockwise (seen from above)."""
    t = math.radians(deg)
    ct, st = math.cos(t), math.sin(t)
    return x * ct + y * st, -x * st + y * ct


def make_scene(parts):
    """parts: dict with keys base, turret (facing deg or None), clips..."""
    def f(x, y, z):
        acc = Acc(x.shape[0])
        if parts.get("base", True):
            base_sdf(x, y, z, acc, parts.get("base_clip"), parts.get("pylon_clip"))
        if parts.get("turret") is not None:
            lx, ly = rotate(x, y, parts["turret"])
            turret_sdf(lx, ly, z, acc, parts.get("turret_clip"), parts.get("collar_only", False))
        return acc.d, acc.m
    return f


# ------------------------------------------------------------------ ray marcher
def march(scene, ox, oy, oz, d, max_t, steps=160, eps=0.04):
    n = ox.shape[0]
    t = np.zeros(n)
    hit = np.zeros(n, bool)
    alive = np.arange(n)
    for _ in range(steps):
        if alive.size == 0:
            break
        px, py, pz = ox[alive] + d[0] * t[alive], oy[alive] + d[1] * t[alive], oz[alive] + d[2] * t[alive]
        dist, _ = scene(px, py, pz)
        t[alive] += dist * 0.9
        h = dist < eps
        hit[alive[h]] = True
        keep = ~h & (t[alive] < max_t) & (pz > -0.5)
        alive = alive[keep]
    return hit, t


def normals(scene, x, y, z, e=0.12):
    g = []
    for dx, dy, dz in ((e, 0, 0), (0, e, 0), (0, 0, e)):
        g.append(scene(x + dx, y + dy, z + dz)[0] - scene(x - dx, y - dy, z - dz)[0])
    n = np.stack(g, 1)
    return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)


def gbuffer(scene, w, h, cx, cy, scale=1.0, ss=3, shadow_scene=None, ztop=84.0):
    """Render the scene at `ss`x supersampling. Returns dict of per-subpixel arrays (h*ss, w*ss)."""
    js, is_ = np.mgrid[0:h * ss, 0:w * ss]
    sx = (is_ + 0.5) / ss - cx
    sy = (js + 0.5) / ss - cy
    u, vv = (sx / scale).ravel(), (sy / scale).ravel()
    A = (vv + ztop * C30) * 2 * R2
    B = u * R2
    ox, oy, oz = (A + B) / 2, (A - B) / 2, np.full(u.shape, ztop)
    d = -VIEW
    hit, t = march(scene, ox, oy, oz, d, max_t=ztop / 0.5 + 10)
    px, py, pz = ox + d[0] * t, oy + d[1] * t, oz + d[2] * t
    out = {"hit": hit, "x": px, "y": py, "z": pz, "shape": (h * ss, w * ss), "ss": ss}
    mat = np.zeros(u.shape, np.int8)
    nrm = np.zeros((u.shape[0], 3))
    lit = np.ones(u.shape)
    ao = np.ones(u.shape)
    idx = np.flatnonzero(hit)
    if idx.size:
        _, m = scene(px[idx], py[idx], pz[idx])
        mat[idx] = m
        nrm[idx] = normals(scene, px[idx], py[idx], pz[idx])
        sh_scene = shadow_scene or scene
        o = np.stack([px[idx], py[idx], pz[idx]], 1) + nrm[idx] * 0.35
        occl, _ = march(sh_scene, o[:, 0], o[:, 1], o[:, 2], LIGHT, max_t=80, steps=90, eps=0.05)
        lit[idx] = np.where(occl, 0.0, 1.0)
        # ambient occlusion: how much geometry crowds each point along its normal
        occ = np.zeros(idx.size)
        for i, hstep in enumerate((0.7, 1.6, 3.2, 6.0, 10.0)):
            q = o + nrm[idx] * hstep
            dq, _ = sh_scene(q[:, 0], q[:, 1], q[:, 2])
            occ += np.clip(hstep - dq, 0, None) / hstep * 0.75 ** i
        ao[idx] = np.clip(1 - 0.45 * occ, 0.25, 1)
    # ground shadow: follow each ray to z = 0 and look towards the light
    tg = oz / -d[2]
    gx, gy = ox + d[0] * tg, oy + d[1] * tg
    near = (np.abs(gx) < 110) & (np.abs(gy) < 110)
    g_idx = np.flatnonzero(near)
    occl, _ = march(shadow_scene or scene, gx[g_idx], gy[g_idx], np.full(g_idx.size, 0.05), LIGHT,
                    max_t=140, steps=110, eps=0.05)
    shadow = np.zeros(u.shape, bool)
    shadow[g_idx] = occl
    out.update(mat=mat, n=nrm, lit=lit, ao=ao, shadow=shadow)
    return out


def value_noise(x, y, z, scale):
    """Smooth 3D value noise in [-1, 1]."""
    p = np.stack([x, y, z], 1) * scale
    i = np.floor(p)
    f = p - i
    f = f * f * (3 - 2 * f)
    def h(ix, iy, iz):
        v = np.sin(ix * 127.1 + iy * 311.7 + iz * 74.7) * 43758.5453
        return (v - np.floor(v)) * 2 - 1
    out = 0
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = (f[:, 0] if dx else 1 - f[:, 0]) * (f[:, 1] if dy else 1 - f[:, 1]) * (f[:, 2] if dz else 1 - f[:, 2])
                out = out + w * h(i[:, 0] + dx, i[:, 1] + dy, i[:, 2] + dz)
    return out


def shade(gb, pal, glow_level=1.0):
    """Per-subpixel RGB and effective material (REMAP_SIDES resolved to REMAP / STEEL).
    Stock-like look: low ambient, strong key light, AO in crevices, grime noise, chrome reflections."""
    mat = gb["mat"].copy()
    n = gb["n"]
    x, y, z = gb["x"], gb["y"], gb["z"]
    tops = n[:, 2] > 0.72
    mat[(mat == REMAP_SIDES) & tops] = STEEL
    mat[mat == REMAP_SIDES] = REMAP
    grime = 0.6 * value_noise(x, y, z, 0.18) + 0.4 * value_noise(x, y, z, 0.55)
    # ground pad: dirt with concrete slabs showing through
    pad = (mat == GRIME) & (z < 1.0)
    mat[pad & (grime > 0.12)] = CONCRETE
    base = np.zeros((mat.size, 3))
    for m_, i in BASE_INDEX.items():
        base[mat == m_] = pal[i]
    ndl = n @ LIGHT
    diff = np.clip(ndl, 0, None) * gb["lit"]
    refl = 2 * ndl[:, None] * n - LIGHT
    spec = np.clip(refl @ VIEW, 0, None) ** 22 * gb["lit"]
    # environment reflection (sky above, dark ground below) gives chrome its banding
    rv = 2 * (n @ VIEW)[:, None] * n - VIEW
    env = np.clip(rv[:, 2], 0, 1) ** 2
    ao = gb["ao"]
    k = (0.22 + 0.95 * diff) * ao * (1 + 0.16 * grime)
    rgb = base * k[:, None] * 1.08
    spec_amt = np.select([np.isin(mat, [CHROME, STEEL]), mat == REMAP, np.isin(mat, [DARK, COPPER])],
                         [0.9, 0.45, 0.35], 0.05)
    rgb += (spec_amt * spec * 255 * ao)[:, None]
    ch = mat == CHROME
    rgb[ch] = rgb[ch] * 0.6 + (env[ch] * 150 * ao[ch])[:, None]
    # emissive parts: white-hot where facing the viewer, icy Allied blue at the rim
    g = mat == GLOW
    facing = np.clip(n[g] @ VIEW, 0, 1) ** 2
    core = np.array([255, 255, 255.]) * facing[:, None] + np.array([120, 150, 240.]) * (1 - facing[:, None])
    dim = np.array([45, 50, 105.])
    rgb[g] = dim + (core - dim) * glow_level
    return np.clip(rgb, 0, 255), mat


BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16 - 0.5


def quantize(rgb, mat, hit, shadow, shape, ss, pal, cover=0.5):
    """Downsample to pixels and map to palette indices within each material's ramp.
    Returns (index image, shadow mask image)."""
    hh, ww = shape[0] // ss, shape[1] // ss
    rgb = rgb.reshape(hh, ss, ww, ss, 3)
    hit4 = hit.reshape(hh, ss, ww, ss)
    cov = hit4.mean((1, 3))
    wsum = np.maximum(hit4.sum((1, 3)), 1)[..., None]
    col = (rgb * hit4[..., None]).sum((1, 3)) / wsum
    m4 = mat.reshape(hh, ss, ww, ss).transpose(0, 2, 1, 3).reshape(hh, ww, ss * ss)
    counts = np.stack([(m4 == k).sum(-1) for k in range(NMAT)], -1)
    counts[..., 0] = 0
    major = counts.argmax(-1)
    out = np.zeros((hh, ww), np.uint8)
    solid = cov >= cover
    dither = np.tile(BAYER, (hh // 4 + 1, ww // 4 + 1))[:hh, :ww] * 18
    for m_, ramp in RAMPS.items():
        sel = solid & (major == m_)
        if not sel.any():
            continue
        c = col[sel] + dither[sel][:, None]
        cand = pal[ramp].astype(float)
        out[sel] = np.array(ramp)[np.argmin(((c[:, None, :] - cand[None]) ** 2).sum(-1), 1)]
    sh = shadow.reshape(hh, ss, ww, ss).mean((1, 3)) >= 0.5
    return out, sh


# ------------------------------------------------------------------ SHP writing (compression 3)
def encode_rle_zero(a):
    """TS/RA2 SHP compression 3: per line a u16 length, then bytes with 0 runs written as 0,count."""
    out = bytearray()
    for row in a:
        line = bytearray()
        i, n = 0, len(row)
        while i < n:
            if row[i] == 0:
                j = i
                while j < n and row[j] == 0 and j - i < 255:
                    j += 1
                line += bytes((0, j - i))
                i = j
            else:
                line.append(int(row[i]))
                i += 1
        out += struct.pack("<H", len(line) + 2) + line
    return bytes(out)


def write_shp(path, frames, pal, w=W, h=H):
    """frames: list of full-canvas (h, w) uint8 index images."""
    headers, body = [], bytearray()
    base = 8 + 24 * len(frames)
    for f in frames:
        ys, xs = np.nonzero(f)
        if ys.size == 0:
            headers.append(struct.pack("<4HI4BII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
            continue
        x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
        crop = f[y0:y1, x0:x1]
        nz = crop[crop > 1]
        rc = pal[nz].mean(0).astype(int) if nz.size else (0, 0, 0)
        data = encode_rle_zero(crop)
        headers.append(struct.pack("<4HI4BII", x0, y0, x1 - x0, y1 - y0, 3, int(rc[0]), int(rc[1]), int(rc[2]), 0, 0,
                                   base + len(body)))
        body += data
        while len(body) % 8:
            body.append(0)
    open(path, "wb").write(struct.pack("<4H", 0, w, h, len(frames)) + b"".join(headers) + bytes(body))


def check_shp(path, frames):
    """Round-trip: decode with render.read_shp and compare to what we meant to write."""
    ww, hh, dec = R.read_shp(path)
    assert (ww, hh) == (frames[0].shape[1], frames[0].shape[0]) and len(dec) == len(frames), path
    for f, (x, y, w, h, a) in zip(frames, dec):
        full = np.zeros_like(f)
        if a is not None:
            full[y:y + h, x:x + w] = a
        assert (full == f).all(), path


def write_hva(path, section="DUMMY01"):
    ident = struct.pack("<12f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
    open(path, "wb").write(b"CHDFTUR".ljust(16, b"\0") + struct.pack("<II", 1, 1) +
                           section.encode().ljust(16, b"\0") + ident)


# ------------------------------------------------------------------ products
def extract(name_path):
    import mixextract
    archive, inner = name_path
    with open(os.path.join(GAME, archive), "rb") as f:
        return mixextract.extract(f.read(), inner)


def load_sources(tmp):
    src = {"unittem.pal": ("ra2.mix", "cache.mix/unittem.pal"), "cameo.pal": ("ra2.mix", "cache.mix/cameo.pal"),
           "gtgcantur.vxl": ("ra2.mix", "local.mix/gtgcantur.vxl"),
           "gcanicon.shp": ("language.mix", "cameo.mix/gcanicon.shp"),
           "gagcan.shp": ("ra2.mix", "snow.mix/gagcan.shp"),
           "voxels.vpl": ("ra2md.mix", "localmd.mix/voxels.vpl")}
    for k, v in src.items():
        open(os.path.join(tmp, k), "wb").write(extract(v))


def load_vpl(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 8)[0]
    return np.frombuffer(d, np.uint8, n * 256, 16 + 768).reshape(n, 256)


def frame_from(gb, pal, glow=1.0, damage=0, rng=None):
    rgb, mat = shade(gb, pal, glow)
    if damage:
        # scorch marks: blotchy darkening, broken lamps go dark teal
        x, y, z = gb["x"], gb["y"], gb["z"]
        noise = (np.sin(x * 0.31 + 1.7) * np.sin(y * 0.27 + 0.4) + np.sin((x + y) * 0.13 + z * 0.4)
                 + 0.6 * np.sin(x * 0.9 - y * 0.7))
        burn = gb["hit"] & (noise > (1.25 - 0.45 * damage))
        rgb[burn] *= 0.35
        mat[burn & (mat != GLOW)] = DARK
        dead = (mat == GLOW) & (np.sin(x * 0.5 + y * 0.8) > (0.4 - 0.5 * damage))
        rgb[dead] = [30, 30, 60]
        rgb[gb["hit"]] *= 0.9
    return quantize(rgb, mat, gb["hit"], gb["shadow"], gb["shape"], gb["ss"], pal)


def base_frames(pal):
    scene = make_scene({"base": True, "turret": None})
    gb = gbuffer(scene, W, H, CX, CY)
    frames, shadows = [], []
    for dmg in (0, 1, 2):
        img, sh = frame_from(gb, pal, glow=0.55 if dmg == 0 else 0.35, damage=dmg)
        frames.append(img)
        shadows.append(np.where(sh, 1, 0).astype(np.uint8))
    return frames + shadows, gb


def buildup_frames(pal, sec, vpl, normal_table):
    frames, shadows = [], []
    for f in range(12):
        t = f / 11
        bc = 0.8 + (TURRET_Z0 + 0.5) * min(1, t / 0.4)
        pc = 3.2 + 37.5 * np.clip((t - 0.2) / 0.4, 0, 1)
        tc = TURRET_Z0 + 0.3 + 50 * np.clip((t - 0.42) / 0.5, 0, 1)
        parts = {"base": True, "base_clip": bc if bc < TURRET_Z0 else None, "pylon_clip": pc if pc < 40.5 else None,
                 "turret": None, "turret_clip": tc if tc < TURRET_Z0 + 50 else None}
        shadow_parts = dict(parts, turret=-90.0 if t > 0.42 else None)
        gb = gbuffer(make_scene(parts), W, H, CX, CY, ss=2, shadow_scene=make_scene(shadow_parts))
        glow = 0.0 if f < 9 else (0.5 if f < 11 else 1.0)
        rgb, mat = shade(gb, pal, glow)
        # welding sparks along the growing edge
        z = gb["z"]
        for clip in (parts["base_clip"], parts["pylon_clip"], parts["turret_clip"]):
            if clip is not None:
                s = gb["hit"] & (z > clip - 0.7) & (np.sin(gb["x"] * 1.7 + gb["y"] * 2.3 + f) > 0.2)
                mat[s], rgb[s] = HOT, [255, 230, 120]
        img, sh = quantize(rgb, mat, gb["hit"], gb["shadow"], gb["shape"], gb["ss"], pal)
        if t > 0.42:
            # the real voxel, drawn the way the engine will draw it, so the hand-over doesn't pop
            timg, tdep = vpl_draw(sec, -90.0, vpl, normal_table, clip=parts["turret_clip"], hot=True)
            hh, ww = gb["shape"]
            ss = gb["ss"]
            bx, by, bz = (a.reshape(hh, ww)[ss // 2::ss, ss // 2::ss] for a in (gb["x"], gb["y"], gb["z"]))
            bdep = np.where(img > 0, (bx + by) / R2 * S30 + bz * C30, -1e9)
            front = (timg > 0) & (tdep > bdep - 0.5)
            img = np.where(front, timg, img)
        frames.append(img)
        shadows.append(np.where(sh, 1, 0).astype(np.uint8))
    return frames + shadows


def anim_frames(pal, n=16):
    """Glow overlay: a bright wave running round the deck ring, flickering crystals, crawling arcs.
    Only pixels of the ring/crystals that are visible (turret collar included as occluder) are drawn."""
    scene = make_scene({"base": True, "turret": 0.0, "collar_only": True})
    gb = gbuffer(scene, W, H, CX, CY, ss=2)
    ss = gb["ss"]
    x, y, z, mat = gb["x"], gb["y"], gb["z"], gb["mat"]
    ring = gb["hit"] & (mat == GLOW) & (z < RING_Z + 2)
    crystal = gb["hit"] & (mat == GLOW) & (z >= RING_Z + 2)
    ang = np.arctan2(y, x)
    rng = np.random.default_rng(5)
    frames = []
    for f in range(n):
        phase = 2 * math.pi * f / n
        wave = np.cos(ang - phase)
        lvl = np.zeros(x.shape)
        lvl[ring] = 0.55 + 0.45 * np.clip(wave[ring], 0, 1) ** 3 + 0.25 * np.clip(np.cos(2 * (ang[ring] + phase)), 0, 1)
        for k, (px_, py_) in enumerate(PYLONS):
            sel = crystal & ((x - px_) ** 2 + (y - py_) ** 2 < 16)
            lvl[sel] = 0.6 + 0.4 * abs(math.sin(phase * 2 + k * 1.3))
        sub = ring | crystal
        gb2 = dict(gb, mat=np.where(sub, GLOW, 0).astype(np.int8), hit=sub)
        rgb, m2 = shade(gb2, pal, 1.0)
        rgb[sub] *= np.clip(lvl[sub], 0, 1.2)[:, None]
        img, _ = quantize(rgb, m2, sub, np.zeros_like(sub), gb["shape"], ss, pal, cover=0.25)
        img[img > 0] = np.where(np.isin(img[img > 0], [198, 197, 196]), 0, img[img > 0])  # drop the darkest
        # electric arcs from three visible crystals down to the ring (the rear one is behind the turret)
        for k, (px_, py_) in enumerate(PYLONS):
            if px_ < 0 and py_ < 0:
                continue
            if rng.random() < 0.45:
                continue
            a = math.atan2(py_, px_) + rng.uniform(-0.5, 0.5)
            p0 = np.array([px_, py_, CRYSTAL_Z])
            p1 = np.array([RING_R * math.cos(a), RING_R * math.sin(a), RING_Z])
            draw_arc(img, p0, p1, rng)
        frames.append(img)
    return frames


def to_screen(p):
    x, y, z = p
    return CX + (x - y) / R2, CY + (x + y) / (2 * R2) - z * C30


def draw_arc(img, p0, p1, rng, segs=7):
    a, b = np.array(to_screen(p0)), np.array(to_screen(p1))
    pts = [a]
    for i in range(1, segs):
        t = i / segs
        q = a + (b - a) * t
        perp = np.array([-(b - a)[1], (b - a)[0]]); perp /= np.linalg.norm(perp) + 1e-9
        pts.append(q + perp * rng.uniform(-2.5, 2.5))
    pts.append(b)
    for p, q in zip(pts, pts[1:]):
        n = int(max(abs(q - p)) * 2) + 1
        for t in np.linspace(0, 1, n):
            xx, yy = (p + (q - p) * t).round().astype(int)
            if 0 <= xx < W and 0 <= yy < H:
                img[yy, xx] = 15
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    if 0 <= xx + dx < W and 0 <= yy + dy < H and img[yy + dy, xx + dx] != 15:
                        img[yy + dy, xx + dx] = 192


def box_fraction(solid, r):
    """Fraction of solid voxels in the (2r+1)^3 box around each voxel (summed-area table)."""
    a = np.pad(solid.astype(np.int32), r)
    c = a.cumsum(0).cumsum(1).cumsum(2)
    c = np.pad(c, ((1, 0), (1, 0), (1, 0)))
    k = 2 * r + 1
    X, Y, Z = solid.shape
    s = (c[k:k + X, k:k + Y, k:k + Z] - c[:X, k:k + Y, k:k + Z] - c[k:k + X, :Y, k:k + Z] - c[k:k + X, k:k + Y, :Z]
         + c[:X, :Y, k:k + Z] + c[:X, k:k + Y, :Z] + c[k:k + X, :Y, :Z] - c[:X, :Y, :Z])
    return s / k ** 3


def sky_visibility(grid, pts, steps=14):
    """Fraction of upper-hemisphere directions in which each surface voxel sees open sky.
    Rotation-invariant about z, so it stays right however the turret turns."""
    dirs = []
    for elev in (20, 45, 72):
        n_az = {20: 10, 45: 8, 72: 4}[elev]
        for k in range(n_az):
            a = 2 * math.pi * (k + 0.5 * (elev == 45)) / n_az
            e = math.radians(elev)
            dirs.append((math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)))
    dirs.append((0, 0, 1))
    shape = np.array(grid.shape)
    vis = np.zeros(len(pts))
    for d in dirs:
        free = np.ones(len(pts), bool)
        for k in range(2, steps):
            q = np.floor(pts + np.array(d) * k).astype(int)
            inside = np.all((q >= 0) & (q < shape), 1)
            hit = np.zeros(len(pts), bool)
            hit[inside] = grid[q[inside, 0], q[inside, 1], q[inside, 2]]
            free &= ~hit
        vis += free
    return vis / len(dirs)


def turret_voxel(normal_table):
    """Voxelise the turret SDF (1 unit per voxel) into a VXL section with baked shading:
    crevices darker, convex edges lighter, a little noise - the engine's own voxel light is weak."""
    x0, x1, y0, y1, z0, z1 = -25, 50, -21, 21, TURRET_Z0, TURRET_Z0 + 55
    shape = (x1 - x0, y1 - y0, int(z1 - z0))
    idx = np.indices(shape).reshape(3, -1).astype(float)
    X, Y, Z = idx[0] + 0.5 + x0, idx[1] + 0.5 + y0, idx[2] + 0.5 + z0
    acc = Acc(X.size)
    turret_sdf(X, Y, Z, acc)
    solid = acc.d <= 0.3
    sc = lambda a, b, c: (lambda acc2: (turret_sdf(a, b, c, acc2), acc2.d)[1])(Acc(a.size))
    xs, ys, zs = X[solid], Y[solid], Z[solid]
    e = 0.3
    n = np.stack([sc(xs + e, ys, zs) - sc(xs - e, ys, zs), sc(xs, ys + e, zs) - sc(xs, ys - e, zs),
                  sc(xs, ys, zs + e) - sc(xs, ys, zs - e)], 1)
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    mats = acc.m[solid].copy()
    mats[(mats == REMAP_SIDES) & (n[:, 2] > 0.72)] = STEEL
    mats[mats == REMAP_SIDES] = REMAP
    grid = solid.reshape(shape)
    near = box_fraction(grid, 1).reshape(-1)[solid]      # < 0.45: convex edge
    wide = box_fraction(grid, 3).reshape(-1)[solid]      # > 0.55: tucked into a crevice
    h = np.sin(xs * 12.9898 + ys * 78.233 + zs * 37.719) * 43758.5453
    noise = np.round(((h - np.floor(h)) - 0.5) * 1.2)
    # Painted-in light. The engine's voxel light is weak and even, so the form comes from here:
    # open sky -> lighter, overhangs/lower sides -> darker, tops vs undersides, bright convex top edges,
    # dark concave seams. All of it is symmetric about z, so it holds at every turret facing.
    ii0 = idx[:, solid].T
    sky = sky_visibility(grid, ii0 + 0.5)
    nz = n[:, 2]
    # Stock turrets use flat dark colours and let the engine light them. Here the engine lights tops
    # dimly and undersides relatively brightly, so tops get lighter indices and undersides darker ones.
    # Sky visibility and edges add a little form. All terms are symmetric about z.
    shift = (np.round((0.55 - sky) * 5)
             - np.round(np.clip(nz, 0, 1) * 5) + np.round(np.clip(-nz, 0, 1) * 3)
             + np.clip(np.round((wide - 0.5) * 6), 0, 2)
             - ((near < 0.45) & (nz > 0.25)) * 2
             + noise)
    colors = np.zeros(mats.size, np.uint8)
    for m_, (ramp, base) in VOXEL_RAMP.items():
        sel = mats == m_
        pos = np.clip(ramp.index(base) + shift[sel], 0, len(ramp) - 1).astype(int)
        colors[sel] = np.array(ramp)[pos]
    g = mats == GLOW
    colors[g] = GLOW_VOXEL
    colors[g & (n @ np.array([-0.4, 0.6, 0.7]) > 0.55)] = GLOW_VOXEL_HOT
    colors[g & (n[:, 2] < -0.35)] = GLOW_VOXEL_RIM
    sec = vxl.Section("DUMMY01", *shape)
    ii = idx[:, solid].astype(int)
    sec.solid[ii[0], ii[1], ii[2]] = True
    sec.color[ii[0], ii[1], ii[2]] = colors
    sec.normal[ii[0], ii[1], ii[2]] = np.argmax(n @ normal_table.T, 1)
    sec.bounds = [x0, y0, z0, x1, y1, z1]
    return sec


def vpl_draw(sec, facing, vpl, normal_table, clip=None, hot=False):
    """Emulate the game drawing the turret voxel: world placement at 1 px per voxel on the building's
    canvas, light level from VPL_LIGHT, colour through voxels.vpl. Returns (index image, depth)."""
    xs, ys, zs = np.nonzero(sec.solid)
    b = sec.bounds
    p = np.stack([b[0] + xs + .5, b[1] + ys + .5, b[2] + zs + .5], 1)
    col = sec.color[xs, ys, zs].copy()
    nrm = normal_table[sec.normal[xs, ys, zs]]
    if clip is not None:
        keep = p[:, 2] <= clip
        top = keep & (p[:, 2] > clip - 1.0)
        p, col, nrm, top = p[keep], col[keep], nrm[keep], top[keep]
    t = math.radians(facing)
    rot = np.array([[math.cos(t), -math.sin(t), 0], [math.sin(t), math.cos(t), 0], [0, 0, 1]])
    p, nrm = p @ rot.T, nrm @ rot.T
    level = np.clip(np.round(VPL_LIGHT[0] + nrm @ np.array(VPL_LIGHT[1:])), 0, vpl.shape[0] - 1).astype(int)
    out_col = vpl[level, col]
    if clip is not None and hot:
        out_col = np.where(top & (np.sin(p[:, 0] * 1.7 + p[:, 1] * 2.3) > 0), 178, out_col)
    sx = CX + (p[:, 0] - p[:, 1]) / R2
    sy = CY + (p[:, 0] + p[:, 1]) / (2 * R2) - p[:, 2] * C30
    depth = (p[:, 0] + p[:, 1]) / R2 * S30 + p[:, 2] * C30
    img = np.zeros((H, W), np.uint8)
    dep = np.full((H, W), -1e9)
    order = np.argsort(depth)
    for dx in (0, 1):
        for dy in (0, 1):
            X_ = np.floor(sx[order]).astype(int) + dx
            Y_ = np.floor(sy[order]).astype(int) + dy
            ok = (X_ >= 0) & (X_ < W) & (Y_ >= 0) & (Y_ < H)
            img[Y_[ok], X_[ok]] = out_col[order][ok]
            dep[Y_[ok], X_[ok]] = depth[order][ok]
    return img, dep


# ---------------------------------------------------------------- cameo
def make_cameo(src, out, pal_unit):
    from PIL import Image, ImageFilter, ImageEnhance
    from make_graphics import FONT
    cw, chh, K = 60, 48, 4
    upal = R.remap_colors(pal_unit, (70, 110, 235))
    gb = gbuffer(make_scene({"base": True, "turret": 75.0}), cw * K // 2, chh * K // 2, cw * K / 4, 56,
                 scale=0.86, ss=2)
    rgb, mat = shade(gb, upal)
    sh, sw = gb["shape"]
    img = np.zeros((sh, sw, 4))
    img[..., :3] = rgb.reshape(sh, sw, 3)
    img[..., 3] = gb["hit"].reshape(sh, sw) * 255
    obj = Image.fromarray(img.astype(np.uint8), "RGBA")
    obj = ImageEnhance.Brightness(obj).enhance(1.08)
    # night sky with a scanning grid: the building sees everything
    yy = np.linspace(0, 1, chh * K)[:, None, None]
    xx = np.linspace(0, 1, cw * K)[None, :, None]
    bg = np.array([8, 18, 38]) * (1 - yy) + np.array([30, 70, 95]) * yy + np.zeros((1, cw * K, 1))
    grid = ((np.abs(((xx - 0.5) / (yy + 0.35)) * 6 % 1 - 0.5) < 0.04) | (np.abs((1 / (yy + 0.35)) * 3 % 1 - 0.5) < 0.05))
    bg = np.where(grid & (yy > 0.45), bg + np.array([0, 60, 70]), bg)
    back = Image.fromarray(np.clip(bg, 0, 255).astype(np.uint8), "RGB").filter(ImageFilter.GaussianBlur(1.2)).convert(
        "RGBA")
    a = np.asarray(obj).astype(np.float32)
    glowmask = ((a[..., 2] > 200) & (a[..., 2] > a[..., 0] + 40) & (a[..., 3] > 0)).astype(np.uint8) * 255
    halo = Image.fromarray(glowmask, "L").filter(ImageFilter.GaussianBlur(7))
    glow = Image.new("RGBA", obj.size, (140, 170, 255, 0))
    glow.putalpha(halo.point(lambda v: min(255, v * 3)))
    shadow = Image.new("RGBA", obj.size, (0, 0, 0, 0))
    shadow.putalpha(obj.getchannel("A").filter(ImageFilter.GaussianBlur(5)).point(lambda v: v * 0.55))
    back.alpha_composite(shadow, (8, 4))
    back.alpha_composite(glow, (0, -2))
    back.alpha_composite(obj, (0, -2))
    rgbc = np.asarray(back.convert("RGB").resize((cw, chh), Image.LANCZOS)).astype(np.float32)
    label = "CHEAT DEFENSE"
    rgbc[40:48] *= 0.35
    text_w = sum(len(FONT[ch][0]) + 1 for ch in label) - 1
    x = (cw - text_w) // 2
    for ch in label:
        g = FONT[ch]
        for gy in range(5):
            for gx, p in enumerate(g[gy]):
                if p == "#":
                    rgbc[42 + gy, x + gx] = [255, 255, 255] if gy < 3 else [185, 185, 190]
        x += len(g[0]) + 1
    cpal = R.load_pal(os.path.join(src, "cameo.pal"))
    p = cpal[1:].astype(np.float32)
    flat = rgbc.reshape(-1, 3)
    idx = (np.argmin(((flat[:, None, :] - p[None]) ** 2).sum(2), axis=1) + 1).reshape(chh, cw)
    _, _, frames = R.read_shp(os.path.join(src, "gcanicon.shp"))
    stock = frames[0][4]
    ring = np.ones((chh, cw), bool)
    ring[2:-2, 2:-2] = False
    ring[40:47, 2:-2] = False
    idx[ring] = stock[ring]
    R.write_shp(out, idx, os.path.join(src, "gcanicon.shp"))
    return cpal


# ---------------------------------------------------------------- previews
def previews(out_dir, pal, base, buildup, anim, sec, normal_table, src, cpal, cameo_path, vpl):
    from PIL import Image
    ppal = R.remap_colors(pal, (70, 110, 235))
    os.makedirs(out_dir, exist_ok=True)

    def rgba(ix, p=ppal, shadow=None):
        a = np.zeros(ix.shape + (4,), np.uint8)
        if shadow is not None:
            a[shadow == 1] = (0, 0, 0, 110)
        a[ix > 1, :3] = p[ix[ix > 1]]
        a[ix > 1, 3] = 255
        return Image.fromarray(a, "RGBA")

    def ground(w, h):
        rng = np.random.default_rng(1)
        g = np.array([96, 112, 70]) + rng.normal(0, 7, (h, w, 3))
        return Image.fromarray(np.clip(g, 0, 255).astype(np.uint8), "RGB").convert("RGBA")

    def turret_img(facing):
        # drawn through voxels.vpl like the game does (already on the building canvas)
        return rgba(vpl_draw(sec, facing, vpl, normal_table)[0])

    # composite: base + turret voxel (render.py's preview renderer, 1 px/voxel) + glow anim frame
    tiles = []
    for fi, facing in ((0, -90), (0, 0), (5, 135), (10, 225)):
        g = ground(W, H)
        g.alpha_composite(rgba(base[0], shadow=base[3]))
        timg = turret_img(facing)
        # render.py puts the world origin at the image centre; move it onto the foundation centre
        g.alpha_composite(timg)
        g.alpha_composite(rgba(anim[fi]))
        tiles.append(g)
    sheet = Image.new("RGBA", (W * 4, H), (0, 0, 0, 255))
    for i, t in enumerate(tiles):
        sheet.paste(t, (W * i, 0))
    sheet.resize((W * 8, H * 2), Image.NEAREST).save(os.path.join(out_dir, "cheatdef-building.png"))

    # stock Grand Cannon next to it at 1:1 for scale
    _, _, fr = R.read_shp(os.path.join(src, "gagcan.shp"))
    g = ground(W * 2, H)
    stock = np.zeros((H, W), np.uint8)
    x, y, w, h, a = fr[0]
    stock[y:y + h, x:x + w] = a
    g.alpha_composite(rgba(stock), (0, 0))
    g.alpha_composite(rgba(base[0], shadow=base[3]), (W, 0))
    g.alpha_composite(turret_img(-90), (W, 0))
    g.resize((W * 4, H * 2), Image.NEAREST).save(os.path.join(out_dir, "cheatdef-scale-vs-stock-plate.png"))

    for name, frames, cols in (("buildup", buildup[:12], 6), ("damage", base[:3], 3), ("anim", anim, 8)):
        rows = (len(frames) + cols - 1) // cols
        s = Image.new("RGBA", (W * cols, H * rows), (40, 40, 40, 255))
        for i, f in enumerate(frames):
            gnd = ground(W, H)
            if name == "anim":
                gnd.alpha_composite(rgba(base[0]))
            elif name == "buildup":
                gnd.alpha_composite(rgba(np.zeros_like(f), shadow=buildup[12 + i]))
            gnd.alpha_composite(rgba(f))
            s.paste(gnd, ((i % cols) * W, (i // cols) * H))
        s.save(os.path.join(out_dir, f"cheatdef-{name}.png"))
    cam = R.shp_to_image(cameo_path, cpal)
    cam.resize((240, 192), Image.NEAREST).save(os.path.join(out_dir, "cheatdef-cameo.png"))


def main():
    import tempfile
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "assets")
    os.makedirs(out, exist_ok=True)
    normal_table = vxl.load_normal_table(os.path.join(FA2, "voxel_normal_tables.bin"), 4)
    with tempfile.TemporaryDirectory() as src:
        load_sources(src)
        pal = R.load_pal(os.path.join(src, "unittem.pal"))
        print("base ...", flush=True)
        base, _ = base_frames(pal)
        write_shp(os.path.join(out, BASE_SHP), base, pal)
        check_shp(os.path.join(out, BASE_SHP), base)
        vpl = load_vpl(os.path.join(src, "voxels.vpl"))
        print("turret voxel ...", flush=True)
        sec = turret_voxel(normal_table)
        v = vxl.Vxl.load(os.path.join(src, "gtgcantur.vxl"))   # keeps the stock header palette
        v.sections = [sec]
        v.save(os.path.join(out, TURRET + ".vxl"))
        back = vxl.Vxl.load(os.path.join(out, TURRET + ".vxl")).sections[0]
        assert (back.solid == sec.solid).all() and (back.color[back.solid] == sec.color[sec.solid]).all()
        write_hva(os.path.join(out, TURRET + ".hva"))
        print("build-up ...", flush=True)
        buildup = buildup_frames(pal, sec, vpl, normal_table)
        for name in BUILDUP_SHPS:
            write_shp(os.path.join(out, name), buildup, pal)
            check_shp(os.path.join(out, name), buildup)
        print("active anim ...", flush=True)
        anim = anim_frames(pal)
        write_shp(os.path.join(out, ANIM_SHP), anim, pal)
        check_shp(os.path.join(out, ANIM_SHP), anim)
        print("cameo ...", flush=True)
        cpal = make_cameo(src, os.path.join(out, CAMEO), pal)
        previews(os.path.join(HERE, "previews"), pal, base, buildup, anim, sec, normal_table, src, cpal,
                 os.path.join(out, CAMEO), vpl)
    print("wrote", ", ".join(FILES), "to", out)


if __name__ == "__main__":
    main()
