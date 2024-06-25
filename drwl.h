/*
 * drwl - https://codeberg.org/sewn/drwl
 * See LICENSE file for copyright and license details.
 */
#pragma once

#include <stdlib.h>
#include <fcft/fcft.h>
#include <pixman-1/pixman.h>

enum { ColFg, ColBg }; /* colorscheme index */

typedef struct {
	pixman_image_t *pix;
	struct fcft_font *font;
	uint32_t *scheme;
} Drwl;

/* See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details. */
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
utf8decode(uint8_t byte)
{
	uint32_t type = utf8d[byte];
	return !utf8d[256 + type] ? (0xff >> type) & (byte) : 0xFFFD;
}

static int
drwl_init(void)
{
	fcft_set_scaling_filter(FCFT_SCALING_FILTER_LANCZOS3);
	return fcft_init(FCFT_LOG_COLORIZE_AUTO, 0, FCFT_LOG_CLASS_ERROR);
}

/*
 * Caller must call drwl_init before drwl_create, and
 * drwl_destroy on returned context after finalizing usage.
 */
static Drwl *
drwl_create(void)
{
	Drwl *drwl;
	
	if (!(drwl = calloc(1, sizeof(Drwl))))
		return NULL;

	return drwl;
}

static void
drwl_setfont(Drwl *drwl, struct fcft_font *font)
{
	if (drwl)
		drwl->font = font;
}

/* 
 * Returned font is set within the drawing context if given.
 * Caller must call drwl_destroy_font on returned font when done using it, otherwise
 * use drwl_destroy when instead given a drwl context.
 */
static struct fcft_font *
drwl_load_font(Drwl *drwl, size_t fontcount,
		const char *fonts[static fontcount], const char *attributes)
{
	struct fcft_font *font = fcft_from_name(fontcount, fonts, attributes);
	if (drwl)
		drwl_setfont(drwl, font);
	return font;
}

static void
drwl_destroy_font(struct fcft_font *font)
{
	fcft_destroy(font);
}

static inline pixman_color_t
convert_color(uint32_t clr)
{
	return (pixman_color_t){
		((clr >> 24) & 0xFF) * 0x101,
		((clr >> 16) & 0xFF) * 0x101,
		((clr >> 8) & 0xFF) * 0x101,
		(clr & 0xFF) * 0x101
	};
}

static void
drwl_setscheme(Drwl *drwl, uint32_t *scm)
{
	if (drwl)
		drwl->scheme = scm;
}

static inline int
drwl_stride(unsigned int width)
{
	return (((PIXMAN_FORMAT_BPP(PIXMAN_a8r8g8b8) * width + 7) / 8 + 4 - 1) & -4);
}

/*
 * Caller must call drwl_finish_drawing when finished drawing.
 * Parameter stride can be calculated using drwl_stride.
 */
static void
drwl_prepare_drawing(Drwl *drwl, unsigned int w, unsigned int h,
		uint32_t *bits, int stride)
{
	pixman_region32_t clip;

	if (!drwl)
		return;

	drwl->pix = pixman_image_create_bits_no_clear(
		PIXMAN_a8r8g8b8, w, h, bits, stride);
	pixman_region32_init_rect(&clip, 0, 0, w, h);
	pixman_image_set_clip_region32(drwl->pix, &clip);
	pixman_region32_fini(&clip);
}

static void
drwl_rect(Drwl *drwl,
          int x, int y, unsigned int w, unsigned int h,
          int filled, int invert)
{
	pixman_color_t clr;
	if (!drwl || !drwl->scheme || !drwl->pix)
		return;

	clr = convert_color(drwl->scheme[invert ? ColBg : ColFg]);
	if (filled)
		pixman_image_fill_rectangles(PIXMAN_OP_SRC, drwl->pix, &clr, 1,
			&(pixman_rectangle16_t){x, y, w, h});
	else
		pixman_image_fill_rectangles(PIXMAN_OP_SRC, drwl->pix, &clr, 4,
			(pixman_rectangle16_t[4]){
				{ x,         y,         w, 1 },
				{ x,         y + h - 1, w, 1 },
				{ x,         y,         1, h },
				{ x + w - 1, y,         1, h }});
}

static int
drwl_text(Drwl *drwl,
          int x, int y, unsigned int w, unsigned int h,
          unsigned int lpad, const char *text, int invert)
{
	int ty;
	int render = x || y || w || h;
	long x_kern;
	uint32_t cp, last_cp = 0;
	pixman_image_t *fg_pix = NULL;
	const struct fcft_glyph *glyph, *eg;
	int noellipsis = 0;
	pixman_color_t clr;

	if (!drwl || (render && (!drwl->scheme || !w || !drwl->pix)) || !text || !drwl->font)
		return 0;

	if (!render) {
		w = invert ? invert : ~invert;
	} else {
		clr = convert_color(drwl->scheme[invert ? ColBg : ColFg]);
		fg_pix = pixman_image_create_solid_fill(&clr);

		drwl_rect(drwl, x, y, w, h, 1, !invert);

		x += lpad;
		w -= lpad;
	}

	// U+2026 == …
	eg = fcft_rasterize_char_utf32(drwl->font, 0x2026, FCFT_SUBPIXEL_NONE);

	for (const char *p = text; *p; p++) {
		cp = utf8decode(*p);

		glyph = fcft_rasterize_char_utf32(drwl->font, cp, FCFT_SUBPIXEL_NONE);
		if (!glyph)
			continue;

		x_kern = 0;
		if (last_cp)
			fcft_kerning(drwl->font, last_cp, cp, &x_kern, NULL);
		last_cp = cp;

		ty = y + (h - drwl->font->height) / 2 + drwl->font->ascent;

		/* draw ellipsis if remaining text doesn't fit */
		if (!noellipsis && x_kern + glyph->advance.x + eg->advance.x > w && *(p + 1) != '\0') {
			if (drwl_text(drwl, 0, 0, 0, 0, 0, p, 0)
					- glyph->advance.x < eg->advance.x) {
				noellipsis = 1;
			} else {
				w -= eg->advance.x;
				pixman_image_composite32(
					PIXMAN_OP_OVER, fg_pix, eg->pix, drwl->pix, 0, 0, 0, 0,
					x + eg->x, ty - eg->y, eg->width, eg->height);
			}
		}

		if ((x_kern + glyph->advance.x) > w)
			break;

		x += x_kern;

		if (render && pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8)
			// pre-rendered glyphs (eg. emoji)
			pixman_image_composite32(
				PIXMAN_OP_OVER, glyph->pix, NULL, drwl->pix, 0, 0, 0, 0,
				x + glyph->x, ty - glyph->y, glyph->width, glyph->height);
		else if (render)
			pixman_image_composite32(
				PIXMAN_OP_OVER, fg_pix, glyph->pix, drwl->pix, 0, 0, 0, 0,
				x + glyph->x, ty - glyph->y, glyph->width, glyph->height);

		x += glyph->advance.x;
		w -= glyph->advance.x;
	}

	if (render)
		pixman_image_unref(fg_pix);

	return x + (render ? w : 0);
}

static void
drwl_finish_drawing(Drwl *drwl)
{
	if (drwl && drwl->pix)
		pixman_image_unref(drwl->pix);
}

static void
drwl_destroy(Drwl *drwl)
{
	if (drwl->pix)
		pixman_image_unref(drwl->pix);
	if (drwl->font)
		drwl_destroy_font(drwl->font);
	free(drwl);
}

static void
drwl_fini(void)
{
	fcft_fini();
}
