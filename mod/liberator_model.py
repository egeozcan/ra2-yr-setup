"""Procedural voxel model of the Liberator: a from-scratch American Tesla tank.

World units = voxels (step 1.0), x forward, y left/right, z up; the unit's pivot is x = y = 0.
Hull: low angular body with sloped glacis, house-colour armour skirts, tracks with road wheels,
      engine grille, white fender stars, headlights.
Turret: angular faceted turret (house-colour sides, steel roof), a copper-wound Tesla cannon
        with a glowing electrode ball, twin capacitor banks, and a rear Tesla mast with an orb.
"""
import math
import numpy as np
import vxl

# unittem.pal indices (identical across all six theatre unit palettes)
STEEL, STEEL_DARK, STEEL_SEAM = 86, 89, 92
REMAP, REMAP_DARK = 20, 25
TRACK, TRACK_DARK, WHEEL, HUB = 57, 61, 50, 44
GRILLE, GRILLE_DARK = 55, 60
METAL, METAL_DARK = 44, 48
COPPER, COPPER_DARK = 186, 188
GLOW_HOT, GLOW, GLOW_DARK = 15, 9, 2
WHITE, LAMP = 33, 177


class Grid:
    """A section under construction: world bounds with unit voxels and coordinate arrays."""

    def __init__(self, x0, x1, y0, y1, z0, z1):
        self.lo = np.array([x0, y0, z0], float)
        self.shape = (int(round(x1 - x0)), int(round(y1 - y0)), int(round(z1 - z0)))
        idx = np.indices(self.shape).astype(float)
        self.X, self.Y, self.Z = (idx[i] + 0.5 + self.lo[i] for i in range(3))
        self.solid = np.zeros(self.shape, bool)
        self.color = np.zeros(self.shape, np.uint8)

    def add(self, mask, color):
        self.solid |= mask
        self.color[mask] = color

    def paint(self, mask, color):
        m = mask & self.solid
        self.color[m] = color

    def cut(self, mask):
        self.solid &= ~mask

    # ---- primitives (return boolean masks) ----
    def box(self, x0, x1, y0, y1, z0, z1):
        X, Y, Z = self.X, self.Y, self.Z
        return (X >= x0) & (X <= x1) & (Y >= y0) & (Y <= y1) & (Z >= z0) & (Z <= z1)

    def cyl_x(self, x0, x1, cy, cz, r):
        return (self.X >= x0) & (self.X <= x1) & ((self.Y - cy) ** 2 + (self.Z - cz) ** 2 <= r * r)

    def cyl_y(self, y0, y1, cx, cz, r):
        return (self.Y >= y0) & (self.Y <= y1) & ((self.X - cx) ** 2 + (self.Z - cz) ** 2 <= r * r)

    def cyl_z(self, z0, z1, cx, cy, r):
        return (self.Z >= z0) & (self.Z <= z1) & ((self.X - cx) ** 2 + (self.Y - cy) ** 2 <= r * r)

    def sphere(self, cx, cy, cz, r):
        return (self.X - cx) ** 2 + (self.Y - cy) ** 2 + (self.Z - cz) ** 2 <= r * r

    def prism(self, poly, z0, z1, taper=0.0):
        """Convex polygon (counter-clockwise xy points) extruded z0..z1, shrinking by `taper` per unit z."""
        inside = (self.Z >= z0) & (self.Z <= z1)
        inset = (self.Z - z0) * taper
        for (ax, ay), (bx, by) in zip(poly, poly[1:] + poly[:1]):
            ex, ey = bx - ax, by - ay
            ln = math.hypot(ex, ey)
            # signed distance to the edge line, positive inside for CCW polygons
            d = (ex * (self.Y - ay) - ey * (self.X - ax)) / ln
            inside &= d >= inset
        return inside

    def surface(self):
        """Solid voxels with at least one empty 6-neighbour."""
        s = np.pad(self.solid, 1)
        inner = s[2:, 1:-1, 1:-1] & s[:-2, 1:-1, 1:-1] & s[1:-1, 2:, 1:-1] & s[1:-1, :-2, 1:-1] \
            & s[1:-1, 1:-1, 2:] & s[1:-1, 1:-1, :-2]
        return self.solid & ~inner

    def to_section(self, normals):
        sec = vxl.Section("DUMMY01", *self.shape)
        sec.solid, sec.color = self.solid.copy(), self.color.copy()
        sec.bounds = list(self.lo) + list(self.lo + np.array(self.shape, float))
        sec.normal = smooth_normals(self.solid, normals)
        return sec


