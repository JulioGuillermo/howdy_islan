#include "island.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIDTH_STIFFNESS 260.0
#define WIDTH_DAMPING 21.0
#define HEIGHT_STIFFNESS 320.0
#define HEIGHT_DAMPING 23.0
#define WIDTH_CLOSING_DAMPING 32.0
#define HEIGHT_CLOSING_DAMPING 36.0

#define SUCCESS_HOLD_SECONDS 0.9
#define FAILURE_HOLD_SECONDS 1.8
#define SILENCE_LIMIT_SECONDS 5.0
#define SHAKE_SECONDS 0.6
#define SHAKE_AMPLITUDE 10.0
#define SCAN_LINE_SECONDS 1.4
#define FULL_STRETCH_SPEED 900.0

static const struct color SCAN_COLOR = {0.35, 0.78, 0.98};
static const struct color SUCCESS_COLOR = {0.19, 0.82, 0.35};
static const struct color FAILURE_COLOR = {1.0, 0.27, 0.23};

struct failure_text {
	int exit_code;
	const char *title;
	const char *subtitle;
};

static const struct failure_text FAILURE_TEXTS[] = {
	{10, "No face enrolled", "Add one with sudo howdy add"},
	{11, "Not recognised", "Use your password"},
	{13, "Too dark", "Check the IR camera"},
};

static double clamp(double value, double low, double high)
{
	return fmax(low, fmin(value, high));
}

static void touch(struct island *island)
{
	island->last_message_at = island->now;
}

static void set_phase(struct island *island, enum island_phase phase, struct color accent, const char *title, const char *subtitle)
{
	island->phase = phase;
	island->accent = accent;
	snprintf(island->title, sizeof island->title, "%s", title);
	snprintf(island->subtitle, sizeof island->subtitle, "%s", subtitle);
	touch(island);
}

static void expand(struct island *island)
{
	island->collapse_at = 0;
	island->width.target = ISLAND_EXPANDED_WIDTH;
	island->width.damping = WIDTH_DAMPING;
	island->height.target = ISLAND_EXPANDED_HEIGHT;
	island->height.damping = HEIGHT_DAMPING;
}

static void collapse(struct island *island)
{
	island->phase = ISLAND_REST;
	island->collapse_at = 0;
	island->width.target = ISLAND_REST_WIDTH;
	island->width.damping = WIDTH_CLOSING_DAMPING;
	island->height.target = ISLAND_REST_HEIGHT;
	island->height.damping = HEIGHT_CLOSING_DAMPING;
}

static bool is_working(const struct island *island)
{
	return island->phase == ISLAND_PREPARING || island->phase == ISLAND_SCANNING;
}

static bool is_shaking(const struct island *island)
{
	return island->shake_started_at > 0 && island->now - island->shake_started_at < SHAKE_SECONDS;
}

void island_init(struct island *island, double now)
{
	memset(island, 0, sizeof *island);
	island->now = now;
	island->accent = SCAN_COLOR;
	spring_init(&island->width, ISLAND_REST_WIDTH, WIDTH_STIFFNESS, WIDTH_DAMPING);
	spring_init(&island->height, ISLAND_REST_HEIGHT, HEIGHT_STIFFNESS, HEIGHT_DAMPING);
}

void island_begin(struct island *island, uid_t user_uid, const char *user)
{
	snprintf(island->user, sizeof island->user, "%s", user);
	island->user_uid = user_uid;
	island->preview_width = 0;
	island->face_count = 0;
	island->scan_started_at = 0;
	island->shake_started_at = 0;
	set_phase(island, ISLAND_PREPARING, SCAN_COLOR, "Opening camera", "Look at the screen");
	expand(island);
}

void island_scan(struct island *island, double timeout, double threshold)
{
	island->scan_timeout = timeout;
	island->threshold = threshold;
	island->scan_started_at = island->now;
	set_phase(island, ISLAND_SCANNING, SCAN_COLOR, "Scanning", "Looking for your face");
	expand(island);
}

void island_show_frame(struct island *island, int width, int height, const uint8_t *gray_pixels)
{
	if (width <= 0 || height <= 0 || width * height > ISLAND_PREVIEW_MAX_PIXELS)
		return;

	memcpy(island->preview, gray_pixels, (size_t)width * height);
	island->preview_width = width;
	island->preview_height = height;
	touch(island);
}

