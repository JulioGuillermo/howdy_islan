#include "render.h"

#include <math.h>
#include <string.h>

#define SHOULDER 12.0
#define SHOULDER_ROUNDNESS 0.55
#define SHOULDER_MAX_DEPTH_SHARE 0.6
#define OPENING_REACH_SHRINK 0.4
#define OPENING_DEPTH_GROWTH 0.9
#define CLOSING_REACH_GROWTH 1.4
#define CLOSING_DEPTH_SHRINK 0.5
#define CORNER 34.0
#define CORNER_HANDLE 0.3
#define OUTLINE_WIDTH 1.5
#define OUTLINE_ALPHA 0.45

#define PADDING 12.0
#define PREVIEW_ASPECT 1.6
#define PREVIEW_RADIUS 12.0
#define FACE_RADIUS 6.0
#define BADGE_RADIUS 13.0
#define BAR_HEIGHT 4.0

#define FONT_FAMILY "Sans"
#define TITLE_SIZE 17.0
#define SUBTITLE_SIZE 12.5
#define ELLIPSIS "\xe2\x80\xa6"

static const struct color WHITE = {1, 1, 1};
static const struct color SUBTITLE_COLOR = {0.75, 0.75, 0.78};
static const struct color PREVIEW_BACKGROUND = {0.08, 0.08, 0.09};

static double clamp(double value, double low, double high)
{
	return fmax(low, fmin(value, high));
}

static void set_color(cairo_t *cr, struct color color, double alpha)
{
	cairo_set_source_rgba(cr, color.red, color.green, color.blue, alpha);
}

struct shoulders {
	double reach;
	double depth;
};

/*
 * The curves joining the island to the top edge. They stay glued to it and react to the motion:
 * stretched (deeper, narrower) while it comes out, squashed (flatter, wider) while it goes back.
 */
static struct shoulders shoulders_for(const struct island *island)
{
	double stretch = island_stretch(island);
	struct shoulders shoulders = {SHOULDER, SHOULDER};
	if (stretch > 0) {
		shoulders.reach *= 1 - OPENING_REACH_SHRINK * stretch;
		shoulders.depth *= 1 + OPENING_DEPTH_GROWTH * stretch;
	} else {
		shoulders.reach *= 1 - CLOSING_REACH_GROWTH * stretch;
		shoulders.depth *= 1 + CLOSING_DEPTH_SHRINK * stretch;
	}
	shoulders.depth = fmin(shoulders.depth, island->height.value * SHOULDER_MAX_DEPTH_SHARE);
	return shoulders;
}

/* Notch hanging from the top edge, open along it: shoulders into the edge at the top, smooth corners below. */
static void island_outline(cairo_t *cr, double center_x, double width, double height, struct shoulders shoulders)
{
	double left = center_x - width / 2;
	double right = center_x + width / 2;
	double reach = shoulders.reach;
	double depth = shoulders.depth;
	double reach_handle = reach * (1 - SHOULDER_ROUNDNESS);
	double depth_handle = depth * (1 - SHOULDER_ROUNDNESS);
	double corner = fmax(0, fmin(CORNER, fmin(height - depth, width / 2)));
	double corner_handle = corner * CORNER_HANDLE;

	cairo_new_path(cr);
	cairo_move_to(cr, left - reach, 0);
	cairo_curve_to(cr, left - reach_handle, 0, left, depth_handle, left, depth);
	cairo_line_to(cr, left, height - corner);
	cairo_curve_to(cr, left, height - corner_handle, left + corner_handle, height, left + corner, height);
	cairo_line_to(cr, right - corner, height);
	cairo_curve_to(cr, right - corner_handle, height, right, height - corner_handle, right, height - corner);
	cairo_line_to(cr, right, depth);
	cairo_curve_to(cr, right, depth_handle, right + reach_handle, 0, right + reach, 0);
}

