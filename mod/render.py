"""Preview renderer for RA2 voxel units and TS/RA2 SHP images (needs numpy + Pillow)."""
import math, struct
import numpy as np
from PIL import Image


def load_pal(path):
    """RA2 palettes are 6-bit VGA values."""
    p = np.frombuffer(open(path, "rb").read(), np.uint8).reshape(256, 3).astype(np.int32)
    return np.clip(p * 255 // 63, 0, 255).astype(np.uint8)


def remap_colors(pal, rgb):
    """Replace remap indices 16..31 with a ramp of `rgb` (house colour), bright to dark."""
    pal = pal.copy()
    for i in range(16):
        k = 1.0 - i / 18
        pal[16 + i] = [min(255, int(c * k)) for c in rgb]
    return pal


def voxel_points(section, hva=None):
    """World-space centres, colours and normal indices of every solid voxel."""
    xs, ys, zs = np.nonzero(section.solid)
    b, size = section.bounds, section.size
    step = [(b[i + 3] - b[i]) / size[i] for i in range(3)]
    p = np.stack([b[0] + (xs + .5) * step[0], b[1] + (ys + .5) * step[1], b[2] + (zs + .5) * step[2]], 1)
    if hva is not None:
        m = np.asarray(hva, np.float64)
        p = p @ m[:, :3].T + m[:, 3] * section.scale
    return p, section.color[xs, ys, zs], section.normal[xs, ys, zs]


def render(parts, pal, normals, yaw_deg=225, elev_deg=30, px=6, size=(480, 360), light=(-1, -1, 1.2),
           center=(0, 0, 6), bg=(0, 0, 0, 0)):
    """parts: list of (section, hva_matrix_or_None, (dx,dy,dz) world offset, yaw offset degrees)."""
    W, H = size
    img = np.zeros((H, W, 4), np.float32)
    img[:] = bg
    zbuf = np.full((H, W), -1e9)
    L = np.asarray(light, float); L /= np.linalg.norm(L)
    e = math.radians(elev_deg)
    for sec, hva, off, turn in parts:
        p, col, nrm = voxel_points(sec, hva)
        n = normals[np.minimum(nrm, len(normals) - 1)].astype(float)
        t = math.radians(turn)
        rt = np.array([[math.cos(t), -math.sin(t), 0], [math.sin(t), math.cos(t), 0], [0, 0, 1]])
        p = p @ rt.T + np.asarray(off, float) - np.asarray(center, float)
        n = n @ rt.T
        a = math.radians(yaw_deg)
        ry = np.array([[math.cos(a), -math.sin(a), 0], [math.sin(a), math.cos(a), 0], [0, 0, 1]])
        p, n = p @ ry.T, n @ ry.T
        shade = 0.55 + 0.75 * np.clip(n @ L, 0, None)
        sx = p[:, 0]
        sy = -(p[:, 2] * math.cos(e)) - p[:, 1] * math.sin(e)
        depth = -p[:, 1] * math.cos(e) + p[:, 2] * math.sin(e)
        rgb = np.clip(pal[col].astype(float) * shade[:, None], 0, 255)
        r = max(1, int(round(px * 0.75)))
        cx = (W / 2 + sx * px).astype(int)
        cy = (H / 2 + sy * px).astype(int)
        for i in np.argsort(depth):
            y0, y1, x0, x1 = max(cy[i] - r, 0), min(cy[i] + r, H), max(cx[i] - r, 0), min(cx[i] + r, W)
            if y0 >= y1 or x0 >= x1:
                continue
            m = zbuf[y0:y1, x0:x1] < depth[i]
            zbuf[y0:y1, x0:x1][m] = depth[i]
            img[y0:y1, x0:x1][m] = (*rgb[i], 255)
    return Image.fromarray(img.astype(np.uint8), "RGBA")


def read_shp(path):
    """TS/RA2 SHP: returns (width, height, [(x, y, w, h, index-array or None)])."""
    d = open(path, "rb").read()
    _, W, H, n = struct.unpack_from("<4H", d)
    frames = []
    for i in range(n):
        x, y, w, h, flags, _r, _g, _b, _a, _res, off = struct.unpack_from("<4HI4BII", d, 8 + 24 * i)
        if w == 0 or off == 0:
            frames.append((x, y, w, h, None)); continue
        if flags & 2:
            buf, p = bytearray(), off
            for _ in range(h):
                ln = struct.unpack_from("<H", d, p)[0]
                q, row = p + 2, bytearray()
                while q < p + ln:
                    if flags & 1 and d[q] == 0:
                        row += bytes(d[q + 1]); q += 2
                    else:
                        row.append(d[q]); q += 1
                buf += row[:w].ljust(w, b"\0"); p += ln
            a = np.frombuffer(bytes(buf), np.uint8).reshape(h, w)
        else:
            a = np.frombuffer(d, np.uint8, w * h, off).reshape(h, w)
        frames.append((x, y, w, h, a))
    return W, H, frames


def write_shp(path, index_img, template_path=None):
    """Write one uncompressed full-frame SHP (the layout used by stock cameos)."""
    h, w = index_img.shape
    flags, rgba = 0, b"\0\0\0\0"
    if template_path:
        t = open(template_path, "rb").read()
        flags, rgba = struct.unpack_from("<I", t, 16)[0], t[20:24]
    hdr = struct.pack("<4H", 0, w, h, 1) + struct.pack("<4HI", 0, 0, w, h, flags) + rgba + struct.pack("<II", 0, 32)
    open(path, "wb").write(hdr + index_img.astype(np.uint8).tobytes())


def shp_to_image(path, pal, frame=0):
    W, H, frames = read_shp(path)
    x, y, w, h, a = frames[frame]
    out = np.zeros((H, W, 3), np.uint8)
    out[y:y + h, x:x + w] = pal[a]
    return Image.fromarray(out, "RGB")
