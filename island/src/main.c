/*
 * howdy-island: a notch always at the top of every Wayland session on this machine, the login
 * screen's included, that opens with the live face scan (Howdy or Gaze). A scan opens it only in
 * the sessions of the user being authenticated and on the login screen; elsewhere it stays at rest.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>

#include "display_watch.h"
#include "feed.h"
#include "hyprland.h"
#include "island.h"
#include "wayland.h"

#define DEFAULT_SOCKET_PATH "/run/howdy-island/island.sock"
#define LOGIN_SCREEN_USER "greeter"
#define NO_UID ((uid_t)-1)
#define FIXED_POLL_FDS 2

static double monotonic_seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static double earliest(double first, double second)
{
	if (first <= 0)
		return second;
	if (second <= 0)
		return first;
	return fmin(first, second);
}

static int poll_timeout_ms(double deadline, double now)
{
	if (deadline <= 0)
		return -1;
	double milliseconds = ceil((deadline - now) * 1000);
	return (int)fmax(0, fmin(milliseconds, INT_MAX));
}

static uid_t uid_of(const char *user)
{
	struct passwd *entry = getpwnam(user);
	return entry ? entry->pw_uid : NO_UID;
}

static bool sees_scan(uid_t display_owner, const struct island *scan, const struct feed *feed, uid_t login_screen_uid)
{
	if (display_owner == login_screen_uid)
		return feed_scan_started_by_root(feed);
	return display_owner == scan->user_uid;
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);

	static struct island scan;
	static struct island resting;
	double start = monotonic_seconds();
	island_init(&scan, start);
	island_init(&resting, start);
	scan.user_uid = NO_UID;
	uid_t login_screen_uid = uid_of(LOGIN_SCREEN_USER);

	struct feed feed;
	if (!feed_open(&feed, argc > 1 ? argv[1] : DEFAULT_SOCKET_PATH)) {
		perror("howdy-island: cannot open the feed socket");
		return 1;
	}

	struct display_watch watch;
	if (!display_watch_init(&watch, start)) {
		perror("howdy-island: cannot watch /run/user");
		return 1;
	}

	bool was_animating = false;
	for (;;) {
		struct display *displays[DISPLAY_WATCH_MAX];
		uid_t owners[DISPLAY_WATCH_MAX];
		int display_count = display_watch_displays(&watch, displays, owners);

		struct pollfd fds[FIXED_POLL_FDS + DISPLAY_WATCH_MAX];
		fds[0] = (struct pollfd){.fd = feed.fd, .events = POLLIN};
		fds[1] = (struct pollfd){.fd = display_watch_fd(&watch), .events = POLLIN};
		for (int i = 0; i < display_count; i++)
			fds[FIXED_POLL_FDS + i] = (struct pollfd){.fd = display_prepare_poll(displays[i]), .events = POLLIN};

		double now = monotonic_seconds();
		double deadline = earliest(island_next_deadline(&scan), display_watch_next_deadline(&watch));
		if (poll(fds, FIXED_POLL_FDS + display_count, poll_timeout_ms(deadline, now)) < 0 && errno != EINTR) {
			perror("howdy-island: poll");
			return 1;
		}

		now = monotonic_seconds();
		for (int i = 0; i < display_count; i++) {
			struct pollfd *display_fd = &fds[FIXED_POLL_FDS + i];
			bool connection_gone = display_fd->fd < 0 ||
				!display_after_poll(displays[i], display_fd->revents & (POLLIN | POLLHUP | POLLERR));
			if (connection_gone)
				display_watch_drop(&watch, displays[i], now);
		}

		enum island_phase phase_before = scan.phase;
		island_advance(&scan, now);
		int message_count = (fds[0].revents & POLLIN) ? feed_receive(&feed, &scan) : 0;
		if (fds[1].revents & POLLIN)
			display_watch_handle_events(&watch, now);

		bool connected_new_display = display_watch_update(&watch, now);
		bool scan_started = phase_before == ISLAND_REST && scan.phase != ISLAND_REST;
		if (connected_new_display || scan_started)
			hyprland_allow_above_lock();

		display_count = display_watch_displays(&watch, displays, owners);
		bool animating = island_is_animating(&scan);
		bool changed = message_count > 0 || scan.phase != phase_before || (was_animating && !animating);
		was_animating = animating;

		bool audience_may_have_changed = scan.phase != phase_before;
		for (int i = 0; i < display_count; i++) {
			bool sees = sees_scan(owners[i], &scan, &feed, login_screen_uid);
			if ((changed && sees) || audience_may_have_changed)
				display_mark_dirty(displays[i]);
			display_render(displays[i], sees ? &scan : &resting);
		}
	}
}
