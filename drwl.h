/*
 * drwl - https://codeberg.org/sewn/drwl
 * See LICENSE file for copyright and license details.
 */
#pragma once

#include <stdlib.h>
#include <fcft/fcft.h>
#include <pixman-1/pixman.h>

#define BETWEEN(X, A, B) ((A) <= (X) && (X) <= (B))

enum { ColFg, ColBg }; /* colorscheme index */

typedef struct {
	pixman_image_t *pix;
	struct fcft_font *font;
	uint32_t *scheme;
} Drwl;

#define UTF_INVALID 0xFFFD
#define UTF_SIZ     4

static const unsigned char utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static const unsigned char utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static const uint32_t utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static const uint32_t utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

static inline uint32_t
utf8decodebyte(const char c, size_t *i)
{
	for (*i = 0; *i < (UTF_SIZ + 1); ++(*i))
		if (((unsigned char)c & utfmask[*i]) == utfbyte[*i])
			return (unsigned char)c & ~utfmask[*i];
	return 0;
}

static inline size_t
utf8decode(const char *c, uint32_t *u)
{
	size_t i, j, len, type;
	uint32_t udecoded;

	*u = UTF_INVALID;
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, UTF_SIZ))
		return 1;
	for (i = 1, j = 1; i < UTF_SIZ && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	if (!BETWEEN(*u, utfmin[len], utfmax[len]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;
	return len;
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
 * Caller must call drwl_destroy_font on returned font when done using it, 
 * otherwise use drwl_destroy when instead given a drwl context.
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
	int utf8charlen, render = x || y || w || h;
	long x_kern;
	uint32_t cp = 0, last_cp = 0;
	pixman_color_t clr;
	pixman_image_t *fg_pix = NULL;
	int noellipsis = 0;
	const struct fcft_glyph *glyph, *eg;

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
	eg = fcft_rasterize_char_utf32(drwl->font, 0x2026, FCFT_SUBPIXEL_DEFAULT);

	while (*text) {
		utf8charlen = utf8decode(text, &cp);

		glyph = fcft_rasterize_char_utf32(drwl->font, cp, FCFT_SUBPIXEL_DEFAULT);
		if (!glyph)
			continue;

		x_kern = 0;
		if (last_cp)
			fcft_kerning(drwl->font, last_cp, cp, &x_kern, NULL);
		last_cp = cp;

		ty = y + (h - drwl->font->height) / 2 + drwl->font->ascent;

		/* draw ellipsis if remaining text doesn't fit */
		if (!noellipsis && x_kern + glyph->advance.x + eg->advance.x > w && *(text + 1) != '\0') {
			if (drwl_text(drwl, 0, 0, 0, 0, 0, text, 0)
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

		text += utf8charlen;
		x += glyph->advance.x;
		w -= glyph->advance.x;
	}

	if (render)
		pixman_image_unref(fg_pix);

	return x + (render ? w : 0);
}

static unsigned int
drwl_font_getwidth(Drwl *drwl, const char *text)
{
	if (!drwl || !drwl->font || !text)
		return 0;
	return drwl_text(drwl, 0, 0, 0, 0, 0, text, 0);
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
