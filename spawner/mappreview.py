"""Decode the preview picture stored in a RA2/YR map: [Preview] Size=0,0,W,H and [PreviewPack],
base64 of LZO1X-compressed chunks (uint16 compressed size, uint16 decompressed size, data)
holding W*H 24-bit RGB pixels."""
import base64, struct


def lzo1x_decompress(src):
    """Plain LZO1X decompressor (the format described in lzo's doc/LZO1X.TXT)."""
    out, ip, state = bytearray(), 0, 0

    def run_length(ip, base):   # a zero length field is extended by 255 per zero byte, then one more byte
        n = base
        while src[ip] == 0:
            n += 255
            ip += 1
        return n + src[ip], ip + 1

    if src[0] > 17:
        t = src[0] - 17
        out += src[1:1 + t]
        ip, state = 1 + t, (4 if t >= 4 else t)
    while True:
        t = src[ip]
        ip += 1
        if t < 16:
            if state == 0:              # literal run
                n, ip = run_length(ip, 15) if t == 0 else (t, ip)
                out += src[ip:ip + n + 3]
                ip += n + 3
                state = 4
                continue
            if state == 4:              # 3-byte match straight after a literal run
                dist, length = 2049 + (t >> 2) + (src[ip] << 2), 3
            else:                       # 2-byte match
                dist, length = 1 + (t >> 2) + (src[ip] << 2), 2
            ip += 1
            s = t & 3
        elif t < 32:
            length, ip = run_length(ip, 7) if t & 7 == 0 else (t & 7, ip)
            length += 2
            d = src[ip] | src[ip + 1] << 8
            ip += 2
            dist = 16384 + ((t & 8) << 11) + (d >> 2)
            if dist == 16384:           # end of stream
                break
            s = d & 3
        elif t < 64:
            length, ip = run_length(ip, 31) if t & 31 == 0 else (t & 31, ip)
            length += 2
            d = src[ip] | src[ip + 1] << 8
            ip += 2
            dist, s = 1 + (d >> 2), d & 3
        else:
            length = (3 + ((t >> 5) & 1)) if t < 128 else (5 + ((t >> 5) & 3))
            dist, s = 1 + ((t >> 2) & 7) + (src[ip] << 3), t & 3
            ip += 1
        start = len(out) - dist
        if dist >= length:
            out += out[start:start + length]
        else:                           # overlapping copy repeats the last dist bytes
            for i in range(length):
                out.append(out[start + i])
        out += src[ip:ip + s]
        ip += s
        state = s
    return bytes(out)


def preview(map_text):
    """(width, height, rgb bytes) from a map's text, or None if it has no usable preview."""
    size, pack, sec = None, [], None
    for line in map_text.splitlines():
        line = line.strip()
        if line.startswith("["):
            sec = line[1:line.find("]")]
        elif sec == "Preview" and line.startswith("Size="):
            size = [int(v) for v in line[5:].split(",")]
        elif sec == "PreviewPack" and "=" in line:
            pack.append(line.split("=", 1)[1])
    if not size or not pack:
        return None
    w, h = size[2], size[3]
    data, pos, out = base64.b64decode("".join(pack)), 0, bytearray()
    while pos + 4 <= len(data) and len(out) < w * h * 3:
        csize, _ = struct.unpack_from("<HH", data, pos)
        out += lzo1x_decompress(data[pos + 4:pos + 4 + csize])
        pos += 4 + csize
    if w <= 0 or h <= 0 or len(out) < w * h * 3:
        return None
    return w, h, bytes(out[:w * h * 3])


def start_points(map_text):
    """{start position (0-7): (x, y)} in preview pixels. A map cell (cx, cy) (waypoint value cy*1000+cx)
    lies at x = cx - cy + W, y = (cx + cy - W) / 2 on the preview, W being the width in [Map] Size;
    this matches the red start markers the game draws into the preview to within a pixel."""
    width, points, sec = None, {}, None
    for line in map_text.splitlines():
        line = line.strip()
        if line.startswith("["):
            sec = line[1:line.find("]")]
        elif "=" in line:
            key, value = line.split("=", 1)
            if sec == "Map" and key == "Size":
                width = int(value.split(",")[2])
            elif sec == "Waypoints" and key.isdigit() and int(key) < 8:
                points[int(key)] = int(value)
    if width is None:
        return {}
    return {n: (v % 1000 - v // 1000 + width, (v % 1000 + v // 1000 - width) / 2) for n, v in points.items()}
