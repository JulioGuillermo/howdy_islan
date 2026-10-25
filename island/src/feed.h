#pragma once

#include "island.h"

/*
 * The datagram socket Howdy's island_feed.py writes to: the one systemd passes, else a new one bound
 * at socket_path. Returns the fd, or -1.
 */
int feed_open(const char *socket_path);

/* Applies every pending message to the island. Returns how many there were. */
int feed_receive(int fd, struct island *island);