void island_show_faces(struct island *island, const struct face_box *faces, int count)
{
	island->face_count = count < ISLAND_MAX_FACES ? count : ISLAND_MAX_FACES;
	memcpy(island->faces, faces, sizeof *faces * island->face_count);
	if (island->phase == ISLAND_SCANNING && count == 0)
		snprintf(island->subtitle, sizeof island->subtitle, "Looking for your face");
	touch(island);
}

void island_show_certainty(struct island *island, double value)
{
	if (island->phase == ISLAND_SCANNING)
		snprintf(island->subtitle, sizeof island->subtitle, "Distance %.1f (max %.1f)", value, island->threshold);
	touch(island);
}

void island_finish(struct island *island, int exit_code)
{
	if (island->phase == ISLAND_REST)
		return;

	if (exit_code == 0) {
		char greeting[ISLAND_TEXT_MAX + 8];
		snprintf(greeting, sizeof greeting, "Hi, %s", island->user);
		set_phase(island, ISLAND_SUCCESS, SUCCESS_COLOR, "Recognised", greeting);
		island->collapse_at = island->now + SUCCESS_HOLD_SECONDS;
		return;
	}

	char code[32];
	snprintf(code, sizeof code, "Code %d", exit_code);
	const char *title = "Howdy error";
	const char *subtitle = code;
	for (size_t i = 0; i < sizeof FAILURE_TEXTS / sizeof *FAILURE_TEXTS; i++) {
		if (FAILURE_TEXTS[i].exit_code == exit_code) {
			title = FAILURE_TEXTS[i].title;
			subtitle = FAILURE_TEXTS[i].subtitle;
		}
	}
	set_phase(island, ISLAND_FAILURE, FAILURE_COLOR, title, subtitle);
	island->shake_started_at = island->now;
	island->collapse_at = island->now + FAILURE_HOLD_SECONDS;
}

void island_advance(struct island *island, double now)
{
	double elapsed = now - island->now;
	island->now = now;
	spring_step(&island->width, elapsed);
	spring_step(&island->height, elapsed);

	bool hold_over = island->collapse_at > 0 && now >= island->collapse_at;
	bool howdy_went_silent = is_working(island) && now - island->last_message_at > SILENCE_LIMIT_SECONDS;
	if (hold_over || howdy_went_silent)
		collapse(island);
}

bool island_is_animating(const struct island *island)
{
	bool moving = !spring_is_settled(&island->width) || !spring_is_settled(&island->height);
	return moving || is_working(island) || is_shaking(island);
}

bool island_is_raised(const struct island *island)
{
	return island->phase != ISLAND_REST || !spring_is_settled(&island->height) || !spring_is_settled(&island->width);
}

double island_next_deadline(const struct island *island)
{
	double deadline = island->collapse_at;
	if (is_working(island)) {
		double silence_deadline = island->last_message_at + SILENCE_LIMIT_SECONDS;
		deadline = deadline > 0 ? fmin(deadline, silence_deadline) : silence_deadline;
	}
	return deadline;
}

double island_expansion(const struct island *island)
{
	double travel = ISLAND_EXPANDED_HEIGHT - ISLAND_REST_HEIGHT;
	return clamp((island->height.value - ISLAND_REST_HEIGHT) / travel, 0, 1);
}

double island_shake_offset(const struct island *island)
{
	if (!is_shaking(island))
		return 0;
	double since = island->now - island->shake_started_at;
	return SHAKE_AMPLITUDE * sin(since * 40) * exp(-since * 6);
}

double island_stretch(const struct island *island)
{
	return clamp(island->height.velocity / FULL_STRETCH_SPEED, -1, 1);
}

double island_progress(const struct island *island)
{
	switch (island->phase) {
	case ISLAND_PREPARING:
		return 0;
	case ISLAND_SCANNING:
		if (island->scan_timeout <= 0)
			return 0;
		return clamp((island->now - island->scan_started_at) / island->scan_timeout, 0, 1);
	default:
		return 1;
	}
}

double island_scan_line(const struct island *island)
{
	if (island->phase != ISLAND_SCANNING)
		return -1;
	return fmod(island->now, SCAN_LINE_SECONDS) / SCAN_LINE_SECONDS;
}
