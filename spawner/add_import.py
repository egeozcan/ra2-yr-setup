#!/usr/bin/env python3
"""Make a copy of a 32-bit PE exe that also imports DLL!FUNC, so Windows loads DLL before the game starts.

usage: add_import.py IN.exe OUT.exe [DLL [FUNC]]     (defaults: yspawn.dll, yspawn_init)

The import table is rebuilt in a new writable section: the original descriptors, one new
descriptor for DLL, and the terminator. The original tables stay where they are, so the
exe's own import addresses do not move. The bound-import directory is cleared.
"""
import struct, sys


def align(x, a):
    return (x + a - 1) // a * a


def main(src, dst, dll="yspawn.dll", func="yspawn_init"):
    d = bytearray(open(src, "rb").read())
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[pe:pe + 4] == b"PE\0\0"
    nsec, = struct.unpack_from("<H", d, pe + 6)
    optsz, = struct.unpack_from("<H", d, pe + 20)
    opt = pe + 24
    assert struct.unpack_from("<H", d, opt)[0] == 0x10B, "not PE32"
    sec_align, file_align = struct.unpack_from("<II", d, opt + 32)
    size_headers, = struct.unpack_from("<I", d, opt + 60)
    ddir = opt + 96
    imp_rva, imp_size = struct.unpack_from("<II", d, ddir + 8 * 1)
    sect = opt + optsz

    secs = [struct.unpack_from("<8sIIII", d, sect + 40 * i) for i in range(nsec)]
    new_hdr = sect + 40 * nsec
    assert new_hdr + 40 <= size_headers, "no room for another section header"
    assert not any(d[new_hdr:new_hdr + 40]), "section header slot is not empty"

    def rva2off(rva):
        for _, vs, va, rs, rp in secs:
            if va <= rva < va + max(vs, rs):
                return rva - va + rp
        raise ValueError(hex(rva))

    # original descriptors (20 bytes each, up to the all-zero terminator)
    descs = []
    off = rva2off(imp_rva)
    while any(d[off:off + 20]):
        descs.append(bytes(d[off:off + 20]))
        off += 20

    _, lvs, lva, lrs, lrp = secs[-1]
    va = align(lva + max(lvs, lrs), sec_align)
    raw = align(len(d), file_align)

    # layout of the new section
    body = bytearray()
    desc_off = 0
    body += b"\0" * (20 * (len(descs) + 2))
    ilt_off = len(body); body += b"\0" * 8
    iat_off = len(body); body += b"\0" * 8
    name_off = len(body); body += dll.encode() + b"\0"
    body += b"\0" * (len(body) % 2)
    hint_off = len(body); body += struct.pack("<H", 0) + func.encode() + b"\0"

    for i, desc in enumerate(descs):
        body[desc_off + 20 * i:desc_off + 20 * (i + 1)] = desc
    struct.pack_into("<IIIII", body, desc_off + 20 * len(descs),
                     va + ilt_off, 0, 0, va + name_off, va + iat_off)
    struct.pack_into("<I", body, ilt_off, va + hint_off)
    struct.pack_into("<I", body, iat_off, va + hint_off)

    vsize = len(body)
    body += b"\0" * (align(len(body), file_align) - len(body))

    d += b"\0" * (raw - len(d))
    d += body
    struct.pack_into("<8sIIIIIIHHI", d, new_hdr, b".yspawn", vsize, va, len(body), raw,
                     0, 0, 0, 0, 0xC0000040)   # initialised data, read, write
    struct.pack_into("<H", d, pe + 6, nsec + 1)
    struct.pack_into("<I", d, opt + 56, align(va + vsize, sec_align))        # SizeOfImage
    struct.pack_into("<II", d, ddir + 8 * 1, va + desc_off, 20 * (len(descs) + 2))
    struct.pack_into("<II", d, ddir + 8 * 11, 0, 0)                          # bound imports
    struct.pack_into("<I", d, opt + 64, 0)                                   # checksum
    open(dst, "wb").write(d)
    print(f"{dst}: {len(descs)} imports + {dll}!{func}, section .yspawn at {va:#x}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