static void rounded_rectangle(cairo_t *cr, double x, double y, double width, double height, double radius)
{
	radius = fmin(radius, fmin(width / 2, height / 2));
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + width - radius, y + radius, radius, -M_PI / 2, 0);
	cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, M_PI / 2);
	cairo_arc(cr, x + radius, y + height - radius, radius, M_PI / 2, M_PI);
	cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static cairo_surface_t *preview_image(const struct island *island)
{
	cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_RGB24, island->preview_width, island->preview_height);
	cairo_surface_flush(image);
	unsigned char *data = cairo_image_surface_get_data(image);
	int stride = cairo_image_surface_get_stride(image);

	for (int y = 0; y < island->preview_height; y++) {
		uint32_t *row = (uint32_t *)(data + (size_t)y * stride);
		const uint8_t *gray_row = island->preview + (size_t)y * island->preview_width;
		for (int x = 0; x < island->preview_width; x++) {
			uint32_t gray = gray_row[x];
			row[x] = 0xff000000u | gray << 16 | gray << 8 | gray;
		}
	}

	cairo_surface_mark_dirty(image);
	return image;
}

static void draw_preview(cairo_t *cr, const struct island *island, double x, double y, double width, double height)
{
	cairo_save(cr);
	rounded_rectangle(cr, x, y, width, height, PREVIEW_RADIUS);
	cairo_clip(cr);
	set_color(cr, PREVIEW_BACKGROUND, 1);
	cairo_paint(cr);

	if (island->preview_width > 0) {
		cairo_surface_t *image = preview_image(island);
		cairo_save(cr);
		cairo_translate(cr, x + width, y);
		cairo_scale(cr, -width / island->preview_width, height / island->preview_height);
		cairo_set_source_surface(cr, image, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_surface_destroy(image);
	}

	double scan_line = island_scan_line(island);
	if (scan_line >= 0) {
		cairo_rectangle(cr, x, y + scan_line * height - 1, width, 2);
		set_color(cr, island->accent, 0.55);
		cairo_fill(cr);
	}
	cairo_restore(cr);

	for (int i = 0; i < island->face_count; i++) {
		const struct face_box *face = &island->faces[i];
		double mirrored_x = 1 - face->x - face->width;
		rounded_rectangle(cr, x + mirrored_x * width, y + face->y * height, face->width * width, face->height * height, FACE_RADIUS);
		set_color(cr, island->accent, 0.95);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
	}
}

static double text_width(cairo_t *cr, const char *text)
{
	cairo_text_extents_t extents;
	cairo_text_extents(cr, text, &extents);
	return extents.x_advance;
}

/* Copies text into fitted; when wider than max_width, cuts it at a character boundary and adds an ellipsis. */
static void fit_text(cairo_t *cr, const char *text, double max_width, char *fitted)
{
	strcpy(fitted, text);
	size_t length = strlen(text);
	while (length > 0 && text_width(cr, fitted) > max_width) {
		length--;
		while (length > 0 && ((unsigned char)text[length] & 0xc0) == 0x80)
			length--;
		memcpy(fitted, text, length);
		strcpy(fitted + length, ELLIPSIS);
	}
}

static void draw_text(cairo_t *cr, const char *text, double size, cairo_font_weight_t weight, struct color color,
	double x, double top, double max_width)
{
	cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL, weight);
	cairo_set_font_size(cr, size);

	char fitted[ISLAND_TEXT_MAX + sizeof ELLIPSIS];
	fit_text(cr, text, max_width, fitted);

	cairo_font_extents_t font;
	cairo_font_extents(cr, &font);
	set_color(cr, color, 1);
	cairo_move_to(cr, x, top + font.ascent);
	cairo_show_text(cr, fitted);
}

static void draw_progress_bar(cairo_t *cr, double fraction, struct color accent, double x, double y, double width)
{
	rounded_rectangle(cr, x, y, width, BAR_HEIGHT, BAR_HEIGHT / 2);
	set_color(cr, WHITE, 0.12);
	cairo_fill(cr);

	if (fraction <= 0)
		return;
	rounded_rectangle(cr, x, y, fmax(BAR_HEIGHT, width * fraction), BAR_HEIGHT, BAR_HEIGHT / 2);
	set_color(cr, accent, 1);
	cairo_fill(cr);
}

