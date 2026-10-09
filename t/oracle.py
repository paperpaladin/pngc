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
	dims FILE       print "W H" straight out of IHDR
	mksplit DIR F   rewrite F with its IDAT split at awkward boundaries
	mkfuzz DIR F    truncations and single-byte mutations of F
	mkseq DIR       chunk-structure errors PngSuite does not cover
	cmp8 A B        compare two PAM files, truncating 16-bit samples to 8
	pam RAW C D     expand raw samples to the RGBA payload we expect back
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


def cmd_dims(path):
    for t, body, _ in chunks(path):
        if t == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
            print(w, h)
            return
    sys.exit("no IHDR in %s" % path)


def readpam(path):
    d = open(path, "rb").read()
    i = d.index(b"ENDHDR\n") + 7
    hdr = dict(l.split()[:2] for l in d[:i].split(b"\n") if b" " in l)
    return (int(hdr[b"WIDTH"]), int(hdr[b"HEIGHT"]), int(hdr[b"MAXVAL"]),
            int(hdr[b"DEPTH"]), d[i:])


def fail(msg):
    print(msg)
    sys.exit(1)


def cmd_cmp8(a, b):
    """Compare our decode against libpng's, at 8 bits per sample.

    libpng's 16 -> 8 reduction, as gdk-pixbuf asks for it, is png_set_strip_16:
    keep the high byte, no rounding. Measured, not assumed -- rounding instead
    disagrees with libpng on every 16-bit file in PngSuite."""
    wa, ha, mva, da, pa = readpam(a)
    wb, hb, mvb, db, pb = readpam(b)
    if (wa, ha) != (wb, hb):
        fail("size %dx%d vs %dx%d" % (wa, ha, wb, hb))
    if da != 4 or db != 4:
        fail("expected 4 channels")
    if mva == 65535:
        pa = pa[0::2]
    if mvb == 65535:
        pb = pb[0::2]
    if len(pa) != wa * ha * 4 or len(pb) != len(pa):
        fail("payload %d vs %d, want %d"
             % (len(pa), len(pb), wa * ha * 4))
    nd = sum(1 for i in range(len(pa)) if pa[i] != pb[i])
    if nd:
        i = next(i for i in range(len(pa)) if pa[i] != pb[i])
        fail("%d of %d samples differ, first at %d: %d vs %d"
             % (nd, len(pa), i, pa[i], pb[i]))


def cmd_pam(raw, colour, depth):
    """Expand the pixel bytes of a mkfilt fixture into the RGBA payload pngc
    must produce, at the source's own sample depth.

    This exists for one reason: gdk-pixbuf hands back 8-bit pixels, so libpng
    can only ever check the HIGH byte of a 16-bit sample. Setting every low
    byte to zero left the whole suite green until this was added. The data
    here is random bytes generated in Python, so where each one lands is
    pinned by something other than the C code."""
    colour, depth = int(colour), int(depth)
    n = 2 if depth == 16 else 1
    d = open(raw, "rb").read()
    src = [d[i:i + n] for i in range(0, len(d), n)]
    full = b"\xff" * n
    out = bytearray()
    chans = {0: 1, 2: 3, 4: 2, 6: 4}[colour]
    for i in range(0, len(src), chans):
        p = src[i:i + chans]
        if colour == 0:
            out += p[0] * 3 + full
        elif colour == 2:
            out += p[0] + p[1] + p[2] + full
        elif colour == 4:
            out += p[0] * 3 + p[1]
        else:
            out += p[0] + p[1] + p[2] + p[3]
    sys.stdout.buffer.write(bytes(out))


def png(path, w, h, depth, colour, rows, plte=None, trns=None, interlace=0):
    """rows is a list of (filter, scanline-bytes) already in file order."""
    out = bytearray(SIG)

    def chunk(t, body):
        out.extend(struct.pack(">I", len(body)))
        out.extend(t + body)
        out.extend(struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF))

    chunk(b"IHDR",
          struct.pack(">IIBBBBB", w, h, depth, colour, 0, 0, interlace))
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


