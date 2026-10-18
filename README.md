# pngc

A PNG decoder in one C11 file, 908 lines, with **libpng as the only authority
on what the pixels are**. No zlib, no libpng linked, no third-party code: the
DEFLATE decoder is vendored from the sibling `../inflate` with attribution,
and everything else is here.

## make test

```
$ make clean && make test
rm -f pngc pngc-asan
rm -rf t/tmp
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -ffile-prefix-map=/home/x/projects/pngc=. -o pngc pngc.c
t/run.sh ./pngc

PngSuite (full): 162/162 valid decoded, 14/14 corrupt rejected
74 passed, 0 failed
```

```
$ make test-asan

PngSuite (full): 162/162 valid decoded, 14/14 corrupt rejected
74 passed, 0 failed
```

18 seconds, 31 under the sanitizers.

Usage is a file or stdin, PAM on stdout:

```
$ pngc photo.png > photo.pam        # netpbm P7, RGB_ALPHA, 4 channels
$ pngc < photo.png | pnmtopng       # if you have netpbm, it reads this
$ pngc -r photo.png                 # the unfiltered scanline bytes
$ pngc -z photo.png                 # the inflated IDAT stream
```

Exit 0 if the file decoded, 1 if it was refused, 2 if the command line was
wrong.

## What the score means

PngSuite is the canonical PNG test corpus: 176 images covering every colour
type, every bit depth, both interlace methods, all five filters, and 14
deliberately corrupt files whose names begin with `x` and which must be
*rejected*. The naming convention encodes the expected result, so the corpus
scores itself and the score is asserted by `make test`, not reported by
it: a regression to 150 fails the suite.

```
162/162  valid files decode
162/162  of those agree with libpng, sample for sample
 14/14   corrupt files rejected: exit 1, nothing on stdout, no crash
```

The 14 are four bad signatures, two signatures mangled by CR/LF translation,
two bad CRC-32s (one on IHDR, one on IDAT), one file with no IDAT at all, two
undefined colour types and three illegal bit depths.

## The oracle, and why there are five of them

A test you wrote, checking code you wrote, against a number you worked out
yourself, proves that you were consistent. Every assertion here is a question
put to something else.

1. libpng, for the pixels. `gdk-pixbuf-thumbnailer` is libpng. Asked for a
thumbnail at the image's own longest side it does no scaling and writes the
same pixels back out as a plain 8-bit non-interlaced PNG. Decoding that and
comparing against our decode of the original tests every hard path: 1/2/4-bit
packing, big-endian 16-bit samples, palettes, tRNS, Adam7: against an
implementation written by other people from the same spec.

The simple path that comparison routes through is not taken on trust. It is
the same path the forward-filter fixtures pin byte for byte, and its inflate
output is already checked against Python's zlib.

Two things libpng settled that would otherwise have been my guesses. Its
16-to-8 reduction here is `png_set_strip_16`, the high byte with no rounding
(rounding instead disagrees on every one of the 24 16-bit files), and the
sub-byte scaling is `v * 255 / max`, so a 4-bit `0xf` becomes `0xff` and not
`0xf0`.

2. PngSuite's own declared equalities. This is the check that catches an
Adam7 or filter bug which merely "produces an image". The corpus says certain
files differ only in *how* the same pixels were written, so their output must
be identical byte for byte: 46 groups of it:

| group | what differs | n |
| --- | --- | --- |
| `basn<X>` vs `basi<X>` | interlaced or not | 15 pairs |
| `s##n3p0#` vs `s##i3p0#` | the same, at 1x1 through 40x40 | 18 pairs |
| `oi1`/`oi2`/`oi4`/`oi9` | one IDAT, two, four, nine | 2 groups |
| `z00`/`z03`/`z06`/`z09` | zlib level 0, 3, 6, 9 | 1 group |
| `bg*` | a bKGD chunk we must ignore | 6 groups |
| `ps*`, `pp0`, `ch*` | sPLT and hIST, likewise | 4 groups |

The `s##` pairs are where it earns its place: a 1x1 interlaced image puts one
pixel in Adam7 pass 1 and *nothing at all* in the other six, and an empty pass
carries no filter bytes either.

And one inequality: `basn0g01` and `basn0g02` must not decode the same. A
comparison harness that silently compares two empty files reports all 46 groups
as passing, and that assertion is what proves they are looking at something.

3. Python's zlib, for the DEFLATE half. `pngc -z` dumps the inflated IDAT
stream and it is compared against `zlib.decompress` for all 162 files, byte for
byte. That splits the decoder in two: past this point a pixel bug cannot be a
DEFLATE bug. `zlib.crc32` separately judges our chunk CRC-32, so the
accept/reject score is not founded on a checksum that only agrees with itself.