/* Spinning arc while working, a check on success, a cross on failure. */
static void draw_status_badge(cairo_t *cr, const struct island *island, double center_x, double center_y)
{
	double radius = BADGE_RADIUS;
	cairo_set_line_width(cr, 2.5);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	set_color(cr, island->accent, 1);
	cairo_new_path(cr);

	if (island->phase == ISLAND_SUCCESS) {
		cairo_move_to(cr, center_x - radius * 0.5, center_y);
		cairo_line_to(cr, center_x - radius * 0.1, center_y + radius * 0.4);
		cairo_line_to(cr, center_x + radius * 0.55, center_y - radius * 0.4);
	} else if (island->phase == ISLAND_FAILURE) {
		double arm = radius * 0.42;
		cairo_move_to(cr, center_x - arm, center_y - arm);
		cairo_line_to(cr, center_x + arm, center_y + arm);
		cairo_move_to(cr, center_x + arm, center_y - arm);
		cairo_line_to(cr, center_x - arm, center_y + arm);
	} else {
		double start = fmod(island->now, 1.0) * 2 * M_PI;
		cairo_arc(cr, center_x, center_y, radius * 0.7, start, start + M_PI * 1.3);
	}
	cairo_stroke(cr);
}

/* The open island's content, laid out for ISLAND_EXPANDED_WIDTH x ISLAND_EXPANDED_HEIGHT. */
static void draw_content(cairo_t *cr, const struct island *island)
{
	double preview_height = ISLAND_EXPANDED_HEIGHT - 2 * PADDING;
	double preview_width = preview_height * PREVIEW_ASPECT;
	draw_preview(cr, island, PADDING, PADDING, preview_width, preview_height);

	double text_left = 2 * PADDING + preview_width;
	double badge_center_x = ISLAND_EXPANDED_WIDTH - PADDING - BADGE_RADIUS;
	double text_max_width = badge_center_x - BADGE_RADIUS - PADDING - text_left;
	draw_text(cr, island->title, TITLE_SIZE, CAIRO_FONT_WEIGHT_BOLD, WHITE, text_left, PADDING + 4, text_max_width);
	draw_text(cr, island->subtitle, SUBTITLE_SIZE, CAIRO_FONT_WEIGHT_NORMAL, SUBTITLE_COLOR, text_left, PADDING + 28, text_max_width);
	draw_progress_bar(cr, island_progress(island), island->accent, text_left,
		ISLAND_EXPANDED_HEIGHT - PADDING - 2 * BAR_HEIGHT, text_max_width);
	draw_status_badge(cr, island, badge_center_x, PADDING + 16);
}

void render_island(cairo_t *cr, const struct island *island)
{
	cairo_save(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_restore(cr);

	double width = island->width.value;
	double height = island->height.value;
	if (width < 1 || height < 0.5)
		return;

	double center_x = RENDER_CANVAS_WIDTH / 2.0 + island_shake_offset(island);
	double expansion = island_expansion(island);
	struct shoulders shoulders = shoulders_for(island);

	island_outline(cr, center_x, width, height, shoulders);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_fill(cr);

	double content_alpha = clamp(expansion * 1.6 - 0.6, 0, 1);
	if (content_alpha > 0) {
		cairo_save(cr);
		island_outline(cr, center_x, width, height, shoulders);
		cairo_clip(cr);
		cairo_push_group(cr);
		cairo_translate(cr, center_x - ISLAND_EXPANDED_WIDTH / 2, height - ISLAND_EXPANDED_HEIGHT);
		draw_content(cr, island);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, content_alpha);
		cairo_restore(cr);
	}

	if (expansion > 0) {
		island_outline(cr, center_x, width, height, shoulders);
		set_color(cr, island->accent, OUTLINE_ALPHA * expansion);
		cairo_set_line_width(cr, OUTLINE_WIDTH);
		cairo_stroke(cr);
	}
}
