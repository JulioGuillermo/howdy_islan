#pragma once

#include <stdbool.h>

#include "island.h"

/* One compositor connection: an island surface on the internal panel, or where the compositor puts it. */
struct display;

enum connect_result {
	CONNECT_OK,
	CONNECT_UNREACHABLE,
	CONNECT_UNSUPPORTED,
};

enum connect_result display_connect(const char *socket_path, struct display **display);
void display_destroy(struct display *display);

/* Around poll(): returns the fd to wait on, or -1 when the connection is gone. */
int display_prepare_poll(struct display *display);
/* Returns false when the connection is gone. */
bool display_after_poll(struct display *display, bool readable);

void display_mark_dirty(struct display *display);
void display_render(struct display *display, const struct island *island);
