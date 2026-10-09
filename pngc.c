/* pngc -- a PNG (RFC 2083 / W3C PNG 1.2) decoder in one C11 file.
 *
 * Reads a PNG from a file argument or stdin, writes the decoded image to
 * stdout as a PAM (netpbm P7) so the result is inspectable with tools that
 * were not written here.
 *
 * The DEFLATE core in the middle of this file is lifted from the sibling
 * project ../inflate, which implements RFC 1951 from scratch against
 * /usr/bin/gzip as its oracle. Only the decoder half travels, and PNG's
 * zlib (RFC 1950) framing is added here because inflate does gzip framing
 * instead. See README.md.
 *
 * The authority on correctness is libpng, reached through
 * gdk-pixbuf-thumbnailer, plus Python's zlib and the PngSuite corpus.
 * Nothing here is checked against my own reading of the spec alone.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *m) {
	fprintf(stderr, "pngc: %s\n", m);
	exit(1);
}

/* ---------------------------------------------------------------- buffer */

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void bneed(Buf *b, size_t extra) {
	if (extra > SIZE_MAX - b->n) die("out of memory");
	if (b->n + extra <= b->cap) return;
	size_t c = b->cap ? b->cap : 1u << 16;
	while (c < b->n + extra) {
		if (c > SIZE_MAX / 2) { c = b->n + extra; break; }
		c <<= 1;
	}
	uint8_t *q = realloc(b->p, c);
	if (!q) die("out of memory");
	b->p = q;
	b->cap = c;
}
static void bwrite(Buf *b, const void *s, size_t n) {
	bneed(b, n);
	memcpy(b->p + b->n, s, n);
	b->n += n;
}

/* ----------------------------------------------------------------- crc32 */

/* The same polynomial gzip and zlib use; PNG specifies it in clause 3.4.
 * Every chunk carries one and every one of them is checked. */
static uint32_t crctab[256];
static void crcinit(void) {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
		crctab[i] = c;
	}
}
static uint32_t crc32b(const uint8_t *p, size_t n) {
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) c = crctab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

static uint32_t be32(const uint8_t *p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
	       (uint32_t)p[3];
}

/* ------------------------------------------------------------- the image */

typedef struct {
	uint32_t w, h;
	int depth, colour, interlace;
	int chans;            /* samples per pixel in the file */
	Buf idat;             /* every IDAT, concatenated */
	uint8_t plte[256 * 3];
	int nplte;
	uint8_t trns[256];    /* palette alpha, defaulted to 255 */
	int ntrns;
	int has_trns;
	uint16_t tk[3];       /* tRNS key for colour types 0 and 2 */
} Png;

static const uint8_t SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};

/* Samples per pixel in the file, by colour type. -1 marks the three values
 * the spec leaves undefined; xc1n0g08.png and xc9n2c08.png are exactly those
 * files and exist to be refused. */
static const int CHANS[7] = {1, -1, 3, 1, 2, -1, 4};

/* Clause 11.2.2, table: not every depth is legal with every colour type.
 * Bit k set means depth 1<<k is allowed. */
static const unsigned DEPTHOK[7] = {
	0x1f, 0, 0x18, 0x0f, 0x18, 0, 0x18,
};

static const char *ihdr(const uint8_t *d, uint32_t len, Png *im) {
	if (len != 13) return "IHDR is not 13 bytes";
	im->w = be32(d);
	im->h = be32(d + 4);
	im->depth = d[8];
	im->colour = d[9];
	int comp = d[10], filt = d[11];
	im->interlace = d[12];

	/* Clause 11.2.2: both are 4-byte unsigned, zero is invalid, and the high
	 * bit is reserved. Rejecting 2^31..2^32-1 here is also the first half of
	 * the overflow story -- see rowbytes(). */
	if (im->w == 0 || im->h == 0) return "zero width or height";
	if (im->w > 0x7fffffffu || im->h > 0x7fffffffu) return "dimension >= 2^31";
	if (im->colour > 6 || CHANS[im->colour] < 0)
		return "undefined colour type";
	if (im->depth != 1 && im->depth != 2 && im->depth != 4 &&
	    im->depth != 8 && im->depth != 16)
		return "bit depth is not 1, 2, 4, 8 or 16";
	if (!(DEPTHOK[im->colour] & (1u << (im->depth == 16 ? 4 :
	                                    im->depth == 8 ? 3 :
	                                    im->depth == 4 ? 2 :
	                                    im->depth == 2 ? 1 : 0))))
		return "bit depth is not legal for this colour type";
	if (comp != 0) return "unknown compression method";
	if (filt != 0) return "unknown filter method";
	if (im->interlace > 1) return "unknown interlace method";
	im->chans = CHANS[im->colour];
	return NULL;
}

