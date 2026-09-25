"""Read/write Westwood .vxl voxel models and .hva animation files (RA2/YR), following XCC's vxl_file.

A section's voxels are held as two numpy uint8 arrays indexed [x, y, z]: `color` and `normal`,
plus a boolean `solid` mask (color index 0 is legal, so emptiness is tracked separately).
"""
import struct
import numpy as np

VXL_ID = b"Voxel Animation\0"
TAILER = struct.Struct("<3if12f6f4B")


class Section:
    def __init__(self, name, cx, cy, cz):
        self.name = name
        self.solid = np.zeros((cx, cy, cz), bool)
        self.color = np.zeros((cx, cy, cz), np.uint8)
        self.normal = np.zeros((cx, cy, cz), np.uint8)
        self.scale = 1 / 12
        self.transform = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]
        self.bounds = [-cx / 2, -cy / 2, -cz / 2, cx / 2, cy / 2, cz / 2]
        self.normal_mode = 4

    @property
    def size(self):
        return self.solid.shape

    def resized(self, pad):
        """Return a copy grown by pad=((x0,x1),(y0,y1),(z0,z1)) voxels, keeping world scale."""
        cx, cy, cz = self.size
        step = [(self.bounds[i + 3] - self.bounds[i]) / n for i, n in enumerate((cx, cy, cz))]
        s = Section(self.name, cx + sum(pad[0]), cy + sum(pad[1]), cz + sum(pad[2]))
        s.scale, s.transform, s.normal_mode = self.scale, list(self.transform), self.normal_mode
        s.bounds = [self.bounds[i] - pad[i][0] * step[i] for i in range(3)] + \
                   [self.bounds[i + 3] + pad[i][1] * step[i] for i in range(3)]
        sl = tuple(slice(p[0], p[0] + n) for p, n in zip(pad, (cx, cy, cz)))
        s.solid[sl], s.color[sl], s.normal[sl] = self.solid, self.color, self.normal
        return s


class Vxl:
    def __init__(self):
        self.sections = []
        self.palette = bytes(768)
        self.remap = 0x1F10

    @classmethod
    def load(cls, path):
        d = open(path, "rb").read()
        assert d[:16] == VXL_ID, path
        v = cls()
        _, nh, nt, size, v.remap = struct.unpack_from("<iIIIH", d, 16)
        v.palette = d[34:802]
        names = [d[802 + 28 * i:818 + 28 * i].split(b"\0")[0].decode() for i in range(nh)]
        body = 802 + 28 * nh
        tail = body + size
        for i in range(nt):
            t = TAILER.unpack_from(d, tail + TAILER.size * i)
            start, _end, data = t[0:3]
            cx, cy, cz, mode = t[22:26]
            s = Section(names[i], cx, cy, cz)
            s.scale, s.transform, s.bounds, s.normal_mode = t[3], list(t[4:16]), list(t[16:22]), mode
            starts = struct.unpack_from(f"<{cx * cy}i", d, body + start)
            for col, off in enumerate(starts):
                if off < 0:
                    continue
                x, y = col % cx, col // cx
                p, z = body + data + off, 0
                while z < cz:
                    z += d[p]
                    n = d[p + 1]
                    p += 2
                    for _ in range(n):
                        s.solid[x, y, z], s.color[x, y, z], s.normal[x, y, z] = True, d[p], d[p + 1]
                        p += 2
                        z += 1
                    p += 1
            v.sections.append(s)
        return v

    def save(self, path):
        heads, bodies, tails = b"", bytearray(), b""
        for i, s in enumerate(self.sections):
            heads += struct.pack("<16siii", s.name.encode()[:15], i, 1, 0)
        for s in self.sections:
            cx, cy, cz = s.size
            starts, ends, data = [], [], bytearray()
            for y in range(cy):
                for x in range(cx):
                    col = s.solid[x, y]
                    if not col.any():
                        starts.append(-1); ends.append(-1)
                        continue
                    begin, z = len(data), 0
                    zs = np.flatnonzero(col)
                    runs = np.split(zs, np.flatnonzero(np.diff(zs) != 1) + 1)
                    for r in runs:
                        data += bytes((r[0] - z, len(r)))
                        for k in r:
                            data += bytes((s.color[x, y, k], s.normal[x, y, k]))
                        data.append(len(r))
                        z = r[-1] + 1
                    if z != cz:
                        data += bytes((cz - z, 0, 0))
                    starts.append(begin); ends.append(len(data) - 1)
            base = len(bodies)
            bodies += struct.pack(f"<{cx * cy}i", *starts) + struct.pack(f"<{cx * cy}i", *ends)
            data_ofs = len(bodies)
            bodies += data
            tails += TAILER.pack(base, base + 4 * cx * cy, data_ofs, s.scale, *s.transform, *s.bounds,
                                 cx, cy, cz, s.normal_mode)
        n = len(self.sections)
        out = VXL_ID + struct.pack("<iIIIH", 1, n, n, len(bodies), self.remap) + self.palette
        open(path, "wb").write(out + heads + bytes(bodies) + tails)


def load_hva(path):
    """Return (section names, frames x sections x 3x4 matrices)."""
    d = open(path, "rb").read()
    nf, ns = struct.unpack_from("<II", d, 16)
    names = [d[24 + 16 * i:40 + 16 * i].split(b"\0")[0].decode() for i in range(ns)]
    m = np.frombuffer(d, "<f4", nf * ns * 12, 24 + 16 * ns).reshape(nf, ns, 3, 4)
    return names, m


def load_normal_table(path, mode):
    """Normal vectors for a VXL normal mode (2 = TS, 4 = RA2) from FinalAlert2's voxel_normal_tables.bin."""
    d = open(path, "rb").read()
    p = 1
    for _ in range(d[0]):
        cls, count = d[p], d[p + 1]
        vecs = np.frombuffer(d, "<f4", count * 3, p + 2).reshape(count, 3)
        p += 2 + count * 12
        if cls == mode:
            return vecs.copy()
    raise KeyError(mode)
