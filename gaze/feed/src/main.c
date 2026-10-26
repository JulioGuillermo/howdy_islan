/*
 * gaze-island-feed: shows Gaze's face scans and enrolments on howdy-island. Gaze has no hooks to
 * patch, so this watches its D-Bus traffic as a bus monitor (root only) and passes it on to the
 * island in the messages Howdy's feed sends.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gaze_watch.h"

#define TICK_MICROSECONDS 500000

static const char *const MONITOR_RULES[] = {
	"type='method_call',interface='com.gundulabs.Gaze'",
	"type='signal',interface='com.gundulabs.Gaze'",
};

static double monotonic_seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int become_monitor(sd_bus *bus)
{
	sd_bus_message *call = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int result = sd_bus_message_new_method_call(bus, &call, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus.Monitoring", "BecomeMonitor");
	if (result >= 0)
		result = sd_bus_message_open_container(call, 'a', "s");
	for (size_t i = 0; result >= 0 && i < sizeof MONITOR_RULES / sizeof *MONITOR_RULES; i++)
		result = sd_bus_message_append(call, "s", MONITOR_RULES[i]);
	if (result >= 0)
		result = sd_bus_message_close_container(call);
	if (result >= 0)
		result = sd_bus_message_append(call, "u", 0);
	if (result >= 0)
		result = sd_bus_call(bus, call, 0, &error, NULL);
	if (result < 0)
		fprintf(stderr, "gaze-island-feed: cannot monitor the system bus: %s\n",
			error.message ? error.message : strerror(-result));
	sd_bus_error_free(&error);
	sd_bus_message_unref(call);
	return result;
}

static int open_monitor_bus(sd_bus **bus)
{
	int result = sd_bus_new(bus);
	if (result >= 0)
		result = sd_bus_set_monitor(*bus, true);
	if (result >= 0)
		result = sd_bus_set_address(*bus, "unix:path=/run/dbus/system_bus_socket");
	if (result >= 0)
		result = sd_bus_set_bus_client(*bus, true);
	if (result >= 0)
		result = sd_bus_start(*bus);
	if (result >= 0)
		result = become_monitor(*bus);
	return result;
}

int main(void)
{
	sd_bus *monitor_bus = NULL;
	struct gaze_watch watch = {.activity = GAZE_IDLE};
	if (open_monitor_bus(&monitor_bus) < 0 || sd_bus_open_system(&watch.query_bus) < 0 ||
		!island_link_open(&watch.island)) {
		fprintf(stderr, "gaze-island-feed: cannot start\n");
		return 1;
	}

	for (;;) {
		sd_bus_message *message = NULL;
		int result = sd_bus_process(monitor_bus, &message);
		if (result < 0) {
			fprintf(stderr, "gaze-island-feed: lost the system bus: %s\n", strerror(-result));
			return 1;
		}
		if (message) {
			gaze_watch_handle(&watch, message, monotonic_seconds());
			sd_bus_message_unref(message);
		}
		gaze_watch_tick(&watch, monotonic_seconds());
		if (result > 0)
			continue;

		result = sd_bus_wait(monitor_bus, TICK_MICROSECONDS);
		if (result < 0 && result != -EINTR) {
			fprintf(stderr, "gaze-island-feed: lost the system bus: %s\n", strerror(-result));
			return 1;
		}
	}
}
