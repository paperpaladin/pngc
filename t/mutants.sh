#!/bin/sh
# Plant each defect, rebuild, and require the suite to go red.
#
# This sweep used to live in a shell history, which is the same as not
# existing: a suite only occasionally checked for being ABLE to fail
# will eventually stop being able to, and nobody will notice, because
# the way that failure presents is everything staying green.
#
# Each entry is a sed expression over a copy of pngc.c. The copy is
# built and the real suite is run against it, so a mutant is caught by
# whatever would have caught it in ordinary use, not by a special
# assertion written to catch that mutant.
#
# One mutant is EXPECTED to survive and is listed as such. It is
# equivalent, not uncaught: swapping the first two Paeth branches to
# prefer b on a pa == pb tie cannot change any output, brute-forced
# over all 16777216 triples. An expected survivor that starts dying
# is as interesting as an expected casualty that starts living, so
# both directions are failures here.
set -u
cd "$(dirname "$0")/.."
R=$(pwd)

D=$(mktemp -d) || exit 2
trap 'rm -rf "$D"' EXIT INT TERM

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"
        [ $# -gt 1 ] && printf '        %s\n' "$2"; return 0; }

# name|expectation|sed expression
#   dies     the suite must go red
#   survives the suite must stay green, and the reason is in the README
MUTANTS="
paeth computed in a byte|dies|s/int p = (int)a + (int)b - (int)c;/int p = (unsigned char)(a + b - c);/
paeth returns a too eagerly|dies|s/if (pa <= pb \&\& pa <= pc) return a;/if (pa <= pc) return a;/
paeth prefers c over b|dies|s/if (pb <= pc) return b;/if (pb < pc) return b;/
paeth prefers b on a tie|survives|s/if (pa <= pb \&\& pa <= pc) return a;\$/if (pa < pb \&\& pa <= pc) return a;/
16-bit low byte dropped|dies|s/if (depth == 16) return (unsigned)row\[2 \* j\] << 8 | row\[2 \* j + 1\];/if (depth == 16) return (unsigned)row[2 * j] << 8;/
16-bit bytes read little-endian|dies|s/if (depth == 16) return (unsigned)row\[2 \* j\] << 8 | row\[2 \* j + 1\];/if (depth == 16) return (unsigned)row[2 * j + 1] << 8 | row[2 * j];/
chunk CRC not checked|dies|s/if (crc32b(type, (size_t)len + 4) != be32(d + len))/if (0)/
average filter rounds up|dies|s|case 3: x += (a + b) >> 1;|case 3: x += (a + b + 1) >> 1;|
up filter reads the current row|dies|s/case 2: x += b; break;/case 2: x += a; break;/
sub-bit samples not masked|dies|s/return (row\[j \/ per\] >> shift) \& ((1u << depth) - 1u);/return (unsigned)(row[j \/ per] >> shift);/
"

run_one() {
	name=$1 expect=$2 expr=$3
	sed "$expr" "$R/pngc.c" > "$D/m.c"
	if cmp -s "$R/pngc.c" "$D/m.c"; then
		bad "planted: $name" "the sed matched nothing, so nothing was tested"
		return
	fi
	if ! $CC $CFLAGS -o "$D/m" "$D/m.c" 2>"$D/cc"; then
		bad "planted: $name" "the mutant does not compile: $(head -1 "$D/cc")"
		return
	fi
	if "$R/t/run.sh" "$D/m" >"$D/out" 2>&1; then
		got=survives
	else
		got=dies
	fi
	if [ "$got" = "$expect" ]; then
		if [ "$expect" = survives ]; then
			ok "$name survives, as the README says it must"
		else
			ok "$name is caught"
		fi
	elif [ "$expect" = dies ]; then
		bad "$name is caught" "the suite stayed green with this defect in"
	else
		bad "$name survives, as the README says it must" \
		    "it died, so either the suite grew a case for it or the \
equivalence argument in the README is wrong"
	fi
}

: "${CC:=gcc}"
: "${CFLAGS:=-std=c11 -O2 -w}"

printf '%s\n' "$MUTANTS" | while IFS='|' read -r name expect expr; do
	[ -n "$name" ] || continue
	run_one "$name" "$expect" "$expr"
done > "$D/log" 2>&1
cat "$D/log"
pass=$(grep -c '^ok' "$D/log" || true)
fail=$(grep -c '^FAIL' "$D/log" || true)

printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
