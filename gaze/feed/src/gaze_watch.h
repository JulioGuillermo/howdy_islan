#pragma once

#include <systemd/sd-bus.h>

#include "island_link.h"

#define GAZE_TEXT_MAX 96
#define BUS_NAME_MAX 256

enum gaze_activity {
	GAZE_IDLE,
	GAZE_VERIFYING,
	GAZE_ENROLLING,
};

/*
 * Follows Gaze's D-Bus traffic (the calls its clients make and the signals its daemon sends back)
 * and replays one scan or enrolment at a time on the island.
 */
struct gaze_watch {
	sd_bus *query_bus;
	struct island_link island;
	enum gaze_activity activity;
	char claim_sender[BUS_NAME_MAX];
	char claim_user[GAZE_TEXT_MAX];
	uid_t claim_caller_uid;
	char daemon_name[BUS_NAME_MAX];
	char hint[GAZE_TEXT_MAX];
	double hint_sent_at;
	bool showing_gray_camera;
};

void gaze_watch_handle(struct gaze_watch *watch, sd_bus_message *message, double now);
/* Repeats the last hint so the island does not close while Gaze has nothing new to say. */
void gaze_watch_tick(struct gaze_watch *watch, double now);
