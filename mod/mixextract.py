#!/usr/bin/env python3
"""Minimal read-only extractor for Westwood RA2/YR .mix archives (incl. encrypted headers).

usage: mixextract.py ARCHIVE NAME[/NESTED_NAME...] OUT_FILE
e.g.   mixextract.py ra2md.mix localmd.mix/rulesmd.ini rulesmd.ini
"""
import base64, struct, sys, zlib

# ---- Blowfish (constants are the hex digits of pi) ----
def _pi_hex_words(n):
    digits = n * 8 + 16
    prec = 4 * digits + 64
    one = 1 << prec
    def arctan_inv(x):
        total, term, k, x2 = 0, one // x, 1, x * x
        while term:
            total += term // k if (k // 2) % 2 == 0 else -(term // k)
            term //= x2
            k += 2
        return total
    pi = 4 * (4 * arctan_inv(5) - arctan_inv(239))
    frac = pi - (3 << prec)
    frac >>= prec - 32 * n
    return [(frac >> (32 * (n - 1 - i))) & 0xFFFFFFFF for i in range(n)]

class Blowfish:
    def __init__(self, key):
        w = _pi_hex_words(18 + 1024)
        self.P, S = w[:18], w[18:]
        self.S = [S[i * 256:(i + 1) * 256] for i in range(4)]
        j = 0
        for i in range(18):
            d = 0
            for _ in range(4):
                d = (d << 8) | key[j % len(key)]
                j += 1
            self.P[i] ^= d
        l = r = 0
        for i in range(0, 18, 2):
            l, r = self._enc(l, r)
            self.P[i], self.P[i + 1] = l, r
        for s in self.S:
            for i in range(0, 256, 2):
                l, r = self._enc(l, r)
                s[i], s[i + 1] = l, r

    def _f(self, x):
        S = self.S
        return ((((S[0][x >> 24] + S[1][(x >> 16) & 255]) & 0xFFFFFFFF) ^ S[2][(x >> 8) & 255]) + S[3][x & 255]) & 0xFFFFFFFF

    def _enc(self, l, r):
        for i in range(16):
            l ^= self.P[i]
            r ^= self._f(l)
            l, r = r, l
        l, r = r, l
        return l ^ self.P[17], r ^ self.P[16]

    def _dec(self, l, r):
        for i in range(17, 1, -1):
            l ^= self.P[i]
            r ^= self._f(l)
            l, r = r, l
        l, r = r, l
        return l ^ self.P[0], r ^ self.P[1]

    def decrypt(self, data):
        out = bytearray()
        for i in range(0, len(data), 8):
            l, r = struct.unpack('>II', data[i:i + 8])
            out += struct.pack('>II', *self._dec(l, r))
        return bytes(out)

# ---- Westwood public RSA key -> Blowfish key ----
_PUBKEY = base64.b64decode("AihRvNoIbTn85FZRYNZRcT+i6KpU+maCsEqr3Q5q+LDB5tH7Tz2qQ38V")
assert _PUBKEY[0] == 2
_N = int.from_bytes(_PUBKEY[2:2 + _PUBKEY[1]], 'big')
_E = 0x10001

def blowfish_key(key_source):
    a = (_N.bit_length() - 1) // 8
    out = bytearray()
    for i in range(0, len(key_source) - a, a + 1):
        v = pow(int.from_bytes(key_source[i:i + a + 1], 'little'), _E, _N)
        out += v.to_bytes(a + 1, 'little')[:a]
    return bytes(out[:56])

# ---- MIX parsing ----
def mix_id(name):
    name = name.upper()
    l = len(name)
    a = l >> 2
    if l & 3:
        name += chr(l - (a << 2))
        i = 3 - (l & 3)
        while i:
            name += name[a << 2]
            i -= 1
    return struct.unpack('<i', struct.pack('<I', zlib.crc32(name.encode('latin-1'))))[0]

def read_index(data):
    flags = struct.unpack_from('<I', data, 0)[0]
    if flags & 0xFFFF:  # old unencrypted TD/RA format: first word is count
        count, _ = struct.unpack_from('<HI', data, 0)
        hdr, body = data[6:6 + count * 12], 6 + count * 12
    elif flags & 0x20000:
        bf = Blowfish(blowfish_key(data[4:84]))
        first = bf.decrypt(data[84:92])
        count, _ = struct.unpack_from('<HI', first, 0)
        hlen = (6 + count * 12 + 7) & ~7
        hdr = bf.decrypt(data[84:84 + hlen])[6:6 + count * 12]
        body = 84 + hlen
    else:
        count, _ = struct.unpack_from('<HI', data, 4)
        hdr, body = data[10:10 + count * 12], 10 + count * 12
    idx = {}
    for i in range(count):
        fid, off, size = struct.unpack_from('<iII', hdr, i * 12)
        idx[fid] = (body + off, size)
    return idx

def extract(data, path):
    for part in path.split('/'):
        idx = read_index(data)
        fid = mix_id(part)
        if fid not in idx:
            raise KeyError(f"{part} not found (id {fid:#010x}, {len(idx)} entries)")
        off, size = idx[fid]
        data = data[off:off + size]
    return data

if __name__ == '__main__':
    archive, path, out = sys.argv[1:4]
    with open(archive, 'rb') as f:
        blob = extract(f.read(), path)
    with open(out, 'wb') as f:
        f.write(blob)
    print(f"{path}: {len(blob)} bytes -> {out}")