def _blur(a, sigma):
    r = int(math.ceil(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    for axis in range(3):
        a = np.apply_along_axis(lambda v: np.convolve(v, k, mode="same"), axis, a)
    return a


def smooth_normals(solid, table, sigma=1.1):
    """Normals from the gradient of blurred occupancy, snapped to the nearest table entry."""
    pad = 4
    occ = _blur(np.pad(solid.astype(float), pad), sigma)
    gx, gy, gz = np.gradient(occ)
    sl = (slice(pad, -pad),) * 3
    n = -np.stack([gx[sl], gy[sl], gz[sl]], -1)
    ln = np.linalg.norm(n, axis=-1, keepdims=True)
    n = np.where(ln > 1e-6, n / np.maximum(ln, 1e-6), np.array([0, 0, 1.0]))
    return np.argmax(n.reshape(-1, 3) @ table.T, axis=1).reshape(solid.shape).astype(np.uint8)


STAR5 = np.array([[c == "X" for c in row] for row in (
    "..X..",
    "XXXXX",
    ".XXX.",
    ".X.X.",
    "X...X",
)])[::-1].T  # rows run along -x (top row points forward), columns along y

HULL_TOP = 9.5


def build_hull():
    g = Grid(-24, 24, -15.5, 15.5, 0, 12)
    X, Y, Z = g.X, g.Y, g.Z
    ay = np.abs(Y)

    # tracks: capsule profile in x/z, |y| 9.5..14.5
    seg_x = np.clip(X, -18.5, 18.5)
    track = (ay >= 9.5) & (ay <= 14.5) & ((X - seg_x) ** 2 + (Z - 3.2) ** 2 <= 3.2 ** 2)
    g.add(track, TRACK)
    g.paint(track & (np.floor(X) % 3 == 0), TRACK_DARK)                       # tread bars
    for wx in (-15, -9, -3, 3, 9, 15):                                        # road wheels
        g.add(g.cyl_y(-15.5, -13.5, wx, 2.6, 2.1) | g.cyl_y(13.5, 15.5, wx, 2.6, 2.1), WHEEL)
        g.paint(g.cyl_y(-15.5, -14.5, wx, 2.6, 0.9) | g.cyl_y(14.5, 15.5, wx, 2.6, 0.9), HUB)

    # lower hull between the tracks, with a sloped nose
    g.add(g.box(-22, 21, -9.5, 9.5, 1.5, 7) & (Z >= 1.5 + np.maximum(0, X - 16) * 1.1), STEEL_DARK)

    # upper hull / fenders: sloped glacis at the front, short slope at the rear
    upper = g.box(-22.5, 23, -15.5, 15.5, 6.5, HULL_TOP)
    upper &= Z <= HULL_TOP - np.maximum(0, X - 11) * 0.42
    upper &= Z <= HULL_TOP - np.maximum(0, -X - 19) * 1.0
    g.add(upper, STEEL)
    g.paint(upper & (Z > HULL_TOP - 1) & (np.abs(ay - 9.5) < 0.6), STEEL_SEAM)  # fender seams

    # armoured skirts in house colour over the upper tracks
    skirt = g.box(-21, 20, -15.5, 15.5, 3.5, 7) & (ay >= 13.5)
    skirt &= Z >= 3.5 + np.maximum(0, X - 16) * 0.9                          # bevelled front edge
    g.add(skirt, REMAP)
    g.paint(skirt & (Z < 4.5), REMAP_DARK)

    # engine deck grille at the rear: recessed slats
    deck = g.box(-21.5, -12, -7.5, 7.5, HULL_TOP - 1, HULL_TOP)
    g.cut(deck & (np.floor(X) % 2 == 0))
    g.paint(g.box(-21.5, -12, -7.5, 7.5, HULL_TOP - 2, HULL_TOP), GRILLE)
    g.paint(g.box(-21.5, -12, -7.5, 7.5, HULL_TOP - 2, HULL_TOP - 1) & (np.floor(X) % 2 == 0), GRILLE_DARK)

    # headlights on the glacis corners
    for sy in (-1, 1):
        g.paint(g.box(17.5, 23, sy * 12.5 - 1.2, sy * 12.5 + 1.2, 5, 8), LAMP)

    # white stars on both fender tops, clear of the turret
    top = g.surface() & (Z > HULL_TOP - 1)
    for sy in (-1, 1):
        for i in range(5):
            for j in range(5):
                if STAR5[i, j]:
                    cx, cy = -2.5 + (2 - i), sy * 12 + (j - 2)  # voxel centres: x on .5, y on integers
                    g.paint(top & (np.abs(X - cx) < 0.5) & (np.abs(Y - cy) < 0.5), WHITE)
    return g


def build_turret():
    z0 = HULL_TOP
    g = Grid(-15, 21, -12, 12, z0, z0 + 21)
    X, Y, Z = g.X, g.Y, g.Z

    # faceted turret body: CCW outline, sides lean inward
    outline = [(-12, -9), (4, -11), (9, -7.5), (10.5, -4), (10.5, 4), (9, 7.5), (4, 11), (-12, 9)]
    body = g.prism(outline, z0, z0 + 6, taper=0.45)
    g.add(body, REMAP)
    g.paint(body & (Z > z0 + 4.8), STEEL)                                     # steel roof
    g.add(g.box(-14.5, -11, -7.5, 7.5, z0 + 1, z0 + 5), STEEL_DARK)           # rear bustle

    # Tesla cannon: mantlet, barrel, copper windings, electrode ball
    g.add(g.box(8, 11.5, -3.5, 3.5, z0 + 1, z0 + 5), STEEL_DARK)
    cz = z0 + 3.2
    g.add(g.cyl_x(11, 16, 0, cz, 1.6), METAL_DARK)
    for rx in (12.2, 14.6):
        g.add(g.cyl_x(rx - 0.8, rx + 0.8, 0, cz, 2.7), COPPER)
        g.paint(g.cyl_x(rx - 0.8, rx + 0.8, 0, cz, 2.7) & (Z < cz - 1.5), COPPER_DARK)
    ball = g.sphere(17.8, 0, cz, 2.5)
    g.add(ball, GLOW)
    g.paint(ball & (Z > cz + 1), GLOW_HOT)
    g.paint(ball & (Z < cz - 1), GLOW_DARK)

    # capacitor banks on the roof
    for sy in (-1, 1):
        cy, zc = sy * 6.2, z0 + 7.2
        g.add(g.cyl_x(-9.5, 2, cy, zc, 1.7), STEEL_DARK)
        for bx in (-6.5, -1.5):
            g.add(g.cyl_x(bx - 0.7, bx + 0.7, cy, zc, 2.1), COPPER)
        g.add(g.cyl_x(2, 3.3, cy, zc, 1.5), GLOW)

    # Tesla mast with coil rings and orb, at the rear of the roof
    mx = -8.0
    g.add(g.cyl_z(z0 + 5, z0 + 7, mx, 0, 2.6), REMAP)
    g.add(g.cyl_z(z0 + 7, z0 + 15, mx, 0, 1.2), METAL)
    for rz in (z0 + 9.5, z0 + 13):
        g.add(g.cyl_z(rz - 0.6, rz + 0.6, mx, 0, 2.3), COPPER)
    orb_z = z0 + 17.2
    orb = g.sphere(mx, 0, orb_z, 2.6)
    g.add(orb, GLOW)
    g.paint(orb & (Z > orb_z + 1), GLOW_HOT)
    g.paint(orb & (Z < orb_z - 1), GLOW_DARK)

    # commander hatch
    g.add(g.cyl_z(z0 + 5, z0 + 6.5, -2, 0, 1.8), STEEL_DARK)
    return g


def build(normals):
    return build_hull().to_section(normals), build_turret().to_section(normals)
