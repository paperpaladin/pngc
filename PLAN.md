# PLAN

## Contract

One C11 file and a CLI. `pngc file.png > file.pam` reads a PNG (W3C PNG 1.2 /
RFC 2083) and writes the decoded image as a netpbm PAM, `RGB_ALPHA`, 4
channels, maxval 255 or 65535 following the source. Signature and chunk
structure parsed, CRC-32 verified on every chunk, IDAT inflated from its zlib
wrapper, all five scanline filters reversed, bit depths 1/2/4/8/16, colour
types 0/2/3/4/6, tRNS in all three forms, and Adam7. stdin if no file. No
zlib, no libpng, no third-party code except the DEFLATE decoder vendored from
the sibling `../inflate`, with attribution.

`-z` writes the inflated IDAT stream and `-r` the unfiltered scanline bytes;
both exist so the decoder can be oracled in stages, not only end to
end.

## Hard invariant

**Every file PngSuite says is valid decodes to the pixels libpng produces, and
every file PngSuite says is corrupt is refused with exit 1, no output and no
crash.**

Two halves, and the second is not softer than the first. This is a parser for
bytes somebody else chose, with the image dimensions among them, so a
malformed input that reads out of bounds is a failed project and not a bug to
triage. `exit 1` with a message on stderr and nothing on stdout is the only
acceptable response to garbage: not a partial image, and not a crash, which
would also be a nonzero exit.

## Oracle

Ranked by how little control I have over them, most independent first:

1. libpng, reached through `gdk-pixbuf-thumbnailer`. Asked for a thumbnail
   at the image's own longest side it returns the same pixels as a plain 8-bit
   non-interlaced PNG, and that is compared against our decode for all 162
   valid files. Other people's code, other people's reading of the spec. Where
   it disagrees with this code, this code is wrong.
2. PngSuite's naming convention. Which of 176 files are valid and which
   are deliberately corrupt is Willem van Schaik's decision, and the corpus
   also declares which files must decode *identically* to each other,
   interlaced against not, one IDAT against nine, zlib level 0 against 9,
   bKGD/sPLT/hIST added to a file we already have.
3. Python's `zlib`. Inflates the concatenated IDAT independently so the
   DEFLATE half can be compared byte for byte before filtering enters the
   picture, and `zlib.crc32` judges our chunk CRC-32.
4. `t/oracle.py`'s forward filters. It *encodes* fixtures, running the
   predictors in the opposite direction, so a shared misreading cannot cancel
   out across an encode/decode pair written as inverses.
5. AddressSanitizer, UBSan, valgrind. Independent of my intent but not of
   my inputs; they only see the paths something else drove them down.
6. Mutation testing. Delete a check or flip a constant, confirm the suite
   goes red. Judges the tests, not the code.
7. Our own round trip: the matched-pair equalities where both sides are
   ours. Weakest, kept because it catches regressions, never used to establish
   correctness.

## Non-goals

- Encoding. There is no PNG writer here and there will not be one.
- APNG (`acTL`/`fcTL`/`fdAT`). Those are ancillary chunks, so an APNG decodes
  as its default still image, as a PNG decoder should.
- Colour management. `gAMA`, `cHRM`, `sRGB`, `iCCP`, `sBIT` are skipped, so
  samples come out as the file stores them. Displaying `g*.png` or `cs*.png`
  *correctly* needs work this does not do.
- Compositing. `bKGD` is ignored and alpha is handed out, not applied.
- Streaming or progressive output. Whole file in memory, deliberately, with a
  1 GiB ceiling in place of unbounded growth.
- Metadata. `tEXt`/`iTXt`/`zTXt`/`tIME` are skipped, not surfaced.
- Anything beyond x86-64 Linux, gcc and clang.

## Commit sequence

- [x] 01 signature, chunk walker, CRC-32 on every chunk
      why: the format hands you an integrity check, so a corrupt file should
      die before any pixel code can turn it into a merely-wrong image.
- [x] 02 IHDR validated against clause 11.2.2
      why: five of the fourteen corrupt files are legal PNG containers with
      illegal header contents, and the depth/colour-type pair has to be
      checked together or files that do not exist are accepted.
- [x] 03 vendor ../inflate's DEFLATE decoder, add zlib (RFC 1950) framing
      why: reimplementing RFC 1951 would have duplicated that project and
      thrown away its gzip oracle. `-z` plus Python's zlib splits the decoder
      into two independently judged halves.
