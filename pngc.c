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


/* =================================================== DEFLATE (RFC 1951) ==
 *
 * Lifted from the sibling project ../inflate, where this same code is tested
 * against /usr/bin/gzip at every compression level, over eleven fixtures,
 * plus nine hand-built streams that each violate exactly one rule of RFC 1951
 * and 300 randomly mutated ones. Reimplementing it here would have duplicated
 * that project's scope and thrown away its oracle, so the decoder travels and
 * the encoder does not. The one thing inflate does not have is zlib (RFC
 * 1950) framing -- it does gzip (RFC 1952) instead -- so zinflate() below is
 * new, and so is Adler-32.
 */

typedef struct { const uint8_t *p; size_t n, i; uint32_t acc; int cnt; } Br;

/* DEFLATE packs bits LSB-first within each byte. need <= 16, so acc never
 * holds more than 23 bits and cannot overflow. */
static int rbits(Br *b, int need, uint32_t *out) {
	while (b->cnt < need) {
		if (b->i >= b->n) return -1;
		b->acc |= (uint32_t)b->p[b->i++] << b->cnt;
		b->cnt += 8;
	}
	*out = b->acc & ((1u << need) - 1);
	b->acc >>= need;
	b->cnt -= need;
	return 0;
}

static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
	35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,
	4,4,5,5,5,5,0};
static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
	257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,
	10,10,11,11,12,12,13,13};