4. Forward filters, for the predictors. `t/oracle.py` *encodes* fixtures:
random scanlines filtered forwards using the spec's own formulation, while
pngc reverses them. A shared misreading cannot cancel out across an
encode/decode pair written as inverses of each other. 50 fixtures, every
colour type at 8 and 16 bits, 23x17 so no row length is round, each written
once per filter and once with a different filter on every scanline, plus two
more for Paeth alone: 257x257 of uniform random bytes and 128x128 drawn from
`{0,1,2,126,127,128,129,254,255}`, greyscale depth 8 so that `bpp` is 1 and
every pixel in the image is one more sample of the predictor. 82433 samples.

5. Sanitizers and mutation. `make test-asan` reruns the identical suite
under `-fsanitize=address,undefined`. Mutation testing judges the suite rather
than the code, and is the only reason two of the holes below are closed.

## The cases that break real decoders

Each of these has a test, and for each one there is a recorded mutant that the
test catches.

- Paeth. `p = a + b - c` must be computed signed and wider than a byte; it
  can be -255 and it can be 510. Doing it in `uint8_t` decodes most
  photographs perfectly and fails 19 of 50 fixtures. The comparisons are `<=`
  and not `<`: strict fails 14.
- Average floors. `(a+b)>>1`, no rounding term. `(a+b+1)>>1` fails 8.
- Bit depths below 8. Packed most-significant-bits first, so the last byte
  of a scanline is partially unused, and those spare low bits are not
  required to be zero, so they are masked and never trusted. The predictor
  steps by bytes-per-pixel floored at *one*, not at a pixel, so at depth 1
  through 4 the left neighbour is the adjacent byte.
- 16-bit samples are big-endian, clause 7.1, whatever the machine is.
  Swapping it fails 53 checks.
- Adam7. Seven passes, each with its own width, height and *per-scanline
  filter bytes*, and a pass can legitimately be empty. Scattering the rows one
  position off while keeping every pass dimension correct fails 67.
- A palette tRNS shorter than the palette. Legal, and eight of the eleven
  tRNS files in the corpus are palette files. The entries tRNS does not reach
  stay opaque; a decoder requiring `len == 3 * nplte` rejects real files.
- Zero-length IDAT, and IDAT split anywhere. Chunk boundaries mean
  nothing: eight rewritten variants of each of three real files: one chunk,
  one byte per chunk, empty IDATs first, last, both, and between every data
  byte, and the zlib header straddling a boundary with the Adler-32
  straddling another, seven that must decode identically to the original,
  and one of nothing but empty IDATs that must be refused.
  Treating an empty IDAT as the end of the run breaks three variants per file.
- The corrupt files. All 14 of PngSuite's, plus 27 crafted files for the
  clause 4 and clause 11 rules the corpus has no file for, plus 384 fuzz
  inputs. See below.

## The integer overflow, and the point of the whole guard section

`width * height * bytes_per_pixel` is the oldest bug in this genre, and PNG
hands the attacker both factors in a 13-byte header that is otherwise
perfectly well formed. At 2^31-1 by 2^31-1 and 8 bytes per pixel, the row size
reaches 2^34 and `(rowbytes + 1) * height` needs 65 bits: 36893488115206848519,
which wraps to 18446744041497296903 on a 64-bit machine. A wrapped total is
not a smaller total, it is a total that no longer bounds anything, and the
length check that was supposed to validate the stream then validates against a
number that came from nowhere.

Yes, it is reachable with a real crafted IHDR, and there is a file for it.
`t/oracle.py mkbad` builds six, each with a correct signature and correct
CRC-32s so that nothing upstream of the arithmetic can catch them, and each
asserts its *exact* rejection reason, not merely exit 1.

Three layers, because no one of them covers the others:

1. the 65-bit products are computed in `uint64_t` with an explicit
   pre-multiply guard, in all three places they occur;
2. a flat 1 GiB ceiling on decoded pixel data, so a header that wraps to
   something merely huge, not impossible is refused by policy and not
   by whatever `malloc` happens to do;
3. the inflated stream must be *exactly* the length IHDR implies. Not at
   least, exactly. Too short is a missing scanline. Too long means the file
   is not describing its own image.

Asserting the exact message is load-bearing and was learned the hard way: the
first version of this test grepped for "overflow" anywhere in stderr, and the
guard in `expected_raw` could be deleted with the suite still green, because
the identical guard in `image_bytes` caught the same file with the identical
message. Giving the two distinct messages makes deleting either fail by name.

## Relationship to ../inflate

