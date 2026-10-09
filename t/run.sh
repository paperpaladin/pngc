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
GOT_VALID=$WANT_VALID
GOT_CORRUPT=9   # bad bit depth and bad colour type are not caught yet

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
	*)	if [ "$rc" = 0 ]; then nvalid=$((nvalid+1))
		else bad "$b: valid file rejected ($(cat "$D/err"))"; fi
		;;
	esac
done

if [ "$nvalid" = "$GOT_VALID" ]; then ok
else bad "decoded $nvalid valid files, expected $GOT_VALID"; fi
if [ "$ncorrupt" = "$GOT_CORRUPT" ]; then ok
else bad "rejected $ncorrupt corrupt files, expected $GOT_CORRUPT"; fi

printf '\nPngSuite (%s): %d/%d valid decoded, %d/%d corrupt rejected\n' \
	"$MODE" "$nvalid" "$WANT_VALID" "$ncorrupt" "$WANT_CORRUPT"
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