static const uint8_t clord[19] =
	{16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

/* Canonical Huffman as counts-plus-sorted-symbols, walked one bit at a time.
 * No lookup table: fifteen lines, and PNG IDATs are small. */
typedef struct { uint16_t count[16]; uint16_t sym[288]; } Huf;

/* <0 over-subscribed, 0 complete, >0 incomplete. */
static int hbuild(Huf *h, const uint8_t *len, int n) {
	memset(h->count, 0, sizeof h->count);
	for (int i = 0; i < n; i++) h->count[len[i]]++;
	if (h->count[0] == n) return 0; /* empty tree: legal while never used */
	int left = 1;
	for (int l = 1; l < 16; l++) {
		left <<= 1;
		left -= h->count[l];
		if (left < 0) return -1;
	}
	uint16_t off[16];
	off[1] = 0;
	for (int l = 1; l < 15; l++) off[l + 1] = off[l] + h->count[l];
	for (int i = 0; i < n; i++) if (len[i]) h->sym[off[len[i]]++] = (uint16_t)i;
	return left;
}

static int hdec(Br *b, const Huf *h) {
	int code = 0, first = 0, index = 0;
	for (int l = 1; l < 16; l++) {
		uint32_t bit;
		if (rbits(b, 1, &bit)) return -1;
		code |= (int)bit;
		int c = h->count[l];
		if (code - first < c) return h->sym[index + (code - first)];
		index += c;
		first = (first + c) << 1;
		code <<= 1;
	}
	return -1;
}

static Huf fixlit, fixdist;
static void fixinit(void) {
	uint8_t l[288];
	for (int i = 0; i < 144; i++) l[i] = 8;
	for (int i = 144; i < 256; i++) l[i] = 9;
	for (int i = 256; i < 280; i++) l[i] = 7;
	for (int i = 280; i < 288; i++) l[i] = 8;
	hbuild(&fixlit, l, 288);
	/* 32 codes, not 30: RFC 1951 3.2.6 defines the unusable 30 and 31 so the
	 * tree comes out complete. Dropping them changes which patterns decode. */
	for (int i = 0; i < 32; i++) l[i] = 5;
	hbuild(&fixdist, l, 32);
}

static const char *inflate_raw(Br *b, Buf *o) {
	uint32_t v, final, type;

	for (;;) {
		if (rbits(b, 1, &final) || rbits(b, 2, &type))
			return "truncated: block header";
		if (type == 3) return "invalid block type 3";

		if (type == 0) {
			b->acc = 0; /* stored blocks resume at the next byte boundary */
			b->cnt = 0;
			if (b->n - b->i < 4) return "truncated: stored header";
			unsigned len = b->p[b->i] | (unsigned)b->p[b->i + 1] << 8;
			unsigned nlen = b->p[b->i + 2] | (unsigned)b->p[b->i + 3] << 8;
			b->i += 4;
			if ((len ^ 0xffffu) != nlen) return "stored block LEN/NLEN mismatch";
			if (b->n - b->i < len) return "truncated: stored data";
			bwrite(o, b->p + b->i, len);
			b->i += len;
		} else {
			Huf dynl, dynd;
			const Huf *lt = &fixlit, *dt = &fixdist;

			if (type == 2) {
				uint32_t hlit, hdist, hclen;
				if (rbits(b, 5, &hlit) || rbits(b, 5, &hdist) ||
				    rbits(b, 4, &hclen))
					return "truncated: dynamic header";
				hlit += 257; hdist += 1; hclen += 4;

				uint8_t cl[19];
				memset(cl, 0, sizeof cl);
				for (uint32_t i = 0; i < hclen; i++) {
					if (rbits(b, 3, &v)) return "truncated: code lengths";
					cl[clord[i]] = (uint8_t)v;
				}
				Huf clh;
				if (hbuild(&clh, cl, 19) < 0)
					return "over-subscribed code-length tree";

				uint8_t lens[288 + 32];
				memset(lens, 0, sizeof lens);
				uint32_t i = 0;
				while (i < hlit + hdist) {
					int sy = hdec(b, &clh);
					if (sy < 0) return "bad code-length symbol";
					if (sy < 16) { lens[i++] = (uint8_t)sy; continue; }
					uint8_t val = 0;
					uint32_t rep;
					if (sy == 16) {
						if (i == 0) return "repeat with no previous length";
						val = lens[i - 1];
						if (rbits(b, 2, &rep)) return "truncated: repeat";
						rep += 3;
					} else if (sy == 17) {
						if (rbits(b, 3, &rep)) return "truncated: repeat";
						rep += 3;
					} else {
						if (rbits(b, 7, &rep)) return "truncated: repeat";
						rep += 11;
					}
					if (rep > hlit + hdist - i)
						return "code-length repeat overruns the alphabet";
					while (rep--) lens[i++] = val;
				}
				if (!lens[256]) return "no end-of-block code";
				if (hbuild(&dynl, lens, (int)hlit) < 0)
					return "over-subscribed literal/length tree";
				if (hbuild(&dynd, lens + hlit, (int)hdist) < 0)
					return "over-subscribed distance tree";
				lt = &dynl;
				dt = &dynd;
			}

			for (;;) {
				int sy = hdec(b, lt);
				if (sy < 0) return "bad literal/length code";
				if (sy < 256) { bneed(o, 1); o->p[o->n++] = (uint8_t)sy; continue; }
				if (sy == 256) break;
				sy -= 257;
				if (sy >= 29) return "invalid length code (286/287)";
				if (rbits(b, lext[sy], &v)) return "truncated: length extra bits";
				unsigned length = lbase[sy] + v;

				int dd = hdec(b, dt);
				if (dd < 0) return "bad distance code";
				if (dd >= 30) return "invalid distance code (30/31)";
				if (rbits(b, dext[dd], &v))
					return "truncated: distance extra bits";
				size_t dist = dbase[dd] + v;
				if (dist > o->n) return "distance reaches before output start";

				bneed(o, length);
				size_t from = o->n - dist;
				/* Byte at a time, deliberately: dist < length is the RLE case
				 * DEFLATE relies on, where source and destination overlap and
				 * both memcpy and memmove give the wrong answer. */
				for (unsigned k = 0; k < length; k++)
					o->p[o->n + k] = o->p[from + k];
				o->n += length;
			}
		}
		if (final) return NULL;
	}
}

/* ------------------------------------------------------ zlib (RFC 1950) */

static uint32_t adler32(const uint8_t *p, size_t n) {
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < n; i++) {
		a += p[i];
		if (a >= 65521) a -= 65521;
		b += a;
		if (b >= 65521) b -= 65521;
	}
	return b << 16 | a;
}

/* PNG clause 10.3 pins this harder than RFC 1950 does: method 8, window size
 * at most 32768, and no preset dictionary. */
