/*
 * drwl - https://codeberg.org/sewn/drwl
 * See LICENSE file for copyright and license details.
 */
#include <fcft/fcft.h>
#include <pixman-1/pixman.h>

#define UTF8_ACCEPT 0
#define UTF8_REJECT 1

static const uint8_t utf8d[] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
	7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
	8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
	0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
	0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
	0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
	1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
	1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
	1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
	1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
};

static inline uint32_t
utf8decode(uint32_t *state, uint32_t *codep, uint8_t byte)
{
	uint32_t type = utf8d[byte];

	*codep = (*state != UTF8_ACCEPT) ?
		(byte & 0x3fu) | (*codep << 6) :
		(0xff >> type) & (byte);

	*state = utf8d[256 + *state*16 + type];
	return *state;
}

static void
drwl_rect(pixman_image_t *pix, 
          int16_t x, int16_t y, uint16_t w, uint16_t h, 
          int filled, pixman_color_t *bg)
{
	if (filled)
		pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, bg, 1, 
			&(pixman_rectangle16_t){x, y, w, h});
	else
		pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, bg, 4,
			(pixman_rectangle16_t[4]){
				{ x,         y,         w, 1 },
				{ x,         y + h - 1, w, 1 },
				{ x,         y,         1, h },
				{ x + w - 1, y,         1, h }});
}

static int
drwl_text(pixman_image_t *pix, struct fcft_font *font,
		int x, int y, unsigned int w, unsigned int h,
		unsigned int lpad, const char *text,
		pixman_color_t *fg, pixman_color_t *bg)
{
	int ty;
	int render = x || y || w || h;
	long x_kern;
	uint32_t cp, last_cp = 0;
	uint32_t state = UTF8_ACCEPT;
	pixman_image_t *fg_pix = NULL;
	const struct fcft_glyph *glyph, *eg;

	if ((render && (!fg || !w)) || !text || !font)
		return 0;

	if (!render) {
		w = -1;
	} else {
		fg_pix = pixman_image_create_solid_fill(fg);

		if (bg)
			drwl_rect(pix, x, y, w, h, 1, bg);

		x += lpad;
		w -= lpad;
	}

	// U+2026 == …
	eg = fcft_rasterize_char_utf32(font, 0x2026, FCFT_SUBPIXEL_NONE);

	for (const char *p = text; *p; p++) {
		if (utf8decode(&state, &cp, *p))
			continue;

		glyph = fcft_rasterize_char_utf32(font, cp, FCFT_SUBPIXEL_NONE);
		if (!glyph)
			continue;

		x_kern = 0;
		if (last_cp)
			fcft_kerning(font, last_cp, cp, &x_kern, NULL);
		last_cp = cp;

		ty = y + (h - font->height) / 2 + font->ascent;

		/* only ellipsis if we haven't reached the end */
		if (x_kern + glyph->advance.x + eg->advance.x > w && *(p + 1) != '\0') {
			w -= eg->advance.x;
			pixman_image_composite32(
				PIXMAN_OP_OVER, fg_pix, eg->pix, pix, 0, 0, 0, 0,
				x + eg->x, ty - eg->y, eg->width, eg->height);
		}

		if ((x_kern + glyph->advance.x) > w)
			break;

		x += x_kern;

		if (render && pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8)
			// pre-rendered glyphs (eg. emoji)
			pixman_image_composite32(
				PIXMAN_OP_OVER, glyph->pix, NULL, pix, 0, 0, 0, 0,
				x + glyph->x, ty - glyph->y, glyph->width, glyph->height);
		else if (render)
			pixman_image_composite32(
				PIXMAN_OP_OVER, fg_pix, glyph->pix, pix, 0, 0, 0, 0,
				x + glyph->x, ty - glyph->y, glyph->width, glyph->height);

		x += glyph->advance.x;
		w -= glyph->advance.x;
	}

	if (render)
		pixman_image_unref(fg_pix);

	return x + (render ? w : 0);
}
