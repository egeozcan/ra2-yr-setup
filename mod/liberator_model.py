"""Procedural voxel model of the Liberator: a from-scratch Allied Tesla tank.

World units = voxels (step 1.0), x forward, y left/right, z up; the unit's pivot is x = y = 0.
Styled after the stock Allied vehicles (Grizzly, Prism Tank, Tank Destroyer): blue-grey steel,
dark running gear, house colour on trim panels, and a stepped, detailed silhouette. The Tesla gun
is drawn the way the stock Tesla tank draws its coils: stacked grey metal discs with dark gaps.
Hull: stepped deck with a sloped glacis, lower fenders with stowage boxes, house-colour side skirts,
      tracks with road wheels, rear engine grilles and exhausts.
Turret: faceted Allied turret (steel roof, house-colour cheeks), a coil-stack Tesla gun ending in a
        small blue-white electrode, two capacitor coils on the roof, commander cupola and antenna.
"""
import math
import numpy as np
import vxl

# Materials. Each is drawn with the unittem.pal indices that the stock Allied vehicles use for it,
# picked by which way the face points: (top, side, underside). All are identical in the six theatre
# unit palettes. Colours stay flat: the engine's voxel light (voxels.vpl) gives the shading.
STEEL, PLATE, DARK, TRACK, REMAP, COIL, GAP, GLOW, LENS = range(1, 10)
COLORS = {
    STEEL: (88, 92, 94),     # main armour: Prism Tank deck 88, sides 92/94
    PLATE: (90, 93, 94),     # secondary plates, boxes, turret ring
    DARK: (55, 57, 59),      # grilles, exhausts, seams, wheels
    TRACK: (57, 59, 59),
    REMAP: (23, 23, 26),     # house colour, the stock tanks' remap shade
    COIL: (50, 51, 13),      # Tesla tank coil discs
    GAP: (55, 56, 56),       # between coil discs
    GLOW: (192, 192, 193),   # electrode; 192/193 stay blue-white through voxels.vpl (see MODDING-NOTES 6)
    LENS: (50, 50, 13),      # headlights
}
HULL_TOP = 11.0


class Grid:
    """A section under construction: world bounds with unit voxels and coordinate arrays."""

    def __init__(self, x0, x1, y0, y1, z0, z1):
        self.lo = np.array([x0, y0, z0], float)
        self.shape = (int(round(x1 - x0)), int(round(y1 - y0)), int(round(z1 - z0)))
        idx = np.indices(self.shape).astype(float)
        self.X, self.Y, self.Z = (idx[i] + 0.5 + self.lo[i] for i in range(3))
        self.solid = np.zeros(self.shape, bool)
        self.mat = np.zeros(self.shape, np.uint8)

    def add(self, mask, mat):
        self.solid |= mask
        self.mat[mask] = mat

    def paint(self, mask, mat):
        self.mat[mask & self.solid] = mat

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

    def to_section(self, normals):
        n_idx, n_vec = smooth_normals(self.solid, normals)
        color = np.zeros(self.shape, np.uint8)
        nz = n_vec[..., 2]
        orient = np.where(nz > 0.6, 0, np.where(nz < -0.4, 2, 1))   # top, side, underside
        for m, cols in COLORS.items():
            sel = self.mat == m
            color[sel] = np.array(cols, np.uint8)[orient[sel]]
        # Drop voxels that can't be seen (no empty neighbour among all 26), like the stock models.
        s = np.pad(self.solid, 1)
        enclosed = np.ones(self.shape, bool)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    enclosed &= s[1 + dx:s.shape[0] - 1 + dx, 1 + dy:s.shape[1] - 1 + dy, 1 + dz:s.shape[2] - 1 + dz]
        sec = vxl.Section("DUMMY01", *self.shape)
        sec.solid = self.solid & ~enclosed
        sec.color = np.where(sec.solid, color, 0).astype(np.uint8)
        sec.normal = np.where(sec.solid, n_idx, 0).astype(np.uint8)
        sec.bounds = list(self.lo) + list(self.lo + np.array(self.shape, float))
        return sec


