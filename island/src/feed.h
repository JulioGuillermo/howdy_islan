#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include "island.h"

/*
 * The datagram socket Howdy's island_feed.py writes to. Anyone may write: Howdy runs as root under
 * sudo, polkit and the login screen, but as the user under a screen locker. The kernel stamps each
 * message with its sender, so a user can only show a scan of their own, and only root reaches the
 * login screen.
 */
struct feed {
	int fd;
	uid_t scan_sender;
};

/* Uses the socket systemd passes, else binds a new one at socket_path. */
bool feed_open(struct feed *feed, const char *socket_path);

/* Applies every pending message to the island. Returns how many there were. */
int feed_receive(struct feed *feed, struct island *island);

/* Whether the scan on the island was started by root, which only then may show it on the login screen. */
bool feed_scan_started_by_root(const struct feed *feed);
