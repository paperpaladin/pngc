#!/bin/sh
# The oracle. Nothing here compares this decoder against my own reading of the
# PNG spec; every assertion is a question put to something I did not write:
#
#   1. PngSuite's naming convention. A file whose name starts with x is
#      deliberately corrupt and MUST be rejected; everything else MUST decode.
#      Willem van Schaik decided which is which, not me.
#   2. libpng, reached through gdk-pixbuf-thumbnailer, for the actual pixels.
#   3. Python's zlib, for the inflated IDAT stream, byte for byte, before any
#      filtering has had a chance to be wrong.
#   4. Matched pairs inside PngSuite that the corpus itself declares equal:
#      interlaced against non-interlaced, one IDAT against nine, zlib level 0
#      against level 9.
#   5. AddressSanitizer and UBSan, via `make test-asan`.
#
# Usage: t/run.sh [path-to-pngc-binary]

IN=${1:-./pngc}
case $IN in /*) ;; *) IN="$PWD/$IN" ;; esac
R=$(cd "$(dirname "$0")/.." && pwd)

# shortcut: scratch dir is keyed on the binary name, so `make test` and
# `make test-asan` can run side by side but two runs of the SAME binary still
# collide. Append $$ if that ever becomes a real workflow. Deliberately under
# the repo and never /tmp: other agents share this machine.
D=$R/t/tmp/$(basename "$IN")
rm -rf "$D"
mkdir -p "$D"

pass=0
fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); printf 'FAIL: %s\n' "$1"; }

# ------------------------------------------------------------------ corpus
# The whole PngSuite if we can get it, the committed subset otherwise. Both
# are scored, and which one ran is printed, so a passing run can never be a
# run that quietly tested forty files instead of a hundred and seventy-six.
TGZ=$R/.scratch/PngSuite-2017jul19.tgz
SUM=0294b244c95a8342c01b00010cf34abdcabc7c6a34fd0fe1bd963917537bfdc8
S=$R/.scratch/suite
if [ ! -d "$S" ]; then
	mkdir -p "$R/.scratch"
	[ -f "$TGZ" ] || curl -sS -o "$TGZ" \
		http://www.schaik.com/pngsuite/PngSuite-2017jul19.tgz 2>/dev/null
	if [ -f "$TGZ" ] && [ "$(sha256sum < "$TGZ" | cut -d' ' -f1)" = "$SUM" ]; then
		mkdir -p "$S" && tar xzf "$TGZ" -C "$S"
	else
		rm -f "$TGZ"
	fi
fi
if [ -d "$S" ]; then
	MODE=full
	WANT_VALID=162
	WANT_CORRUPT=14
else
	MODE=subset
	S=$R/t/png
	WANT_VALID=40
	WANT_CORRUPT=14
fi

# How many of each this binary is expected to get right. Asserted, not
# reported: a decoder that regresses to 100 has to fail the suite, and a
# decoder that improves has to come here and say so.
GOT_VALID=162   # every valid file in the corpus renders
GOT_CORRUPT=14  # every x*.png is now caught
GOT_PIX=162     # libpng agrees on every valid file in the corpus

# ------------------------------------------------- 1. the corpus, scored
nvalid=0
ncorrupt=0
for f in "$S"/*.png; do
	b=$(basename "$f" .png)
	"$IN" "$f" > "$D/out" 2>"$D/err"
	rc=$?
	case $b in
	x*)	# must be rejected, exactly 1, and with nothing on stdout
		if [ "$rc" = 1 ] && [ ! -s "$D/out" ]; then
			ncorrupt=$((ncorrupt+1))
		elif [ "$rc" != 1 ] && [ "$rc" != 0 ]; then
			bad "$b: exit $rc, not a clean rejection"
		elif [ "$rc" = 1 ]; then
			bad "$b: rejected but wrote $(wc -c < "$D/out") bytes to stdout"
		fi
		;;
	*)	if [ "$rc" = 0 ]; then
			nvalid=$((nvalid+1))
		elif [ "$GOT_VALID" = "$WANT_VALID" ]; then
			bad "$b: valid file rejected ($(cat "$D/err"))"
		else
			# Still building up. The count below is the assertion; naming
			# every not-yet-implemented file as a failure would bury the
			# ones that are real.
			printf 'note: %s not decoded yet (%s)\n' "$b" "$(cat "$D/err")"
		fi
		;;
	esac
done

if [ "$nvalid" = "$GOT_VALID" ]; then ok
else bad "decoded $nvalid valid files, expected $GOT_VALID"; fi
if [ "$ncorrupt" = "$GOT_CORRUPT" ]; then ok
else bad "rejected $ncorrupt corrupt files, expected $GOT_CORRUPT"; fi

# ------------------------ 2. Python's zlib agrees with our inflate, exactly
# Splits the decoder in half. If this passes, every later failure is in the
# filtering, unpacking or interlacing and not in the DEFLATE core; if it fails,
# nothing downstream is worth looking at. zlib is not my code, so this is a
# real second opinion and not a round trip.
zbad=0
zn=0
for f in "$S"/*.png; do
	case $(basename "$f") in x*) continue ;; esac
	"$IN" -z "$f" > "$D/ours" 2>/dev/null || { zbad=$((zbad+1)); continue; }
	python3 "$R/t/oracle.py" idat "$f" > "$D/theirs" 2>/dev/null \
		|| { bad "oracle.py could not inflate $(basename "$f")"; continue; }
	zn=$((zn+1))
	cmp -s "$D/ours" "$D/theirs" || {
		zbad=$((zbad+1))
		bad "inflate: $(basename "$f") differs from python zlib"
	}
done
# Every valid file, regardless of how much of the pixel pipeline exists yet:
# -z stops before the filters, so this count is WANT_VALID and not GOT_VALID.
if [ "$zbad" = 0 ] && [ "$zn" = "$WANT_VALID" ]; then ok
else bad "inflate vs zlib: $zn compared, want $WANT_VALID; $zbad differ"; fi

# Our chunk CRC-32 against zlib.crc32. If these disagreed, the whole
# accept/reject score above would be measuring the wrong thing.
crcbad=0
for f in "$S"/*.png; do
	case $(basename "$f") in xcs*|xhd*|xcr*|xlf*|xs*) continue ;; esac
	python3 "$R/t/oracle.py" crc "$f" 2>/dev/null | grep -q BAD && {
		crcbad=$((crcbad+1))
		bad "zlib.crc32 says $(basename "$f") has a bad CRC but we accepted it"
	}
done
[ "$crcbad" = 0 ] && ok

# --------------------------- 3. the filters, against a forward implementation
# oracle.py filters random scanlines FORWARDS; pngc reverses them. A shared
# misreading of a predictor therefore cannot cancel out -- it shows up as a
# byte mismatch. Every colour type at 8 and 16 bits, 23x17 so that no row
# length is a round number, each written once per filter type and once with a
# different filter on every scanline.
mkdir -p "$D/filt"
nfilt=0
for name in $(python3 "$R/t/oracle.py" mkfilt "$D/filt"); do
	"$IN" -r "$D/filt/$name.png" > "$D/filt/$name.got" 2>"$D/err" || {
		bad "filter fixture $name: $(cat "$D/err")"
		continue
	}
	if cmp -s "$D/filt/$name.raw" "$D/filt/$name.got"; then nfilt=$((nfilt+1))
	else bad "filter fixture $name: unfiltered bytes differ"; fi
done
if [ "$nfilt" = 50 ]; then ok
else bad "filter fixtures: $nfilt of 50 matched"; fi

# ----------------------------------- 4. libpng decides what the pixels are
# The real oracle. gdk-pixbuf-thumbnailer is libpng, and asking it for a
# thumbnail at the image's own size gives back the same pixels written out as
# a plain 8-bit non-interlaced PNG. Decoding that and comparing tests every
# hard path -- 1/2/4-bit packing, 16-bit big-endian samples, palettes, tRNS,
# Adam7 -- against an implementation written by other people.
#
# The simple path it routes through is not taken on trust: it is the same path
# the forward-filter fixtures above pin byte for byte, and its inflate output
# is already compared against Python's zlib.
#
# Dimensions come from oracle.py reading IHDR, not from us, because -s must be
# the image's own longest side: gdk-pixbuf scales DOWN to fit but will happily
# scale UP past it.
npix=0
nskip=0
if command -v gdk-pixbuf-thumbnailer > /dev/null; then
	for f in "$S"/*.png; do
		b=$(basename "$f" .png)
		case $b in x*) continue ;; esac
		"$IN" "$f" > "$D/ours.pam" 2>/dev/null || {
			nskip=$((nskip+1))
			continue
		}
		set -- $(python3 "$R/t/oracle.py" dims "$f")
		side=$1
		[ "$2" -gt "$side" ] && side=$2
		gdk-pixbuf-thumbnailer -s "$side" "$f" "$D/ref.png" 2>/dev/null || {
			bad "$b: libpng refused a file we accepted"
			continue
		}
		"$IN" "$D/ref.png" > "$D/theirs.pam" 2>"$D/err" || {
			bad "$b: cannot decode libpng's own output: $(cat "$D/err")"
			continue
		}
		msg=$(python3 "$R/t/oracle.py" cmp8 "$D/ours.pam" "$D/theirs.pam")
		if [ $? = 0 ]; then
			npix=$((npix+1))
		elif [ "$GOT_PIX" = "$WANT_VALID" ]; then
			bad "$b: disagrees with libpng: $msg"
		else
			printf 'note: %s disagrees with libpng: %s\n' "$b" "$msg"
		fi
	done
	if [ "$npix" = "$GOT_PIX" ]; then ok
	else bad "libpng agreed on $npix files, expected $GOT_PIX"; fi
else
	bad "gdk-pixbuf-thumbnailer missing: the pixel oracle did not run"
fi

# ------------------- 5. equalities the corpus itself declares, not me
# The check that catches an Adam7 or filter bug which merely "produces an
# image". PngSuite's naming convention says these files differ only in how the
# same pixels were written, so their decoded output has to be identical --
# byte for byte, not approximately. Nothing here is my opinion about what the
# image looks like; it is somebody else's statement that two files agree.
#
#   basi<X>  vs basn<X>      interlaced vs not, 15 pairs
#   s##i3p0# vs s##n3p0#     the same, at every size from 1x1 to 40x40
#   oi1/2/4/9                one IDAT vs two vs four vs nine
#   z00/03/06/09             zlib level 0 vs 3 vs 6 vs 9
#   bg*                      a bKGD chunk a non-compositing decoder ignores
#   ps*, pp0, ch*            sPLT and hIST, likewise ignored
dec() {
	"$IN" "$S/$1.png" > "$D/g.pam" 2>"$D/err" || {
		printf 'unreadable'
		return
	}
	md5sum < "$D/g.pam" | cut -d" " -f1
}
same() {
	first=""
	for m in "$@"; do
		[ -f "$S/$m.png" ] || return 0   # subset mode: group not present
	done
	for m in "$@"; do
		h=$(dec "$m")
		case $h in unreadable) bad "equality: $m does not decode"; return ;; esac
		if [ -z "$first" ]; then first=$h; firstn=$m
		elif [ "$h" != "$first" ]; then
			bad "equality: $m and $firstn must decode identically"
			return
		fi
	done
	ok
}

for t in 0g01 0g02 0g04 0g08 0g16 2c08 2c16 3p01 3p02 3p04 3p08 \
         4a08 4a16 6a08 6a16; do
	same "basn$t" "basi$t"
done
for t in 01i3p01:01n3p01 02i3p01:02n3p01 03i3p01:03n3p01 04i3p01:04n3p01 \
         05i3p02:05n3p02 06i3p02:06n3p02 07i3p02:07n3p02 08i3p02:08n3p02 \
         09i3p02:09n3p02 32i3p04:32n3p04 33i3p04:33n3p04 34i3p04:34n3p04 \
         35i3p04:35n3p04 36i3p04:36n3p04 37i3p04:37n3p04 38i3p04:38n3p04 \
         39i3p04:39n3p04 40i3p04:40n3p04; do
	same "s${t%%:*}" "s${t##*:}"
done
same oi1n0g16 oi2n0g16 oi4n0g16 oi9n0g16 basn0g16
same oi1n2c16 oi2n2c16 oi4n2c16 oi9n2c16 basn2c16
same z00n2c08 z03n2c08 z06n2c08 z09n2c08
same basi4a08 bgai4a08
same basn4a08 bgbn4a08
same basi4a16 bgai4a16
same basn4a16 bggn4a16
same basn6a08 bgan6a08 bgwn6a08
same basn6a16 bgan6a16 bgyn6a16
same basn0g08 ps1n0g08 ps2n0g08
same basn2c16 pp0n2c16 ps1n2c16 ps2n2c16
same basn3p04 ch1n3p04
same basn3p08 ch2n3p08

# And one inequality, because a comparison harness that silently compares two
# empty files reports every group above as passing. This is the assertion that
# proves the ones above are looking at something.
if [ "$(dec basn0g01)" = "$(dec basn0g02)" ]; then
	bad "equality harness is vacuous: 1-bit and 2-bit grey came out the same"
else
	ok
fi

# ------------------------- 6. IDAT chunk boundaries must mean nothing
# Clause 4.1.3 allows any number of IDAT chunks of any lengths, zero included,
# and the zlib stream is their concatenation. A decoder that treats a chunk
# boundary as a stream boundary -- or that stops at the first empty IDAT --
# breaks on files like these, and oi9n0g16 in the corpus is the mild version.
# Every variant below must decode to exactly what the original does, except
# the all-empty one, which has no zlib stream at all and must be refused.
for src in basn6a08 basi3p04 basn0g16; do
	[ -f "$S/$src.png" ] || continue
	mkdir -p "$D/sp"
	rm -f "$D/sp"/*.png
	want=$("$IN" "$S/$src.png" | md5sum)
	nsp=0
	for name in $(python3 "$R/t/oracle.py" mksplit "$D/sp" "$S/$src.png"); do
		if [ "$name" = allempty ]; then
			"$IN" "$D/sp/$name.png" > "$D/out" 2>/dev/null
			if [ $? = 1 ] && [ ! -s "$D/out" ]; then nsp=$((nsp+1))
			else bad "$src/allempty: a PNG with no zlib data was accepted"; fi
			continue
		fi
		got=$("$IN" "$D/sp/$name.png" 2>"$D/err" | md5sum)
		if [ "$got" = "$want" ]; then nsp=$((nsp+1))
		else bad "$src/$name: differs from the original ($(cat "$D/err"))"; fi
	done
	if [ "$nsp" = 8 ]; then ok
	else bad "$src: $nsp of 8 IDAT-split variants decoded"; fi
done

# ------------------------------- 7. hostile headers must not be believed
# Width x height x bytes-per-pixel is the classic image-decoder overflow, and
# a crafted IHDR is the whole attack: correct signature, correct CRCs, and
# dimensions whose product does not fit the machine. These assert the exact
# rejection, not merely "exit 1", because the wrong guard firing would hide
# the fact that the right one is missing.
mkdir -p "$D/bad"
python3 "$R/t/oracle.py" mkbad "$D/bad" > "$D/badlist"
while read -r name msg; do
	"$IN" "$D/bad/$name.png" > "$D/out" 2>"$D/err"
	rc=$?
	if [ "$rc" != 1 ]; then
		bad "$name: exit $rc, not a clean rejection"
	elif [ -s "$D/out" ]; then
		bad "$name: rejected but wrote to stdout"
	elif ! grep -qi "$msg" "$D/err"; then
		bad "$name: rejected for the wrong reason: $(cat "$D/err")"
	else
		ok
	fi
done < "$D/badlist"

printf '\nPngSuite (%s): %d/%d valid decoded, %d/%d corrupt rejected\n' \
	"$MODE" "$nvalid" "$WANT_VALID" "$ncorrupt" "$WANT_CORRUPT"
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