PNG's IDAT is a zlib (RFC 1950) wrapper around DEFLATE (RFC 1951). The sibling
project `../inflate` already implements RFC 1951 from scratch and is tested
against `/usr/bin/gzip` at every compression level, over eleven fixtures, plus
nine streams that each violate exactly one rule and 300 mutated ones.

So it is vendored, not rewritten. Writing a second DEFLATE here would have
duplicated that project's scope and, worse, thrown away its oracle. A fresh
implementation in this file would be checked only by PNG files, which exercise
a narrow slice of DEFLATE and cannot be varied at will. The decoder half
travels verbatim, bit reader, canonical Huffman, the RFC 1951 tables,
`inflate_raw`, clearly marked in `pngc.c`, and the encoder does not come
along at all.

What is new here is the framing, because `inflate` does gzip and PNG does
zlib: Adler-32, the CMF/FLG header, and the three extra restrictions PNG
clause 10.3 adds on top of RFC 1950 (method 8, window at most 32768, no preset
dictionary), plus a refusal of trailing bytes after the Adler-32. `inflate`'s
own README lists "no raw-DEFLATE or zlib (RFC 1950) mode" as a limitation and
its PLAN has an item for it. This is that item, solved on this side of the
fence, not by changing a project whose suite would not notice.

The vendored code is covered here by its own oracle, independent of
`inflate`'s: `pngc -z` against Python's `zlib.decompress` on all 162 files.

## What does NOT work

Plainly, because a README that only lists successes is not finished.

- No encoder. There is no PNG writer and there will not be one.
- No colour management, at all. `gAMA`, `cHRM`, `sRGB`, `iCCP` and `sBIT`
  are skipped. Samples come out exactly as the file stores them, which is
  correct for a decoder and *wrong for a display*: PngSuite's `g*.png` carry
  gammas from 0.35 to 2.5 and `cs*.png` carry 3-, 5- and 8-bit significant
  depths, and all of them decode to the right samples and the wrong picture.
  This is the gap most likely to matter to a real user.
- No compositing. `bKGD` is ignored and alpha is handed out,
  not applied. That is deliberate and is a tested claim. The `bg*` files must
  decode identically to the versions without a background, but a viewer
  wanting a flat image has to do the blend itself.
- APNG is not animated. `acTL`, `fcTL` and `fdAT` are ancillary, so an
  APNG decodes as its default still image. Arguably right for a PNG decoder;
  stated so nobody is surprised.
- Nothing streams and nothing is progressive. The whole file, the whole
  inflated stream, the whole unfiltered image and the whole RGBA output are
  live at once, so decoding costs several times the pixel count. The 1 GiB
  ceiling stands in for unbounded growth, which means a legal 2 GiB PNG does
  not work at all. Interlaced files could in principle show a coarse image
  early. They do not.
- Ancillary chunk positions are not validated. `gAMA` after IDAT is
  accepted. So is a `cHRM` of the wrong length, since it is never looked at.
  Only PLTE and tRNS have their ordering enforced, because those two change
  the pixels. libpng does not reject the others either, so there is no oracle
  to check a stricter reading against, and inventing one would be worse than
  saying this.
- The reserved case bit of the third chunk-type letter is not enforced.
  Same reason.
- No metadata output. `tEXt`/`iTXt`/`zTXt`/`tIME` are skipped silently.
  `pngc` cannot tell you anything *about* a file, only decode it.
- PAM only. No PPM, no PNG, no PNM with fewer channels: output is always
  4-channel RGBA at the source's sample depth, so a greyscale file comes back
  three times larger than it needs to be.
- Incomplete Huffman trees are tolerated, inherited from `../inflate`: an
  under-subscribed tree builds and an unused bit pattern fails at use
  time, not at build time. zlib is stricter about the literal/length tree.
- Slower than libpng. The inflate core walks Huffman codes one bit at a
  time with no lookup table, and the pixel loop calls `getsample` per channel
  per pixel with a `switch` inside. Fine for a 176-file corpus; not a
  thumbnailer.
- x86-64 Linux, gcc and clang. No other target has been tried.

## Where the tests are still weak

- libpng cannot see the low byte of a 16-bit sample. `gdk-pixbuf` returns
  8-bit pixels, so its agreement on the 24 16-bit files is agreement about
  high bytes only. This was a hole the size of half the data, zeroing every
  low byte of the output left all 71 checks green, and it is now covered by
  Python-generated 16-bit fixtures compared against their expected expansion.
  That closes it, but with *our* plumbing checked against *our* expansion
  rule, not against a second implementation. A tool that emits 16-bit output
  would be a real second opinion. Found by mutation, not by reading, which is
  the honest summary of how much the suite proves on its own.
