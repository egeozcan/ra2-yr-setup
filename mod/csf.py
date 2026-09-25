"""Read/modify Westwood .csf string tables (RA2/YR)."""
import struct


def load(path):
    """Return (header fields, [(label, value, extra_or_None)]) keeping file order."""
    d = open(path, "rb").read()
    assert d[:4] == b" FSC", path
    version, nlabels, nstrings, unused, lang = struct.unpack_from("<5I", d, 4)
    p, out = 24, []
    for _ in range(nlabels):
        assert d[p:p + 4] == b" LBL", p
        count, ln = struct.unpack_from("<II", d, p + 4)
        name = d[p + 12:p + 12 + ln].decode("latin-1")
        p += 12 + ln
        value, extra = "", None
        for k in range(count):
            tag = d[p:p + 4]
            n = struct.unpack_from("<I", d, p + 4)[0]
            raw = bytes(~b & 0xFF for b in d[p + 8:p + 8 + 2 * n])
            p += 8 + 2 * n
            if tag == b"WRTS":
                m = struct.unpack_from("<I", d, p)[0]
                ex = d[p + 4:p + 4 + m]
                p += 4 + m
            else:
                ex = None
            if k == 0:
                value, extra = raw.decode("utf-16-le"), ex
        out.append((name, value, extra))
    return (version, unused, lang), out


def save(path, header, entries):
    version, unused, lang = header
    b = bytearray(b" FSC" + struct.pack("<5I", version, len(entries), len(entries), unused, lang))
    for name, value, extra in entries:
        nb = name.encode("latin-1")
        b += b" LBL" + struct.pack("<II", 1, len(nb)) + nb
        enc = bytes(~c & 0xFF for c in value.encode("utf-16-le"))
        b += (b"WRTS" if extra is not None else b" RTS") + struct.pack("<I", len(value)) + enc
        if extra is not None:
            b += struct.pack("<I", len(extra)) + extra
    open(path, "wb").write(bytes(b))


def set_string(entries, label, value):
    for i, (n, _, ex) in enumerate(entries):
        if n.lower() == label.lower():
            entries[i] = (n, value, ex)
            return
    entries.append((label, value, None))
