#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "spring.h"

#define ISLAND_REST_WIDTH 130.0
#define ISLAND_REST_HEIGHT 7.0
#define ISLAND_EXPANDED_WIDTH 380.0
#define ISLAND_EXPANDED_HEIGHT 112.0

#define ISLAND_TEXT_MAX 96
#define ISLAND_MAX_FACES 8
#define ISLAND_PREVIEW_MAX_PIXELS (320 * 240)

enum island_phase {
	ISLAND_REST,
	ISLAND_PREPARING,
	ISLAND_SCANNING,
	ISLAND_ENROLLING,
	ISLAND_SUCCESS,
	ISLAND_FAILURE,
};

struct color {
	double red;
	double green;
	double blue;
};

/* Position and size as fractions of the preview. */
struct face_box {
	double x;
	double y;
	double width;
	double height;
};

/*
 * What the island shows and how far its open/close animation has gone. Times are monotonic seconds;
 * the message functions stamp them with the time of the last island_advance.
 */
struct island {
	enum island_phase phase;
	struct spring width;
	struct spring height;
	struct color accent;
	char title[ISLAND_TEXT_MAX];
	char subtitle[ISLAND_TEXT_MAX];
	char user[ISLAND_TEXT_MAX];
	uid_t user_uid;
	uint8_t preview[ISLAND_PREVIEW_MAX_PIXELS];
	int preview_width;
	int preview_height;
	struct face_box faces[ISLAND_MAX_FACES];
	int face_count;
	double threshold;
	double scan_timeout;
	double scan_started_at;
	double enrolment_progress;
	double shake_started_at;
	double last_message_at;
	double collapse_at;
	double now;
};

void island_init(struct island *island, double now);

void island_begin(struct island *island, uid_t user_uid, const char *user);
void island_scan(struct island *island, double timeout, double threshold);
void island_show_frame(struct island *island, int width, int height, const uint8_t *gray_pixels);
void island_show_faces(struct island *island, const struct face_box *faces, int count);
void island_show_certainty(struct island *island, double value);
void island_finish(struct island *island, int exit_code);
/* A line under the title while working, such as "Come closer". */
void island_show_hint(struct island *island, const char *hint);
/* The scan or enrolment stopped without a result (the password was typed instead): close quietly. */
void island_cancel(struct island *island);

/* howdy add: same frames and faces as a scan, its own progress and result. */
void island_enrol(struct island *island);
void island_show_enrolment_progress(struct island *island, double fraction);
/* reason: "added", or why it failed ("no-face", "multiple-faces", "dark", "black", "camera-failed",
 * "db-failed"). */
void island_finish_enrolment(struct island *island, const char *reason);

void island_advance(struct island *island, double now);
bool island_is_animating(const struct island *island);
/* True while open or moving: then it must show above fullscreen windows. */
bool island_is_raised(const struct island *island);
/* Next time the island changes on its own (hold over, Howdy gone silent); 0 when none. */
double island_next_deadline(const struct island *island);

/* 0 at rest size, 1 fully open. */
double island_expansion(const struct island *island);
double island_shake_offset(const struct island *island);
/* How fast it opens (toward 1) or closes (toward -1); 0 when still. */
double island_stretch(const struct island *island);
double island_progress(const struct island *island);
/* Height of the sweeping line as a fraction of the preview; negative when not scanning. */
double island_scan_line(const struct island *island);
