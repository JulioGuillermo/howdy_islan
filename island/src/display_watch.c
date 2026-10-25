#include "display_watch.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define USER_RUNTIME_ROOT "/run/user"
#define RESCAN_DELAY_SECONDS 0.3
#define RETRY_SECONDS 5.0
#define SAFETY_RESCAN_SECONDS 30.0
#define WAYLAND_PREFIX "wayland-"
#define LOCK_SUFFIX ".lock"
#define WATCHED_EVENTS (IN_CREATE | IN_MOVED_TO)

static bool is_wayland_socket_name(const char *name)
{
	size_t length = strlen(name);
	size_t suffix_length = strlen(LOCK_SUFFIX);
	if (strncmp(name, WAYLAND_PREFIX, strlen(WAYLAND_PREFIX)) != 0)
		return false;
	return length < suffix_length || strcmp(name + length - suffix_length, LOCK_SUFFIX) != 0;
}

static bool is_uid_name(const char *name)
{
	if (!*name)
		return false;
	for (const char *character = name; *character; character++) {
		if (!isdigit((unsigned char)*character))
			return false;
	}
	return true;
}

static void schedule_rescan(struct display_watch *watch, double at)
{
	if (watch->rescan_at <= 0 || at < watch->rescan_at)
		watch->rescan_at = at;
}

static int find_socket(const struct display_watch *watch, const char *path)
{
	for (int index = 0; index < watch->socket_count; index++) {
		if (strcmp(watch->sockets[index].path, path) == 0)
			return index;
	}
	return -1;
}

static int add_socket(struct display_watch *watch, const char *path, uid_t owner)
{
	if (watch->socket_count == DISPLAY_WATCH_MAX)
		return -1;

	int index = watch->socket_count++;
	struct watched_socket *socket = &watch->sockets[index];
	snprintf(socket->path, sizeof socket->path, "%s", path);
	socket->owner = owner;
	socket->state = CONNECT_UNREACHABLE;
	socket->display = NULL;
	return index;
}

static void remove_socket(struct display_watch *watch, int index)
{
	watch->socket_count--;
	watch->sockets[index] = watch->sockets[watch->socket_count];
}

struct scan_result {
	bool seen[DISPLAY_WATCH_MAX];
	bool connected_any;
	bool needs_retry;
};

static void scan_user_dir(struct display_watch *watch, const char *user_dir, struct scan_result *result)
{
	inotify_add_watch(watch->inotify_fd, user_dir, WATCHED_EVENTS);
	DIR *directory = opendir(user_dir);
	if (!directory)
		return;

	struct dirent *entry;
	while ((entry = readdir(directory))) {
		if (!is_wayland_socket_name(entry->d_name))
			continue;

		char path[DISPLAY_WATCH_PATH_MAX];
		struct stat info;
		bool fits = snprintf(path, sizeof path, "%s/%s", user_dir, entry->d_name) < (int)sizeof path;
		if (!fits || stat(path, &info) != 0 || !S_ISSOCK(info.st_mode))
			continue;

		int index = find_socket(watch, path);
		if (index < 0)
			index = add_socket(watch, path, info.st_uid);
		if (index < 0)
			continue;
		result->seen[index] = true;

		struct watched_socket *socket = &watch->sockets[index];
		if (socket->state == CONNECT_UNREACHABLE) {
			socket->state = display_connect(socket->path, &socket->display);
			result->connected_any = result->connected_any || socket->state == CONNECT_OK;
		}
		result->needs_retry = result->needs_retry || socket->state == CONNECT_UNREACHABLE;
	}
	closedir(directory);
}

/* inotify_add_watch on every pass: a user dir gets its tmpfs mounted after it appears, which is a new inode. */
static bool rescan(struct display_watch *watch, double now)
{
	watch->rescan_at = 0;
	struct scan_result result = {0};

	DIR *root = opendir(USER_RUNTIME_ROOT);
	if (root) {
		struct dirent *entry;
		while ((entry = readdir(root))) {
			if (!is_uid_name(entry->d_name))
				continue;
			char user_dir[DISPLAY_WATCH_PATH_MAX];
			snprintf(user_dir, sizeof user_dir, "%s/%s", USER_RUNTIME_ROOT, entry->d_name);
			scan_user_dir(watch, user_dir, &result);
		}
		closedir(root);
	}

	for (int index = watch->socket_count - 1; index >= 0; index--) {
		if (!result.seen[index] && watch->sockets[index].state != CONNECT_OK)
			remove_socket(watch, index);
	}

	schedule_rescan(watch, now + (result.needs_retry ? RETRY_SECONDS : SAFETY_RESCAN_SECONDS));
	return result.connected_any;
}

bool display_watch_init(struct display_watch *watch, double now)
{
	memset(watch, 0, sizeof *watch);
	watch->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watch->inotify_fd < 0)
		return false;
	watch->root_watch = inotify_add_watch(watch->inotify_fd, USER_RUNTIME_ROOT, WATCHED_EVENTS);
	if (watch->root_watch < 0)
		return false;

	schedule_rescan(watch, now);
	return true;
}

int display_watch_fd(const struct display_watch *watch)
{
	return watch->inotify_fd;
}

void display_watch_handle_events(struct display_watch *watch, double now)
{
	char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t length;
	while ((length = read(watch->inotify_fd, buffer, sizeof buffer)) > 0) {
		char *cursor = buffer;
		while (cursor < buffer + length) {
			const struct inotify_event *event = (const struct inotify_event *)cursor;
			bool new_user_dir = event->wd == watch->root_watch;
			bool new_socket = event->len > 0 && is_wayland_socket_name(event->name);
			if (new_user_dir || new_socket)
				schedule_rescan(watch, now + RESCAN_DELAY_SECONDS);
			cursor += sizeof *event + event->len;
		}
	}
}

bool display_watch_update(struct display_watch *watch, double now)
{
	if (watch->rescan_at <= 0 || now < watch->rescan_at)
		return false;
	return rescan(watch, now);
}

double display_watch_next_deadline(const struct display_watch *watch)
{
	return watch->rescan_at;
}

int display_watch_displays(const struct display_watch *watch, struct display **displays, uid_t *owners)
{
	int count = 0;
	for (int index = 0; index < watch->socket_count; index++) {
		if (watch->sockets[index].state != CONNECT_OK)
			continue;
		displays[count] = watch->sockets[index].display;
		owners[count] = watch->sockets[index].owner;
		count++;
	}
	return count;
}

void display_watch_drop(struct display_watch *watch, struct display *display, double now)
{
	for (int index = 0; index < watch->socket_count; index++) {
		if (watch->sockets[index].display == display) {
			display_destroy(display);
			remove_socket(watch, index);
			schedule_rescan(watch, now + RESCAN_DELAY_SECONDS);
			return;
		}
	}
}