def cmd_mksplit(d, src):
    """Rewrite one PNG with its zlib stream chopped up differently.

    Clause 4.1.3 allows any number of IDAT chunks of any lengths, zero
    included, and the zlib stream is their concatenation -- so a decoder that
    treats chunk boundaries as meaningful breaks on files like these. All of
    these must decode identically to the original, and the all-empty one must
    be rejected."""
    os.makedirs(d, exist_ok=True)
    parts = list(chunks(src))
    data = b"".join(b for t, b, _ in parts if t == b"IDAT")
    head = [(t, b) for t, b, _ in parts if t not in (b"IDAT", b"IEND")]

    def write(name, pieces):
        out = bytearray(SIG)
        for t, body in head + [(b"IDAT", x) for x in pieces] + [(b"IEND", b"")]:
            out.extend(struct.pack(">I", len(body)))
            out.extend(t + body)
            out.extend(struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF))
        open("%s/%s.png" % (d, name), "wb").write(bytes(out))
        print(name)

    write("one", [data])
    write("perbyte", [data[i:i + 1] for i in range(len(data))])
    write("empty_first", [b"", data])
    write("empty_last", [data, b""])
    write("empty_both", [b"", b"", data, b"", b""])
    write("empty_between", [x for i in range(len(data))
                            for x in (data[i:i + 1], b"")])
    # The 2-byte zlib header straddling a chunk boundary, and then the
    # Adler-32 straddling another one.
    write("straddle", [data[:1], data[1:3], data[3:-3], data[-3:-1],
                       data[-1:]])
    write("allempty", [b"", b"", b""])


def cmd_mkfuzz(d, src):
    """Every truncation of F, and 200 single-byte mutations of it.

    Every byte of a PNG is covered either by the signature or by a chunk
    CRC-32 -- except the four length bytes of each chunk header, which are
    not -- so almost any flipped byte should be caught. The assertion in
    t/run.sh is weaker than that on purpose: exit 0 or exit 1, never a signal
    and never a sanitizer abort."""
    os.makedirs(d, exist_ok=True)
    data = open(src, "rb").read()
    rnd = random.Random(len(data))
    names = []
    for n in range(len(data)):
        open("%s/trunc%d.png" % (d, n), "wb").write(data[:n])
        names.append("trunc%d" % n)
    for i in range(200):
        b = bytearray(data)
        at = rnd.randrange(len(b))
        b[at] ^= 1 << rnd.randrange(8)
        open("%s/mut%d.png" % (d, i), "wb").write(bytes(b))
        names.append("mut%d" % i)
    print("\n".join(names))


def raw_png(path, chunks_in, sig=SIG, tail=b""):
    """Assemble a file from explicit (type, body) pairs, CRCs computed. Takes
    the chunk list literally, so it can build files no encoder would."""
    out = bytearray(sig)
    for t, body in chunks_in:
        out.extend(struct.pack(">I", len(body)))
        out.extend(t + body)
        out.extend(struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF))
    out.extend(tail)
    open(path, "wb").write(bytes(out))