def _blur(a, sigma):
    r = int(math.ceil(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    for axis in range(3):
        a = np.apply_along_axis(lambda v: np.convolve(v, k, mode="same"), axis, a)
    return a


def smooth_normals(solid, table, sigma=0.8):
    """Normals from the gradient of blurred occupancy, snapped to the nearest table entry.
    Returns (table index, snapped vector) per voxel. A small sigma keeps flat faces flat."""
    pad = 4
    occ = _blur(np.pad(solid.astype(float), pad), sigma)
    gx, gy, gz = np.gradient(occ)
    sl = (slice(pad, -pad),) * 3
    n = -np.stack([gx[sl], gy[sl], gz[sl]], -1)
    ln = np.linalg.norm(n, axis=-1, keepdims=True)
    n = np.where(ln > 1e-6, n / np.maximum(ln, 1e-6), np.array([0, 0, 1.0]))
    idx = np.argmax(n.reshape(-1, 3) @ table.T, axis=1).reshape(solid.shape)
    return idx.astype(np.uint8), table[idx]


def build_hull():
    g = Grid(-24, 24, -15.5, 15.5, 0, 12)
    X, Y, Z = g.X, g.Y, g.Z
    ay = np.abs(Y)

    # tracks: capsule profile in x/z, |y| 10..14, with tread bars
    seg_x = np.clip(X, -19.5, 19.5)
    track = (ay >= 9.5) & (ay <= 14.5) & ((X - seg_x) ** 2 + (Z - 3.5) ** 2 <= 3.5 ** 2)
    g.add(track, TRACK)
    g.paint(track & (np.floor(X) % 3 == 0), DARK)
    for wx in (-16, -9.5, -3, 3.5, 10):                                       # road wheels
        g.add(g.cyl_y(-15, -14, wx, 3.0, 2.2) | g.cyl_y(14, 15, wx, 3.0, 2.2), DARK)
    for sx in (-19.5, 19.5):                                                  # sprocket and idler
        g.add(g.cyl_y(-15, -14, sx, 3.5, 2.6) | g.cyl_y(14, 15, sx, 3.5, 2.6), PLATE)

    # lower hull between the tracks, sloped at both ends
    lower = g.box(-21.5, 22, -9.5, 9.5, 1.5, 7) & (Z >= 1.5 + np.maximum(0, X - 17) * 1.1)
    lower &= Z >= 1.5 + np.maximum(0, -X - 18) * 1.2
    g.add(lower, PLATE)

    # centre deck: sloped glacis at the front, short slope at the rear
    deck = g.box(-22, 23, -12, 12, 6.5, HULL_TOP)
    deck &= Z <= HULL_TOP - np.maximum(0, X - 12) * 0.42
    deck &= Z <= HULL_TOP - np.maximum(0, -X - 19.5) * 1.3
    g.add(deck, STEEL)
    g.paint(deck & (np.abs(X - 12.5) < 0.5) & (Z > HULL_TOP - 1), DARK)       # glacis seam

    # fenders one step below the deck, over the tracks
    fender = g.box(-22.5, 22.5, -15.5, 15.5, 6.5, 8.5) & (ay >= 11.5)
    fender &= Z <= 8.5 - np.maximum(0, X - 18) * 0.6
    g.add(fender, STEEL)
    g.paint(fender & (X > 6) & (X < 15) & (Z > 8), REMAP)                     # house-colour front fenders

    # house-colour side skirts in three panels, steel bottom edge, bevelled front
    skirt = g.box(-21, 20.5, -15.5, 15.5, 3.5, 8) & (ay >= 14.5)
    skirt &= Z >= 3.5 + np.maximum(0, X - 16.5) * 0.9
    g.add(skirt, REMAP)
    g.paint(skirt & ((np.abs(X + 6.5) < 0.5) | (np.abs(X - 7) < 0.5)), DARK)
    g.paint(skirt & (Z < 4.5), PLATE)

    # stowage boxes on the rear fenders, headlights and tow hooks at the front
    g.add(g.box(-19, -11, -15, 15, 9, 10.5) & (ay >= 12) & (ay <= 14), PLATE)
    g.paint(g.box(-15.5, -15, -15, 15, 9, 10.5) & (ay >= 12), DARK)           # box straps
    g.add(g.box(16, 18, -15, 15, 9, 10) & (ay >= 13) & (ay <= 14), LENS)
    g.add(g.box(19.5, 21.5, -15, 15, 4, 5.5) & (np.abs(ay - 6) < 0.5), DARK)

    # engine deck: two recessed grilles, exhausts at the rear corners
    for sy in (-1, 1):
        grille = g.box(-20.5, -13, sy * 5 - 3.5, sy * 5 + 3.5, HULL_TOP - 1, HULL_TOP)
        g.cut(grille & (np.floor(X) % 2 == 0))
        g.paint(g.box(-20.5, -13, sy * 5 - 3.5, sy * 5 + 3.5, HULL_TOP - 2, HULL_TOP), DARK)
        g.add(g.cyl_x(-24, -21.5, sy * 9.5, 8.5, 1.2), DARK)

    # driver hatch and vents on the glacis, deck seam behind the turret, turret ring
    glacis_top = Z > HULL_TOP - np.maximum(0, X - 12) * 0.42 - 1
    g.paint(g.box(14, 17, -2.5, 2.5, 0, 12) & glacis_top, PLATE)
    g.paint(g.box(14, 19, -9, 9, 0, 12) & (np.abs(Y) >= 5) & (np.floor(X) % 2 == 0) & glacis_top, DARK)
    g.paint(deck & (np.abs(X + 11.5) < 0.5) & (Z > HULL_TOP - 1), DARK)
    g.add(g.cyl_z(HULL_TOP, 12, 0, 0, 8.5), PLATE)
    return g


TURRET_Z0 = 12.0   # the turret ring's top face
# Electrode centre, where the bolt should start: PrimaryFireFLH (build-tesla-mod.py) points here
# at about 6 leptons per voxel.
ELECTRODE = (20.0, 0.0, 16.5)


def build_turret():
    z0 = TURRET_Z0
    g = Grid(-16, 22, -12, 12, z0, z0 + 13)
    X, Y, Z = g.X, g.Y, g.Z
    roof = z0 + 7   # top face of the body; voxel centres sit on .5

    # faceted body: CCW outline, sides lean inward; house-colour cheeks on the front facets
    outline = [(-12, -9.5), (4, -10.5), (10, -6.5), (11.5, -3), (11.5, 3), (10, 6.5), (4, 10.5), (-12, 9.5)]
    body = g.prism(outline, z0, roof, taper=0.35)
    g.add(body, STEEL)
    g.paint(body & (X > 3.5) & (np.abs(Y) > 2.5) & (Z < roof - 1), REMAP)
    g.paint(body & (np.abs(X - 3.5) < 0.5) & (Z < roof - 1), DARK)           # seam behind the cheeks
    g.add(g.box(-15, -11, -7, 7, z0 + 1, z0 + 5.5), PLATE)                    # rear bustle
    g.paint(g.box(-15, -11, -7, 7, z0 + 3, z0 + 3.5), DARK)                  # bustle rack

    # Tesla gun, sized like one of the stock Tesla tank's coils: mantlet, dark core,
    # four grey coil discs, a small electrode tip
    ex, _, ez = ELECTRODE
    g.add(g.box(9.5, 12, -4, 4, z0 + 1, roof - 1), PLATE)
    g.add(g.cyl_x(11, ex - 1, 0, ez, 2.3), GAP)
    for cx in (12.5, 14.5, 16.5, 18.5):
        g.add(g.cyl_x(cx - 0.5, cx + 0.5, 0, ez, 3.3), COIL)
    g.add(g.sphere(ex, 0, ez, 1.4), GLOW)

    # capacitor coils on the roof: stacked discs, dark caps
    for sy in (-1, 1):
        cx, cy = -6.5, sy * 4.5
        g.add(g.cyl_z(roof, roof + 5, cx, cy, 1.0), GAP)
        for rz in (roof + 1.5, roof + 3.5):
            g.add(g.cyl_z(rz - 0.5, rz + 0.5, cx, cy, 1.8), COIL)
        g.add(g.cyl_z(roof + 5, roof + 6, cx, cy, 1.0), DARK)

    # commander cupola, periscope, antenna on the bustle
    g.add(g.cyl_z(roof, roof + 1.5, 0, -4, 2.2), PLATE)
    g.add(g.box(3, 4, 1, 3, roof, roof + 1), DARK)
    g.add(g.box(-13, -12, 4, 5, z0 + 5.5, z0 + 13), DARK)
    return g


def build(normals):
    return build_hull().to_section(normals), build_turret().to_section(normals)
