#!/usr/bin/env python3
"""The parts of the oracle that need something other than a shell.

Nothing here decodes a PNG the way pngc does. It uses Python's stdlib zlib --
not my code -- to inflate IDAT independently, and it *encodes* test images,
which means the filter predictors here run forwards while pngc runs them
backwards. A shared misreading of the Paeth predictor would therefore not
cancel out: it would show up as a mismatch.

	idat FILE       inflate the concatenated IDAT of FILE to stdout
	crc FILE        print "type length crc-ok" per chunk, zlib.crc32 as judge
	mkfilt DIR      write filter/bit-depth fixtures and their expected pixels
	mkbad DIR       write hostile files: huge IHDR dimensions, wrong lengths
"""
import os
import random
import struct
import sys
import zlib

SIG = b"\x89PNG\r\n\x1a\n"


def chunks(path):
    d = open(path, "rb").read()
    assert d[:8] == SIG, path
    i = 8
    while i + 8 <= len(d):
        (n,) = struct.unpack(">I", d[i:i + 4])
        t = d[i + 4:i + 8]
        body = d[i + 8:i + 8 + n]
        crc = d[i + 8 + n:i + 12 + n]
        yield t, body, crc
        i += 12 + n


def cmd_idat(path):
    data = b"".join(b for t, b, _ in chunks(path) if t == b"IDAT")
    sys.stdout.buffer.write(zlib.decompress(data))


def cmd_crc(path):
    for t, body, crc in chunks(path):
        ok = struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF) == crc
        print(t.decode("latin1"), len(body), "ok" if ok else "BAD")


def png(path, w, h, depth, colour, rows, plte=None, trns=None, interlace=0):
    """rows is a list of (filter, scanline-bytes) already in file order."""
    out = bytearray(SIG)

    def chunk(t, body):
        out.extend(struct.pack(">I", len(body)))
        out.extend(t + body)
        out.extend(struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF))

    chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, colour, 0, 0, interlace))
    if plte:
        chunk(b"PLTE", plte)
    if trns is not None:
        chunk(b"tRNS", trns)
    raw = b"".join(bytes([f]) + bytes(s) for f, s in rows)
    chunk(b"IDAT", zlib.compress(raw, 6))
    chunk(b"IEND", b"")
    open(path, "wb").write(bytes(out))


def paeth(a, b, c):
    """RFC 2083 clause 6.6, written out as the spec writes it. This is the
    predictor pngc has to agree with; getting it from an independent source is
    the entire point of the fixture below."""
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def encode(pixels, w, h, bpp, ftypes):
    """Filter raw scanline bytes forwards. pixels is h rows of w*bpp bytes."""
    rows, prior = [], bytes(len(pixels[0]))
    for y in range(h):
        cur, f = pixels[y], ftypes[y]
        line = bytearray(len(cur))
        for i in range(len(cur)):
            a = cur[i - bpp] if i >= bpp else 0
            b = prior[i]
            c = prior[i - bpp] if i >= bpp else 0
            if f == 0:
                line[i] = cur[i]
            elif f == 1:
                line[i] = (cur[i] - a) & 0xFF
            elif f == 2:
                line[i] = (cur[i] - b) & 0xFF
            elif f == 3:
                line[i] = (cur[i] - ((a + b) >> 1)) & 0xFF
            else:
                line[i] = (cur[i] - paeth(a, b, c)) & 0xFF
        rows.append((f, line))
        prior = cur
    return rows


