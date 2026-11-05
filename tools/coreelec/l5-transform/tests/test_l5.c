/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dv_l5_transform.h"

static const struct dv_l5_geometry identity = {
	{3840, 0, 3840, 3840, 0, 3840},
	{2160, 0, 2160, 2160, 0, 2160}
};

static void pack(u32 *words, const unsigned char *bytes, u32 size)
{
	u32 i;
	memset(words, 0, 512 * sizeof(*words));
	words[0] = (size << 8) | bytes[0];
	for (i = 1; i < size; i++)
		words[1 + (i - 1) / 4] |= (u32)bytes[i] << (8 * ((i - 1) % 4));
}

/* L1 before L5, an unknown level after it, plus nonzero padding. */
static void metadata(u32 *words, u32 l, u32 r, u32 t, u32 b)
{
	unsigned char bytes[104] = {0};
	bytes[0] = 0x81;
	bytes[70] = 3;
	bytes[74] = 6; bytes[75] = 1;
	memset(bytes + 76, 0x5a, 6);
	bytes[85] = 8; bytes[86] = 5;
	bytes[87] = l >> 8; bytes[88] = l & 255;
	bytes[89] = r >> 8; bytes[90] = r & 255;
	bytes[91] = t >> 8; bytes[92] = t & 255;
	bytes[93] = b >> 8; bytes[94] = b & 255;
	bytes[98] = 4; bytes[99] = 123;
	memset(bytes + 100, 0xa5, 4);
	pack(words, bytes, sizeof(bytes));
	words[26] |= 0xee000000U;
}

static void expected(const u32 *w, u32 l, u32 r, u32 t, u32 b)
{
	assert(dv_l5_be16(w, 87) == l);
	assert(dv_l5_be16(w, 89) == r);
	assert(dv_l5_be16(w, 91) == t);
	assert(dv_l5_be16(w, 93) == b);
}

static void geometry_tests(void)
{
	u32 source[512], result[512], again[512], i;
	struct dv_l5_geometry g = identity;
	metadata(source, 0, 0, 276, 277);
	memcpy(result, source, sizeof(source));
	assert(dv_l5_transform(result, 27, &g));
	assert(!memcmp(result, source, sizeof(source)));

	/* Near 115% zoom: actual integer VPP crop, not a guessed GUI zoom. */
	g.x.crop_start = 250; g.x.crop_end = 3590;
	g.y.crop_start = 140; g.y.crop_end = 2020;
	memcpy(result, source, sizeof(source));
	assert(dv_l5_transform(result, 27, &g));
	expected(result, 0, 0, 156, 157);
	for (i = 0; i < 105; i++)
		if (i < 87 || i >= 95)
			assert(dv_l5_byte(result, i) == dv_l5_byte(source, i));
	expected(source, 0, 0, 276, 277);
	/* Repeated output copies must not compound, and reset is exact. */
	memcpy(again, source, sizeof(source));
	assert(dv_l5_transform(again, 27, &g));
	assert(!memcmp(result, again, sizeof(result)));
	memcpy(again, source, sizeof(source));
	assert(dv_l5_transform(again, 27, &identity));
	assert(!memcmp(source, again, sizeof(source)));

	/* Pure position change: same dimensions, no new decoded frame needed. */
	g = identity;
	g.y.crop_end = 2060; g.y.dest_start = 100;
	memcpy(result, source, sizeof(source));
	assert(dv_l5_transform(result, 27, &g));
	expected(result, 0, 0, 376, 177);

	/* Zoom out, asymmetry, resolution conversion and simultaneous borders. */
	g = identity;
	g.x.dest_start = 384; g.x.dest_end = 3456;
	g.y.dest_start = 216; g.y.dest_end = 1944;
	memcpy(result, source, sizeof(source));
	assert(dv_l5_transform(result, 27, &g));
	expected(result, 384, 384, 436, 437);
	g = identity;
	g.x.output_size = g.x.dest_end = 1920;
	g.y.output_size = g.y.dest_end = 1080;
	memcpy(result, source, sizeof(source));
	assert(dv_l5_transform(result, 27, &g));
	expected(result, 0, 0, 138, 138);
	metadata(result, 400, 200, 100, 300);
	assert(dv_l5_transform(result, 27, &g));
	expected(result, 200, 100, 50, 150);

	/* Crop removes all borders, but never erases the source L5. */
	g = identity;
	g.y.crop_start = 300; g.y.crop_end = 1860;
	memcpy(result, source, sizeof(source));
	assert(dv_l5_transform(result, 27, &g));
	expected(result, 0, 0, 0, 0);
	expected(source, 0, 0, 276, 277);
	metadata(result, 0, 0, 100, 200); /* later scene */
	g = identity;
	g.y.crop_start = 100; g.y.crop_end = 2060;
	assert(dv_l5_transform(result, 27, &g));
	expected(result, 0, 0, 0, 110);
}