- [x] 04 size arithmetic and the integer-overflow guards
      why: width x height x bytes-per-pixel is the classic image-decoder CVE
      and PNG hands the attacker both factors.
- [x] 05 reverse the None, Sub, Up and Average filters
      why: four fifths of clause 6, with the one that gets miscoded held back
      so its own commit can be about proving it.
- [x] 06 the Paeth predictor and 82000 samples of it
      why: signed wider-than-byte arithmetic, `<=` not `<`, and ties. All
      three are invisible on ordinary images.
- [x] 07 RGBA output as PAM, colour types 0/2/4/6, libpng as judge
      why: the first commit that produces pixels, and therefore the first one
      an independent decoder can contradict.
- [x] 08 palette images: PLTE, bounds, 162 of 162 decoding
      why: colour type 3 has its rules outside IHDR, and an out-of-range
      index is a corrupt file, not a licence to read past an array.
- [x] 09 tRNS in all three shapes
      why: three different chunks with one name, and a palette tRNS shorter
      than the palette is a real file in the corpus.
- [x] 10 assert the equalities PngSuite declares, plus one inequality
      why: the check that catches an Adam7 or filter bug which merely
      produces an image. The inequality is what proves the equalities are
      looking at something.
- [x] 11 IDAT chunk boundaries must mean nothing
      why: zero-length chunks and a zlib header straddling a boundary are
      legal, and the corpus only has the mild version.
- [x] 12 fix: `memcpy(NULL, src, 0)` on a zero-length IDAT
      why: UBSan found it the moment zero-length IDAT entered the suite.
- [x] 13 the corrupt-file sweep: 384 inputs, exit 0 or 1 and nothing else
      why: "nonzero exit" is not a test, because a segfault is nonzero.
- [x] 14 the chunk rules PngSuite has no file for, plus two controls
      why: clause 4 and clause 11 are mostly uncovered by the corpus, and
      without an accepted control the rejections could all be the fixture
      generator being broken.
- [x] 15 the interface: stdin, -h, exit codes, PAM geometry
      why: cheap, and each one has been wrong in some program at some point.
- [x] 16 close the gap libpng cannot see: 16-bit low bytes
      why: gdk-pixbuf returns 8-bit pixels, so zeroing every low byte left
      all 71 checks green. Found by mutation, not by reading.
- [x] 17 docs: README, PLAN, LICENSE
      why: last, so they describe what was built, not what was hoped.

### Milestone 1: make the weak spots in the suite actually strong

- [ ] 18 run the mutation sweep from `make` instead of from my shell history
      why: it found the 16-bit low-byte hole and three Paeth variants, and it
      currently exists only as commands I typed. A suite that is only
      occasionally checked for being able to fail will eventually stop being
      able to.
- [ ] 19 an independent 16-bit pixel oracle
      why: libpng's agreement on a 16-bit file is agreement about high bytes.
      Item 16 covers the low bytes with Python-generated fixtures, which is
      our own plumbing checked against our own expansion rule; a tool that
      emits 16-bit output (`pnmtopng`, ImageMagick) would be a real
      second opinion on the 24 16-bit files in the corpus.
- [ ] 20 assert stderr content for the corpus rejections too
      why: the crafted files assert their exact rejection reason and the 14
      x*.png do not, so a guard firing for the wrong reason on a real corrupt
      file would pass unnoticed.

### Milestone 2: stop holding everything in memory

- [ ] 21 inflate and unfilter one scanline at a time through a 32K window
      why: four whole-image buffers are live at once, and that is what the 1 GiB
      ceiling is standing in for. A 2 GiB PNG is legal and does not work.
- [ ] 22 write the PAM as rows are finished, not at the end
      why: halves the peak and is the only honest way to decode something
      larger than RAM. Non-interlaced only; Adam7 cannot stream by
      construction.

### Milestone 3: the chunks currently skipped

- [ ] 23 validate ancillary chunk positions and lengths
      why: `gAMA` after IDAT is accepted today. libpng does not reject it
      either, so this needs an oracle before it needs code.
- [ ] 24 `-g` to apply gAMA, and sBIT-aware output
      why: `g*.png` and `cs*.png` decode to the right samples and the wrong
      picture. That is a real limitation and it is currently only documented.
