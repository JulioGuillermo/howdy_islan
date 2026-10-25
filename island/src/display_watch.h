#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include "wayland.h"

#define DISPLAY_WATCH_MAX 16
#define DISPLAY_WATCH_PATH_MAX 108

/*
 * Follows the Wayland sockets of every user under /run/user (the login screen's included) and keeps
 * one connection to each compositor.
 */
struct display_watch {
	int inotify_fd;
	int root_watch;
	double rescan_at;
	int socket_count;
	struct watched_socket {
		char path[DISPLAY_WATCH_PATH_MAX];
		uid_t owner;
		enum connect_result state;
		struct display *display;
	} sockets[DISPLAY_WATCH_MAX];
};

bool display_watch_init(struct display_watch *watch, double now);
int display_watch_fd(const struct display_watch *watch);
void display_watch_handle_events(struct display_watch *watch, double now);
/* Connects to new compositors when a rescan is due. Returns true when it connected to any. */
bool display_watch_update(struct display_watch *watch, double now);
double display_watch_next_deadline(const struct display_watch *watch);

/* Fills displays and the uid owning each one; returns how many. */
int display_watch_displays(const struct display_watch *watch, struct display **displays, uid_t *owners);
/* Forgets a display whose connection is gone, and looks again in case its compositor restarted. */
void display_watch_drop(struct display_watch *watch, struct display *display, double now);