def cmd_mkfilt(d):
    os.makedirs(d, exist_ok=True)
    rnd = random.Random(20170719)
    index = []
    # Colour types at 8 and 16 bits, each written five times with one single
    # filter throughout, then once with a different filter on every scanline.
    for colour, chans in ((0, 1), (2, 3), (4, 2), (6, 4)):
        for depth in (8, 16):
            bpp = chans * depth // 8
            w, h = 23, 17   # not a multiple of anything
            px = [bytes(rnd.randrange(256) for _ in range(w * bpp))
                  for _ in range(h)]
            for f in range(5):
                name = "f%d_c%d_d%d" % (f, colour, depth)
                png("%s/%s.png" % (d, name), w, h, depth, colour,
                    encode(px, w, h, bpp, [f] * h))
                open("%s/%s.raw" % (d, name), "wb").write(b"".join(px))
                index.append(name)
            name = "fmix_c%d_d%d" % (colour, depth)
            png("%s/%s.png" % (d, name), w, h, depth, colour,
                encode(px, w, h, bpp, [rnd.randrange(5) for _ in range(h)]))
            open("%s/%s.raw" % (d, name), "wb").write(b"".join(px))
            index.append(name)
    # Paeth gets two more, because its tie-breaking is where decoders diverge
    # and 23x17 of uniform random bytes does not hit the ties often enough.
    # Greyscale depth 8 so bpp is 1 and (a, b, c) are three adjacent bytes:
    # every pixel in the image is one more sample of the predictor.
    w = h = 257
    px = [bytes(rnd.randrange(256) for _ in range(w)) for _ in range(h)]
    png("%s/paeth_wide.png" % d, w, h, 8, 0, encode(px, w, h, 1, [4] * h))
    open("%s/paeth_wide.raw" % d, "wb").write(b"".join(px))
    index.append("paeth_wide")
    # Values clustered at the ends and the middle, so p - a, p - b and p - c
    # collide constantly and all three branches plus both ties are taken.
    alpha = [0, 1, 2, 126, 127, 128, 129, 254, 255]
    w = h = 128
    px = [bytes(rnd.choice(alpha) for _ in range(w)) for _ in range(h)]
    png("%s/paeth_tie.png" % d, w, h, 8, 0, encode(px, w, h, 1, [4] * h))
    open("%s/paeth_tie.raw" % d, "wb").write(b"".join(px))
    index.append("paeth_tie")
    print("\n".join(index))


def cmd_mkbad(d):
    """Files a decoder has to refuse without crashing. Every one of these has
    a correct signature and correct CRCs, so nothing upstream of the header
    arithmetic can catch them -- which is the point."""
    os.makedirs(d, exist_ok=True)
    one = [(0, b"\0")]
    cases = [
        # (name, w, h, depth, colour, substring the rejection must mention)
        #
        # 2^31-1 squared at 8 bytes per pixel. Row bytes reach 2^34 and the
        # height is 2^31, so (rowbytes+1)*height needs 65 bits: unguarded it
        # wraps from 36893488115206848519 to 18446744041497296903, and a
        # length check against a wrapped total is no check at all.
        ("ovf_rowsxh", 0x7FFFFFFF, 0x7FFFFFFF, 16, 6,
         "scanline total overflows"),
        # One scanline, 17 GB of it. No 64-bit wrap; the size ceiling is what
        # has to say no.
        ("ovf_onerow", 0x7FFFFFFF, 1, 16, 6, "limit"),
        # 10 GB of RGBA8, the shape a plausible-looking header takes.
        ("ovf_10gb", 100000, 100000, 8, 6, "limit"),
        # The same arithmetic through Adam7, where it is summed seven times.
        ("ovf_adam7", 0x7FFFFFFF, 0x7FFFFFFF, 16, 6,
         "scanline total overflows"),
    ]
    for name, w, h, depth, colour, msg in cases:
        png("%s/%s.png" % (d, name), w, h, depth, colour, one,
            interlace=1 if "adam7" in name else 0)
        print("%s %s" % (name, msg))
    # Honest dimensions, dishonest IDAT: one byte short and one byte long.
    png("%s/short_idat.png" % d, 8, 8, 8, 0, [(0, bytes(8))] * 7)
    print("short_idat size")
    png("%s/long_idat.png" % d, 8, 8, 8, 0, [(0, bytes(8))] * 9)
    print("long_idat size")


if __name__ == "__main__":
    {"idat": cmd_idat, "crc": cmd_crc, "mkfilt": cmd_mkfilt,
     "mkbad": cmd_mkbad}[sys.argv[1]](
        *sys.argv[2:])