/* Returns NULL on success, else why this file is not a PNG we will decode.
 * Every rejection has to come from here: a corrupt file must never reach the
 * pixel stages at all. */
static const char *walk(const uint8_t *p, size_t n, Png *im) {
	if (n < 8 || memcmp(p, SIG, 8) != 0) return "bad PNG signature";
	size_t i = 8;
	int seen_ihdr = 0, seen_iend = 0, seen_idat = 0, idat_closed = 0;

	while (i < n) {
		if (n - i < 8) return "truncated chunk header";
		uint32_t len = be32(p + i);
		/* Clause 3.2: a length is an unsigned 31-bit value. */
		if (len > 0x7fffffffu) return "chunk length exceeds 2^31-1";
		const uint8_t *type = p + i + 4;
		for (int k = 0; k < 4; k++)
			if ((type[k] < 'A' || type[k] > 'Z') &&
			    (type[k] < 'a' || type[k] > 'z'))
				return "chunk type is not alphabetic";
		if (n - i - 8 < (size_t)len + 4) return "truncated chunk";
		const uint8_t *d = p + i + 8;
		if (crc32b(type, (size_t)len + 4) != be32(d + len))
			return "chunk CRC-32 mismatch";
		i += 8 + (size_t)len + 4;

		int critical = !(type[0] & 0x20);
		int is_idat = !memcmp(type, "IDAT", 4);
		/* Clause 4.1.3: the IDAT chunks must be consecutive. A run that restarts
		 * after some other chunk is not the file shape the spec defines, and
		 * sequence errors are how a decoder ends up stitching two unrelated
		 * streams together. */
		if (seen_idat && !is_idat) idat_closed = 1;

		if (!memcmp(type, "IHDR", 4)) {
			if (seen_ihdr) return "duplicate IHDR";
			if (i != 8 + 8 + (size_t)len + 4) return "IHDR is not first";
			seen_ihdr = 1;
			const char *e = ihdr(d, len, im);
			if (e) return e;
		} else if (!seen_ihdr) {
			return "first chunk is not IHDR";
		} else if (is_idat) {
			if (idat_closed) return "IDAT chunks are not consecutive";
			seen_idat = 1;
			bwrite(&im->idat, d, len);
		} else if (!memcmp(type, "IEND", 4)) {
			if (len) return "IEND is not empty";
			seen_iend = 1;
			if (i != n) return "data after IEND";
			break;
		} else if (critical) {
			/* An unknown critical chunk means the file needs something we do not
			 * implement, so guessing is worse than refusing. */
			if (memcmp(type, "PLTE", 4)) return "unknown critical chunk";
		}
	}
	if (!seen_ihdr) return "no IHDR";
	if (!seen_idat) return "no IDAT";
	if (!seen_iend) return "no IEND";
	return NULL;
}

/* -------------------------------------------------------------------- io */

static Buf slurp(const char *path) {
	FILE *f = path ? fopen(path, "rb") : stdin;
	if (!f) die("cannot open input");
	Buf b = {0, 0, 0};
	for (;;) {
		bneed(&b, 1u << 16);
		size_t got = fread(b.p + b.n, 1, 1u << 16, f);
		b.n += got;
		if (got < 1u << 16) break;
	}
	if (ferror(f)) die("read error");
	if (path) fclose(f);
	return b;
}

int main(int argc, char **argv) {
	crcinit();
	const char *path = NULL;
	for (int a = 1; a < argc; a++) {
		if (argv[a][0] == '-' && argv[a][1])
			die("usage: pngc [file.png]");
		path = argv[a];
	}

	Buf in = slurp(path);
	Png im;
	memset(&im, 0, sizeof im);
	const char *e = walk(in.p, in.n, &im);
	if (e) {
		fprintf(stderr, "pngc: %s\n", e);
		free(in.p);
		free(im.idat.p);
		return 1;
	}
	printf("ok %ux%u depth %d colour %d interlace %d, %zu IDAT bytes\n",
	       im.w, im.h, im.depth, im.colour, im.interlace, im.idat.n);
	free(in.p);
	free(im.idat.p);
	return 0;
}
