#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Sends messages to the howdy-island service, in the same datagram format Howdy's island_feed.py
 * uses. Each message carries the uid of the process Gaze is authenticating for, so the island
 * applies the same rules as with Howdy: a user can only show a scan of their own, and only root
 * (sudo, polkit, the login screen) reaches the login screen.
 */
struct island_link {
	int fd;
	uid_t sender_uid;
};

bool island_link_open(struct island_link *link);
void island_link_send(const struct island_link *link, char kind, const void *payload, size_t length);
void island_link_send_text(const struct island_link *link, char kind, const char *text);