static void unchanged(u32 *w, u32 count, const struct dv_l5_geometry *g)
{
	u32 original[512];
	memcpy(original, w, sizeof(original));
	assert(!dv_l5_transform(w, count, g));
	assert(!memcmp(original, w, sizeof(original)));
}

static void rejection_tests(void)
{
	u32 w[512], len;
	struct dv_l5_geometry g = identity;
	metadata(w, 0, 0, 0, 0);
	g.y.dest_start = 200;
	unchanged(w, 27, &g); /* suppression survives zoom out */
	metadata(w, 0, 0, 276, 277);
	g.y.crop_end = g.y.crop_start;
	unchanged(w, 27, &g);
	g = identity; g.x.output_size = 65536;
	unchanged(w, 27, &g);
	g = identity; g.x.dest_end = 4000;
	unchanged(w, 27, &g);
	g = identity; g.y.crop_start = 2000;
	unchanged(w, 27, &g); /* no visible active-area intersection */
	metadata(w, 0, 0, 2000, 2000);
	unchanged(w, 27, &identity);
	metadata(w, 0, 0, 276, 277);
	for (len = 0; len < 27; len++)
		unchanged(w, len, &identity);
	unchanged(w, 513, &identity);
	assert(!dv_l5_transform(NULL, 27, &identity));
	assert(!dv_l5_transform(w, 27, NULL));
	dv_l5_put_byte(w, 70, 4); /* truncated block after a valid L5 */
	unchanged(w, 27, &identity);
	metadata(w, 0, 0, 276, 277);
	dv_l5_put_byte(w, 95, 255); /* overflowing trailing TLV length */
	unchanged(w, 27, &identity);
	metadata(w, 0, 0, 276, 277);
	dv_l5_put_byte(w, 85, 7); /* incorrect L5 payload size */
	unchanged(w, 27, &identity);
	metadata(w, 0, 0, 276, 277);
	dv_l5_put_byte(w, 86, 42); /* no L5 */
	unchanged(w, 27, &identity);
	metadata(w, 0, 0, 276, 277);
	dv_l5_put_byte(w, 99, 5); /* second L5 */
	unchanged(w, 27, &identity);
}

static u32 random_state = 0x51f031a2;
static u32 random_u32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static void fuzz_tests(void)
{
	u32 w[512], i, j, count;
	for (i = 0; i < 20000; i++) {
		for (j = 0; j < 512; j++)
			w[j] = random_u32();
		count = random_u32() % 514;
		(void)dv_l5_transform(w, count, &identity);
	}
	/* Independent rational oracle for active edges and outward rounding. */
	for (i = 0; i < 20000; i++) {
		struct dv_l5_axis a;
		u32 near, far, n, f;
		double exact_n, exact_f;
		a.coded_size = 1 + random_u32() % 65535;
		a.output_size = 1 + random_u32() % 65535;
		a.crop_start = random_u32() % a.coded_size;
		a.crop_end = a.crop_start + 1 + random_u32() % (a.coded_size - a.crop_start);
		a.dest_start = random_u32() % a.output_size;
		a.dest_end = a.dest_start + 1 + random_u32() % (a.output_size - a.dest_start);
		near = random_u32() % a.coded_size;
		far = random_u32() % (a.coded_size - near);
		if (!dv_l5_map_axis(&a, near, far, &n, &f))
			continue;
		exact_n = a.dest_start + ((double)near - a.crop_start) *
			(a.dest_end - a.dest_start) / (a.crop_end - a.crop_start);
		exact_f = a.dest_start + ((double)a.coded_size - far - a.crop_start) *
			(a.dest_end - a.dest_start) / (a.crop_end - a.crop_start);
		if (exact_n < a.dest_start) exact_n = a.dest_start;
		if (exact_f > a.dest_end) exact_f = a.dest_end;
		assert(n <= exact_n + 1e-8 && exact_n - n < 1.00000001);
		assert(a.output_size - f >= exact_f - 1e-8);
		assert(a.output_size - f - exact_f < 1.00000001);
		assert(n + f < a.output_size);
	}
}

int main(void)
{
	geometry_tests();
	rejection_tests();
	fuzz_tests();
	puts("PASS: L5 geometry, metadata integrity, rejection, 40000 randomized cases");
	return 0;
}