def cmd_mkseq(d):
    """Chunk-structure and chunk-content errors, every one with a correct
    signature and correct CRC-32s, so nothing but the rule being tested can
    catch them. PngSuite's x*.png cover bad signatures, bad CRCs, bad IHDR
    fields and a missing IDAT; these are the rest of clause 4 and 11."""
    os.makedirs(d, exist_ok=True)

    def ih(w=4, h=4, depth=8, colour=0, interlace=0):
        return struct.pack(">IIBBBBB", w, h, depth, colour, 0, 0, interlace)

    def idat(w=4, h=4, bpp=1):
        return zlib.compress(bytes((1 + w * bpp) * h), 6)

    pal = bytes(3 * 4)
    end = (b"IEND", b"")
    cases = []

    def case(name, chunks_in, msg, **kw):
        raw_png("%s/%s.png" % (d, name), chunks_in, **kw)
        cases.append((name, msg))

    case("two_ihdr", [(b"IHDR", ih()), (b"IHDR", ih()), (b"IDAT", idat()),
                      end], "duplicate IHDR")
    case("no_ihdr", [(b"gAMA", b"\0\0\0\1"), (b"IHDR", ih()),
                     (b"IDAT", idat()), end], "not IHDR")
    case("ihdr_short", [(b"IHDR", ih()[:12]), (b"IDAT", idat()), end],
         "13 bytes")
    case("no_iend", [(b"IHDR", ih()), (b"IDAT", idat())], "no IEND")
    case("iend_data", [(b"IHDR", ih()), (b"IDAT", idat()),
                       (b"IEND", b"x")], "IEND is not empty")
    case("after_iend", [(b"IHDR", ih()), (b"IDAT", idat()), end],
         "data after IEND", tail=b"junk")
    case("idat_gap", [(b"IHDR", ih()), (b"IDAT", idat()[:4]),
                      (b"tEXt", b"k\0v"), (b"IDAT", idat()[4:]), end],
         "not consecutive")
    case("unknown_crit", [(b"IHDR", ih()), (b"QRST", b"zz"),
                          (b"IDAT", idat()), end], "unknown critical chunk")
    case("type_digits", [(b"IHDR", ih()), (b"1234", b""), (b"IDAT", idat()),
                         end], "not alphabetic")
    # Colour type 3 with no palette, and a palette where there must not be one.
    case("pal_no_plte", [(b"IHDR", ih(colour=3)), (b"IDAT", idat()), end],
         "no PLTE")
    case("plte_in_grey", [(b"IHDR", ih()), (b"PLTE", pal),
                          (b"IDAT", idat()), end], "greyscale")
    case("plte_in_ga", [(b"IHDR", ih(colour=4)), (b"PLTE", pal),
                        (b"IDAT", idat(bpp=2)), end], "greyscale")
    case("two_plte", [(b"IHDR", ih(colour=3)), (b"PLTE", pal),
                      (b"PLTE", pal), (b"IDAT", idat()), end],
         "duplicate PLTE")
    case("plte_after_idat", [(b"IHDR", ih(colour=3)), (b"IDAT", idat()),
                             (b"PLTE", pal), end], "PLTE after IDAT")
    case("plte_len", [(b"IHDR", ih(colour=3)), (b"PLTE", bytes(7)),
                      (b"IDAT", idat()), end], "RGB triples")
    case("plte_empty", [(b"IHDR", ih(colour=3)), (b"PLTE", b""),
                        (b"IDAT", idat()), end], "RGB triples")
    # Five entries in a 2-bit palette image: four is the most it can index.
    case("plte_overdepth", [(b"IHDR", ih(depth=2, colour=3)),
                            (b"PLTE", bytes(3 * 5)),
                            (b"IDAT", zlib.compress(bytes(2 * 4), 6)), end],
         "bit depth can index")
    # An index of 3 against a palette holding two entries.
    case("plte_index", [(b"IHDR", ih(w=1, h=1, colour=3)),
                        (b"PLTE", bytes(6)),
                        (b"IDAT", zlib.compress(b"\0\3", 6)), end],
         "past the end of PLTE")
    case("trns_on_rgba", [(b"IHDR", ih(colour=6)), (b"tRNS", b"\0\0"),
                          (b"IDAT", idat(bpp=4)), end], "already has an alpha")
    case("trns_grey_len", [(b"IHDR", ih()), (b"tRNS", b"\0\0\0"),
                           (b"IDAT", idat()), end], "not 2 bytes")
    case("trns_rgb_len", [(b"IHDR", ih(colour=2)), (b"tRNS", b"\0\0"),
                          (b"IDAT", idat(bpp=3)), end], "not 6 bytes")
    # Key 300 in an 8-bit image, where no sample can ever equal it.
    case("trns_overdepth", [(b"IHDR", ih()), (b"tRNS", b"\x01\x2c"),
                            (b"IDAT", idat()), end], "above the bit depth")
    case("trns_before_plte", [(b"IHDR", ih(colour=3)), (b"tRNS", b"\0"),
                              (b"PLTE", pal), (b"IDAT", idat()), end],
         "before PLTE")
    case("trns_longer_than_plte", [(b"IHDR", ih(colour=3)), (b"PLTE", pal),
                                   (b"tRNS", bytes(5)), (b"IDAT", idat()),
                                   end], "longer than the palette")
    case("two_trns", [(b"IHDR", ih()), (b"tRNS", b"\0\0"),
                      (b"tRNS", b"\0\0"), (b"IDAT", idat()), end],
         "duplicate tRNS")
    case("trns_after_idat", [(b"IHDR", ih()), (b"IDAT", idat()),
                             (b"tRNS", b"\0\0"), end], "tRNS after IDAT")
    # A length with the high bit set: clause 3.2 caps it at 2^31-1.
    big = bytearray(SIG)
    big.extend(b"\x00\x00\x00\x0dIHDR" + ih())
    big.extend(struct.pack(">I", zlib.crc32(b"IHDR" + ih()) & 0xFFFFFFFF))
    big.extend(b"\x80\x00\x00\x01IDAT")
    open("%s/len_high_bit.png" % d, "wb").write(bytes(big))
    cases.append(("len_high_bit", "chunk length exceeds"))

    # The control. Built by the same helper, with nothing wrong: it must be
    # ACCEPTED. Without it every rejection above could be the helper emitting
    # a malformed IDAT and the suite would be green for the wrong reason.
    raw_png("%s/control.png" % d, [(b"IHDR", ih()), (b"IDAT", idat()), end])
    raw_png("%s/control_pal.png" % d, [(b"IHDR", ih(colour=3)),
                                       (b"PLTE", pal), (b"tRNS", b"\0\x80"),
                                       (b"IDAT", idat()), end])

    print("control ACCEPT")
    print("control_pal ACCEPT")
    for name, msg in cases:
        print("%s %s" % (name, msg))


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
     "mkbad": cmd_mkbad, "dims": cmd_dims, "cmp8": cmd_cmp8,
     "mksplit": cmd_mksplit, "mkfuzz": cmd_mkfuzz,
     "mkseq": cmd_mkseq, "pam": cmd_pam}[sys.argv[1]](
        *sys.argv[2:])