- The 14 corpus rejections do not assert their reason. The crafted files
  do; these only assert exit 1 and empty stdout. A guard firing for the wrong
  reason on a real corrupt file would pass unnoticed.
- The mutation sweep is `make mutants` now, not something I typed. Ten
  defects, each planted into a copy of `pngc.c`, built, and run against
  the real suite, so a mutant is caught by whatever would catch it in
  ordinary use rather than by an assertion written for it. Nine must die
  and one must survive; an expected survivor that starts dying is as
  interesting as an expected casualty that starts living, so both
  directions fail the sweep. A sed expression that matches nothing is
  also a failure, which is how a mutation table rots: the code moves,
  the pattern stops matching, and the sweep reports a pass for a defect
  it never planted.

  It is not part of `make test`, for the reason `test-asan` is not: a
  build and a full suite run per mutant is about four minutes. It is the
  thing to run when the suite changes, because what it measures is
  whether the suite can still fail.
- One Paeth mutant survives and should not be chased. Swapping the first
  two branches to prefer `b` on a `pa == pb` tie leaves the suite green. That
  looks exactly like a coverage gap and is not one: `pa == pb` means
  `|b-c| == |a-c|`, so either `a == b`, and the branches return the same
  byte, or `a + b == 2c`, in which case `p == c` and `pc == 0`, and both
  orderings fall through to `c` unless `a == b` anyway. Brute-forced over all
  16777216 triples: zero differ. The spec's order is kept because it is the spec's order, and no test needs it.
- The corpus is scored in two modes. `t/run.sh` fetches PngSuite into
  `.scratch/` and verifies its SHA-256. With no network it falls back to the
  54-file subset committed under `t/png/`, scores 40/40 and 14/14, and prints
  `subset` instead of `full`. Which mode ran is always printed, so a passing
  run cannot quietly have been forty files.

  That paragraph was false until the floors stopped being typed in.
  `GOT_VALID` and `GOT_PIX` were literal `162`s while the subset's own
  expectation dropped to 40, so the subset branch compared a count that
  tops out at 40 against 162 and **could not pass**, two failures on any
  clone without the corpus and without the network, while the summary line
  printed the `40/40` promised above. Reproduced by removing `.scratch/`
  and stubbing out the fetch, then fixed by taking the floors from the mode
  that is running.

## Deliberate corners

Each marked in the source with a `shortcut:` comment naming its ceiling.

| where | shortcut | upgrade when |
| --- | --- | --- |
| `MAXPIX` | flat 1 GiB ceiling on decoded pixels | somebody needs a bigger image |
| `slurp` | four whole-image buffers live at once | an image stops fitting in RAM |
| `walk` | ancillary chunks skipped unvalidated | an oracle exists for stricter |
| `die` | exits without freeing | OOM or write-error recovery matters |
| `t/run.sh` | scratch dir keyed on binary name only | two runs of the *same* binary race |

## Files

```
pngc.c        908 lines: chunk walker, CRC-32, IHDR, vendored DEFLATE +
              zlib framing, size guards, five filters, Adam7, RGBA, PAM
Makefile      build, test, test-asan, clean
t/run.sh      the suite: 74 checks, prints the PngSuite score
t/oracle.py   zlib inflate, forward filters, and every fixture generator
t/png/        54 PngSuite files for offline runs, with PROVENANCE
```

## A note on being wrong

Four things in here were wrong and were caught by something other than me
reading the code again.

The harness found its own first bug before the decoder had any pixels in it.
`idat_closed` was set after the first IDAT instead of after the first
non-IDAT, so all six `oi2`/`oi4`/`oi9` files: the ones whose entire purpose
is to split IDAT across several chunks, were rejected as non-consecutive.
Writing the corpus runner at commit 1 is the only reason that was a five-minute
bug, not a confusing one later.

`make test` was green with 62 checks on the commit that added zero-length IDAT
chunks. The identical suite under the sanitizers had twelve failures, three of
them `memcpy(NULL, src, 0)`, undefined even though it copies nothing, and
the rest knock-on, including a rejection test that "failed" only because
UBSan's exit code is 86 and the test asserts 1. `ASAN_OPTIONS=exitcode=86` in
the Makefile is load-bearing for exactly that reason, which `../inflate`
learned first and this project inherited, not rediscovered.

The overflow test could not fail. It grepped for "overflow" anywhere in
stderr, and the guard it existed to protect could be deleted with the suite
green, because a second guard with the same message caught the same file.

And the 16-bit low bytes, above: 71 checks, two independent oracles, and half
the sample data in 24 of the corpus files was never looked at by any of them.

## License

GPL-3. libpng is the oracle this is checked against and `gdk-pixbuf` is how it
is reached. A reimplementation of a format has no business being less free
than the tools that prove it right.