static const char *zinflate(const uint8_t *p, size_t n, Buf *o) {
	if (n < 2) return "zlib stream shorter than its header";
	if ((p[0] & 0x0f) != 8) return "zlib compression method is not deflate";
	if ((p[0] >> 4) > 7) return "zlib window size above 32768";
	if (p[1] & 0x20) return "zlib preset dictionary is not allowed in PNG";
	if (((unsigned)p[0] << 8 | p[1]) % 31) return "zlib header check failed";

	Br b = {p, n, 2, 0, 0};
	const char *e = inflate_raw(&b, o);
	if (e) return e;
	b.acc = 0;
	b.cnt = 0;
	if (b.n - b.i < 4) return "truncated zlib Adler-32";
	if (adler32(o->p, o->n) != be32(b.p + b.i)) return "zlib Adler-32 mismatch";
	/* Clause 10.3 says the zlib stream ends with IDAT; junk past the Adler-32
	 * means the file is not what it claims. */
	if (b.n - b.i != 4) return "trailing bytes after the zlib stream";
	return NULL;
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


/* ------------------------------------------------------- size arithmetic */

/* Adam7, clause 8.2. Seven passes, each with its own origin and stride, so
 * each has its own width, height and -- the part decoders get wrong -- its own
 * per-scanline filter byte. A pass can be legitimately empty: a 1-pixel-wide
 * image puts nothing at all in passes 2, 4 and 6.
 */
static const uint32_t A7X0[7] = {0, 4, 0, 2, 0, 1, 0};
static const uint32_t A7Y0[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint32_t A7XS[7] = {8, 8, 4, 4, 2, 2, 1};
static const uint32_t A7YS[7] = {8, 8, 8, 4, 4, 2, 2};

static uint32_t passdim(uint32_t n, uint32_t start, uint32_t step) {
	return n > start ? (n - start + step - 1) / step : 0;
}

/* Bytes in one scanline, excluding its filter byte. 64-bit on purpose: w is
 * under 2^31, chans at most 4 and depth at most 16, so the bit count is at
 * most 2^37 and cannot wrap here. It very much can wrap once multiplied by a
 * height, which is what the caller has to guard. */
static uint64_t rowbits(uint32_t w, int depth, int chans) {
	return (uint64_t)w * (unsigned)chans * (unsigned)depth;
}
static uint64_t rowbytes(uint32_t w, int depth, int chans) {
	return (rowbits(w, depth, chans) + 7) / 8;
}

/* shortcut: a flat 1 GiB ceiling on decoded pixel data, so that a header
 * claiming 2^31 x 2^31 is refused by policy rather than by whatever malloc
 * happens to do. Make it a -m flag if anyone needs a bigger image than this.
 */
#define MAXPIX (1024ull * 1024 * 1024)

/* Exactly how many bytes the inflated IDAT stream must contain: for every
 * non-empty pass, one filter byte plus one scanline, per row.
 *
 * The multiply below is THE image-decoder integer overflow. rowbytes can reach
 * 2^34 and height 2^31, whose product needs 65 bits, so on a 64-bit machine
 * the unguarded form wraps to a small number -- and a small number is exactly
 * what makes it dangerous, because the allocation then succeeds and the
 * scanline loop walks off the end of it. t/run.sh crafts an IHDR that lands on
 * that wrap deliberately.
 */
static const char *expected_raw(const Png *im, uint64_t *out) {
	uint64_t total = 0;
	int lo = im->interlace ? 0 : 6, hi = 7;
	for (int p = lo; p < hi; p++) {
		uint32_t pw = im->w, ph = im->h;
		if (im->interlace) {
			pw = passdim(im->w, A7X0[p], A7XS[p]);
			ph = passdim(im->h, A7Y0[p], A7YS[p]);
		}
		if (!pw || !ph) continue;
		uint64_t rb = rowbytes(pw, im->depth, im->chans) + 1;
		if (ph > (UINT64_MAX - total) / rb)
			return "scanline total overflows 64 bits";
		total += rb * ph;
	}
	*out = total;
	return NULL;
}

/* Bytes of unpacked, unfiltered scanline data for the whole image, which is
 * what gets allocated. Same overflow, second place to make it. */
static const char *image_bytes(const Png *im, uint64_t *out) {
	uint64_t rb = rowbytes(im->w, im->depth, im->chans);
	if (im->h > UINT64_MAX / (rb ? rb : 1)) return "image dimensions overflow";
	uint64_t n = rb * im->h;
	if (n > MAXPIX || n > SIZE_MAX) return "image larger than the 1 GiB limit";
	*out = n;
	return NULL;
}


/* ----------------------------------------------------- scanline filters */

/* Clause 6: every scanline carries a filter-type byte, and the predictor works
 * on bytes a whole pixel apart -- or on adjacent bytes when a pixel is smaller
 * than a byte, which is what the floor at 1 is for.
 *
 *	a = the byte bpp to the left, 0 off the left edge
 *	b = the byte directly above, 0 on the first row
 *	c = the byte above and bpp to the left, 0 off either edge
 */
/* Clause 6.6, written as the spec writes it. The most commonly miscoded line
 * in PNG: the tie-breaking is not symmetric, the comparisons are <= and not <,
 * and the order a-then-b-then-c is normative. p can be negative and can exceed
 * 255, so the arithmetic has to be signed and wider than a byte -- doing it in
 * uint8_t is the classic wrong version that still decodes most images. */
static unsigned paeth(unsigned a, unsigned b, unsigned c) {
	int p = (int)a + (int)b - (int)c;
	int pa = p > (int)a ? p - (int)a : (int)a - p;
	int pb = p > (int)b ? p - (int)b : (int)b - p;
	int pc = p > (int)c ? p - (int)c : (int)c - p;
	if (pa <= pb && pa <= pc) return a;
	if (pb <= pc) return b;
	return c;
}

static size_t filtbpp(int depth, int chans) {
	int bits = depth * chans;
	return bits >= 8 ? (size_t)(bits / 8) : 1;
}

/* Reverse the filters of one pass -- or of a whole non-interlaced image, which
 * is the same thing with one pass. src holds rows*(rb+1) bytes, dst receives
 * rows*rb. Reconstruction reads dst's own previous row, so the loop order is
 * not an implementation detail. */
static const char *unfilter(const uint8_t *src, uint8_t *dst, uint32_t rows,
                            size_t rb, size_t bpp) {
	for (uint32_t y = 0; y < rows; y++) {
		int f = *src++;
		if (f > 4) return "unknown scanline filter type";
		const uint8_t *prev = y ? dst + (size_t)(y - 1) * rb : NULL;
		uint8_t *cur = dst + (size_t)y * rb;
		for (size_t i = 0; i < rb; i++) {
			unsigned a = i >= bpp ? cur[i - bpp] : 0;
			unsigned b = prev ? prev[i] : 0;
			unsigned c = (prev && i >= bpp) ? prev[i - bpp] : 0;
			unsigned x = src[i];
			switch (f) {
			case 1: x += a; break;
			case 2: x += b; break;
			case 3: x += (a + b) >> 1; break;  /* floor, clause 6.4 */
			case 4: x += paeth(a, b, c); break;
			default: break;                    /* 0, None */
			}
			cur[i] = (uint8_t)x;
		}
		src += rb;
	}
	return NULL;
}

/* Unfilter every pass in order, concatenated. For a non-interlaced image that
 * is one pass and the result is the image; for Adam7 it is seven pass images
 * that still have to be scattered into place. */
static const char *unfilter_all(const Png *im, const Buf *raw, Buf *out) {
	size_t bpp = filtbpp(im->depth, im->chans);
	const uint8_t *src = raw->p;
	int lo = im->interlace ? 0 : 6;
	for (int p = lo; p < 7; p++) {
		uint32_t pw = im->w, ph = im->h;
		if (im->interlace) {
			pw = passdim(im->w, A7X0[p], A7XS[p]);
			ph = passdim(im->h, A7Y0[p], A7YS[p]);
		}
		if (!pw || !ph) continue;
		size_t rb = (size_t)rowbytes(pw, im->depth, im->chans);
		bneed(out, rb * (size_t)ph);
		const char *e = unfilter(src, out->p + out->n, ph, rb, bpp);
		if (e) return e;
		out->n += rb * (size_t)ph;
		src += (rb + 1) * (size_t)ph;
	}
	return NULL;
}


/* -------------------------------------------------------- pixel output */

/* Extract sample j of a scanline. Three cases, and the middle one is the only
 * one most decoders get tested on:
 *
 *   depth 16: big-endian, clause 7.1. Network byte order, not the machine's.
 *   depth 8:  one byte.
 *   depth 1, 2, 4: packed left to right, most significant bits first, so the
 *             last byte of a scanline can be partially unused -- and those
 *             spare low bits are not required to be zero, so they must be
 *             masked off rather than trusted.
 */
static unsigned getsample(const uint8_t *row, size_t j, int depth) {
	if (depth == 16) return (unsigned)row[2 * j] << 8 | row[2 * j + 1];
	if (depth == 8) return row[j];
	size_t per = (size_t)(8 / depth);
	unsigned shift = (unsigned)(8 - depth) - (unsigned)(j % per) * (unsigned)depth;
	return (row[j / per] >> shift) & ((1u << depth) - 1u);
}

/* Scale a sample to 8 bits the way the spec's "sample depth scaling" does:
 * v * 255 / max, which for depths 1, 2 and 4 is exactly bit replication
 * (x255, x85, x17) and is what libpng's expand produces. */
static unsigned to8(unsigned v, int depth) {
	return depth == 8 ? v : v * 255u / ((1u << depth) - 1u);
}

/* Bytes of RGBA output, guarded. This is bigger than the packed input, by up
 * to 32x for 1-bit greyscale, so it needs its own ceiling rather than
 * inheriting the one on the packed size. */
static const char *out_bytes(const Png *im, uint64_t *out) {
	uint64_t per = 4 * (im->depth == 16 ? 2u : 1u);
	uint64_t px = (uint64_t)im->w * im->h;	/* under 2^62, cannot wrap */
	if (px > UINT64_MAX / per) return "RGBA size overflows 64 bits";
	uint64_t n = px * per;
	if (n > MAXPIX || n > SIZE_MAX)
		return "RGBA image larger than the 1 GiB limit";
	*out = n;
	return NULL;
}

/* One pixel of the source, as RGBA at the source's own sample depth. */
static void topixel(const Png *im, const uint8_t *row, size_t k,
                    unsigned px[4]) {
	unsigned full = (1u << im->depth) - 1u;
	switch (im->colour) {
	case 3:
		/* Palette lookup lands in the next commit. Until then say so rather
		 * than fall through into the truecolour case, which would read four
		 * samples out of a one-sample-per-pixel scanline. */
		px[0] = px[1] = px[2] = 0;
		px[3] = full;
		break;
	case 0:
		px[0] = px[1] = px[2] = getsample(row, k, im->depth);
		px[3] = full;
		break;
	case 2:
		px[0] = getsample(row, k * 3 + 0, im->depth);
		px[1] = getsample(row, k * 3 + 1, im->depth);
		px[2] = getsample(row, k * 3 + 2, im->depth);
		px[3] = full;
		break;
	case 4:
		px[0] = px[1] = px[2] = getsample(row, k * 2 + 0, im->depth);
		px[3] = getsample(row, k * 2 + 1, im->depth);
		break;
	default: /* 6 */
		px[0] = getsample(row, k * 4 + 0, im->depth);
		px[1] = getsample(row, k * 4 + 1, im->depth);
		px[2] = getsample(row, k * 4 + 2, im->depth);
		px[3] = getsample(row, k * 4 + 3, im->depth);
		break;
	}
}

/* Walk the unfiltered pass images and place every pixel at its final
 * position. For a non-interlaced image there is one pass and the placement is
 * the identity; for Adam7 the seven passes each land on their own lattice,
 * which is the only reason this is not a memcpy. */
static const char *render(const Png *im, const Buf *img, uint8_t *out) {
	if (im->colour == 3) return "palette images are not implemented yet";
	int wide = im->depth == 16;
	size_t per = wide ? 8 : 4;
	size_t off = 0;
	int lo = im->interlace ? 0 : 6;

	for (int p = lo; p < 7; p++) {
		uint32_t x0 = 0, y0 = 0, xs = 1, ys = 1, pw = im->w, ph = im->h;
		if (im->interlace) {
			x0 = A7X0[p]; y0 = A7Y0[p]; xs = A7XS[p]; ys = A7YS[p];
			pw = passdim(im->w, x0, xs);
			ph = passdim(im->h, y0, ys);
		}
		if (!pw || !ph) continue;
		size_t rb = (size_t)rowbytes(pw, im->depth, im->chans);
		for (uint32_t y = 0; y < ph; y++) {
			const uint8_t *row = img->p + off + (size_t)y * rb;
			uint8_t *dst = out + ((size_t)(y0 + y * ys) * im->w) * per;
			for (uint32_t k = 0; k < pw; k++) {
				unsigned px[4];
				topixel(im, row, k, px);
				uint8_t *q = dst + (size_t)(x0 + k * xs) * per;
				for (int ch = 0; ch < 4; ch++) {
					if (wide) {
						q[2 * ch] = (uint8_t)(px[ch] >> 8);
						q[2 * ch + 1] = (uint8_t)px[ch];
					} else {
						q[ch] = (uint8_t)to8(px[ch], im->depth);
					}
				}
			}
		}
		off += rb * (size_t)ph;
	}
	return NULL;
}

/* PAM (netpbm P7), because it is the only one of the netpbm formats that can
 * carry four channels and 16-bit samples, which is what PNG can carry. Its
 * sample order is big-endian, same as PNG's. */
static void writepam(const Png *im, const uint8_t *out, size_t n) {
	printf("P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL %u\n"
	       "TUPLTYPE RGB_ALPHA\nENDHDR\n",
	       im->w, im->h, im->depth == 16 ? 65535u : 255u);
	if (n && fwrite(out, 1, n, stdout) != n) die("write error");
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
	fixinit();
	const char *path = NULL;
	int zdump = 0, rdump = 0;
	for (int a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-z")) { zdump = 1; continue; }
		if (!strcmp(argv[a], "-r")) { rdump = 1; continue; }
		if (argv[a][0] == '-' && argv[a][1])
			die("usage: pngc [-z|-r] [file.png]");
		path = argv[a];
	}

	Buf in = slurp(path);
	Png im;
	memset(&im, 0, sizeof im);
	Buf raw = {0, 0, 0};
	uint64_t want = 0, npix = 0;
	(void)npix;
	const char *e = walk(in.p, in.n, &im);
	if (!e) e = expected_raw(&im, &want);
	if (!e) e = image_bytes(&im, &npix);
	if (!e) e = zinflate(im.idat.p, im.idat.n, &raw);
	/* The stream has to be exactly the right length. Too short and some
	 * scanline is missing; too long and the file is not describing the image
	 * its own header describes. Either way, guessing is how a decoder ends up
	 * reading past the end of what it inflated. */
	if (!e && raw.n != want) e = "inflated IDAT is not the size IHDR implies";
	if (e) {
		fprintf(stderr, "pngc: %s\n", e);
		free(in.p);
		free(im.idat.p);
		free(raw.p);
		return 1;
	}
	Buf img = {0, 0, 0};
	if (rdump) {
		/* Stage two, handed out before any colour conversion: the filters
		 * reversed and nothing else. t/oracle.py knows what these bytes
		 * should be because it filtered them forwards. */
		e = unfilter_all(&im, &raw, &img);
		if (e) {
			fprintf(stderr, "pngc: %s\n", e);
			free(in.p); free(im.idat.p); free(raw.p); free(img.p);
			return 1;
		}
		if (img.n && fwrite(img.p, 1, img.n, stdout) != img.n)
			die("write error");
	} else if (zdump) {
		/* The inflate half, handed out unfiltered so Python's zlib can be
		 * asked whether it agrees before any pixel code is in the way. */
		if (raw.n && fwrite(raw.p, 1, raw.n, stdout) != raw.n)
			die("write error");
	} else {
		uint64_t nout = 0;
		e = out_bytes(&im, &nout);
		if (!e) e = unfilter_all(&im, &raw, &img);
		uint8_t *rgba = NULL;
		if (!e) {
			rgba = calloc((size_t)nout, 1);
			if (!rgba) die("out of memory");
			e = render(&im, &img, rgba);
		}
		if (e) {
			fprintf(stderr, "pngc: %s\n", e);
			free(in.p); free(im.idat.p); free(raw.p); free(img.p);
			free(rgba);
			return 1;
		}
		writepam(&im, rgba, (size_t)nout);
		free(rgba);
	}
	free(in.p);
	free(im.idat.p);
	free(raw.p);
	free(img.p);
	return 0;
}
