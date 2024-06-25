/* See LICENSE file for copyright and license details. */

/* appearance */
static const char *font_name = "monospace:size=16:dpi=96";
static uint32_t scheme[2] = {
	[ColFg] = 0xbbbbbbff,
	[ColBg] = 0x000000ff,
};

static int32_t width = 0;
static int32_t height = 0;
static int x = 0;
static int y = 0;

/* behavior */
static int period = 5;

/*
 * Delimeter string, encountered as a separate line in subcommand output,
 * signaling rendering buffered text and continuing with next frame.
 */
static const char delimeter[] = "\4";
