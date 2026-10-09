CC      = gcc
# -ffile-prefix-map keeps the build directory out of DWARF, so the binary is
# bit-identical wherever it is built. Checked by ../unrepro.
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic -ffile-prefix-map=$(CURDIR)=.

pngc: pngc.c
	$(CC) $(CFLAGS) -o $@ $<

test: pngc
	t/run.sh ./pngc

# This decoder parses attacker-chosen binary with attacker-chosen dimensions,
# which is the exact shape of problem that produces image-decoder CVEs, so the
# whole suite has to pass again with the sanitizers on.
#
# exitcode=86 is load-bearing: a sanitizer abort exits 1 by default, and
# t/run.sh asserts "exit 1" to mean "cleanly rejected this file", so without it
# test-asan could not fail. UBSan needs halt_on_error or it only prints.
SANOPT = ASAN_OPTIONS=exitcode=86 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86

test-asan: pngc.c
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-Wall -Wextra -Wpedantic -ffile-prefix-map=$(CURDIR)=. -o pngc-asan pngc.c
	$(SANOPT) t/run.sh ./pngc-asan

clean:
	rm -f pngc pngc-asan
	rm -rf t/tmp

.PHONY: test test-asan clean
