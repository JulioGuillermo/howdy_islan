#include "gaze_watch.h"

#include <pwd.h>
#include <stdio.h>
#include <string.h>

#include "preview.h"

#define GAZE_BUS_NAME "com.gundulabs.Gaze"
#define CAMERA_TIMEOUT_SECONDS 12
#define HINT_REPEAT_SECONDS 2.0
#define NOT_RECOGNISED_CODE 11
#define TOO_DARK_CODE 13
#define FRAME_HEADER_BYTES 4

struct text_pair {
	const char *key;
	const char *text;
};

static const struct text_pair FACE_HINTS[] = {
	{"no-face", "Look at the camera"},
	{"too-dark", "Too dark"},
	{"clipped", "Move back a little"},
	{"not-centered", "Center your face"},
	{"too-far", "Come closer"},
	{"too-close", "Move back a little"},
	{"ready", "Hold still"},
	{"usable", "Hold still"},
};

static const struct text_pair ENROLMENT_PROMPTS[] = {
	{"look-straight", "Face the camera"},
	{"look-up", "Tilt your face slightly up"},
	{"look-down", "Tilt your face slightly down"},
	{"look-left", "Turn your face slightly left"},
	{"look-right", "Turn your face slightly right"},
	{"captured", "Captured"},
};

#define COUNT(array) (sizeof(array) / sizeof *(array))

static const char *lookup(const struct text_pair *pairs, size_t count, const char *key)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(pairs[i].key, key) == 0)
			return pairs[i].text;
	}
	return NULL;
}

static bool is_working(const struct gaze_watch *watch)
{
	return watch->activity != GAZE_IDLE;
}

static void show_hint(struct gaze_watch *watch, const char *hint, double now)
{
	if (hint)
		snprintf(watch->hint, sizeof watch->hint, "%s", hint);
	island_link_send_text(&watch->island, 'H', watch->hint);
	watch->hint_sent_at = now;
}

static void finish(struct gaze_watch *watch, char kind, const char *text)
{
	island_link_send_text(&watch->island, kind, text);
	watch->activity = GAZE_IDLE;
}

static void remember_daemon(struct gaze_watch *watch)
{
	watch->daemon_name[0] = '\0';
	sd_bus_message *reply = NULL;
	const char *owner = NULL;
	int result = sd_bus_call_method(watch->query_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetNameOwner", NULL, &reply, "s", GAZE_BUS_NAME);
	if (result >= 0 && sd_bus_message_read(reply, "s", &owner) >= 0)
		snprintf(watch->daemon_name, sizeof watch->daemon_name, "%s", owner);
	sd_bus_message_unref(reply);
}

static void start(struct gaze_watch *watch, enum gaze_activity activity, double now)
{
	struct passwd *user = getpwnam(watch->claim_user);
	if (!user)
		return;
	remember_daemon(watch);

	watch->activity = activity;
	watch->showing_gray_camera = false;
	watch->hint[0] = '\0';
	watch->island.sender_uid = watch->claim_caller_uid;

	char begin[GAZE_TEXT_MAX + 16];
	snprintf(begin, sizeof begin, "%u %s", (unsigned)user->pw_uid, watch->claim_user);
	island_link_send_text(&watch->island, 'S', begin);
	if (activity == GAZE_VERIFYING) {
		char scan[32];
		snprintf(scan, sizeof scan, "%d 0", CAMERA_TIMEOUT_SECONDS);
		island_link_send_text(&watch->island, 'R', scan);
	} else {
		island_link_send_text(&watch->island, 'N', "");
	}
	watch->hint_sent_at = now;
}

static void remember_claim(struct gaze_watch *watch, sd_bus_message *call, const char *sender)
{
	const char *user = NULL;
	sd_bus_creds *creds = NULL;
	uid_t caller_uid;
	bool known = sd_bus_message_read(call, "s", &user) >= 0 &&
		sd_bus_get_name_creds(watch->query_bus, sender, SD_BUS_CREDS_EUID, &creds) >= 0 &&
		sd_bus_creds_get_euid(creds, &caller_uid) >= 0;
	sd_bus_creds_unref(creds);
	if (!known)
		return;

	snprintf(watch->claim_sender, sizeof watch->claim_sender, "%s", sender);
	snprintf(watch->claim_user, sizeof watch->claim_user, "%s", user);
	watch->claim_caller_uid = caller_uid;
}

static void handle_call(struct gaze_watch *watch, sd_bus_message *call, const char *member, const char *sender,
	double now)
{
	if (strcmp(member, "Claim") == 0) {
		remember_claim(watch, call, sender);
		return;
	}
	if (strcmp(sender, watch->claim_sender) != 0)
		return;

	if (strncmp(member, "VerifyStart", strlen("VerifyStart")) == 0)
		start(watch, GAZE_VERIFYING, now);
	else if (strcmp(member, "EnrollStart") == 0)
		start(watch, GAZE_ENROLLING, now);
	else if (strcmp(member, "Release") == 0 && is_working(watch))
		finish(watch, 'X', "");
}

static void show_verify_result(struct gaze_watch *watch, sd_bus_message *signal)
{
	const char *result = NULL;
	const char *rgb_status = NULL;
	const char *ir_status = NULL;
	if (sd_bus_message_read(signal, "s", &result) < 0 || sd_bus_message_skip(signal, "a(sddbddb)") < 0 ||
		sd_bus_message_read(signal, "ss", &rgb_status, &ir_status) < 0)
		return;

	bool cancelled = strcmp(rgb_status, "unused") == 0 && strcmp(ir_status, "unused") == 0;
	bool too_dark = strcmp(rgb_status, "too-dark") == 0 || strcmp(ir_status, "too-dark") == 0;
	char code[16];
	if (strcmp(result, "verify-match") == 0) {
		finish(watch, 'E', "0");
	} else if (cancelled) {
		finish(watch, 'X', "");
	} else {
		snprintf(code, sizeof code, "%d", too_dark ? TOO_DARK_CODE : NOT_RECOGNISED_CODE);
		finish(watch, 'E', code);
	}
}

static void show_enrolment_status(struct gaze_watch *watch, sd_bus_message *signal, double now)
{
	const char *face_name = NULL;
	uint32_t progress = 0;
	uint32_t steps = 0;
	int done = 0;
	const char *prompt = NULL;
	double time_remaining = 0;
	if (sd_bus_message_read(signal, "suubsd", &face_name, &progress, &steps, &done, &prompt, &time_remaining) < 0)
		return;

	if (steps > 0) {
		char fraction[16];
		snprintf(fraction, sizeof fraction, "%.3f", (double)progress / steps);
		island_link_send_text(&watch->island, 'P', fraction);
	}
	if (!done) {
		show_hint(watch, lookup(ENROLMENT_PROMPTS, COUNT(ENROLMENT_PROMPTS), prompt), now);
		return;
	}

	if (strcmp(prompt, "completed") == 0)
		finish(watch, 'A', "added");
	else if (strcmp(prompt, "cancelled") == 0)
		finish(watch, 'X', "");
	else
		finish(watch, 'A', prompt);
}

/* Enrolment streams both cameras; once the IR camera has shown up, only it is shown. */
static void show_preview(struct gaze_watch *watch, sd_bus_message *signal)
{
	const void *jpeg = NULL;
	size_t length = 0;
	static struct preview preview;
	if (sd_bus_message_read_array(signal, 'y', &jpeg, &length) < 0 || !preview_from_jpeg(&preview, jpeg, length))
		return;
	if (watch->showing_gray_camera && !preview.from_gray_camera)
		return;
	watch->showing_gray_camera = preview.from_gray_camera;

	static uint8_t frame[FRAME_HEADER_BYTES + sizeof preview.gray];
	frame[0] = preview.width & 0xff;
	frame[1] = preview.width >> 8;
	frame[2] = preview.height & 0xff;
	frame[3] = preview.height >> 8;
	size_t pixels = (size_t)preview.width * preview.height;
	memcpy(frame + FRAME_HEADER_BYTES, preview.gray, pixels);
	island_link_send(&watch->island, 'G', frame, FRAME_HEADER_BYTES + pixels);
}

static void handle_signal(struct gaze_watch *watch, sd_bus_message *signal, const char *member, double now)
{
	if (strcmp(member, "FaceStatus") == 0) {
		const char *status = NULL;
		if (sd_bus_message_read(signal, "s", &status) >= 0)
			show_hint(watch, lookup(FACE_HINTS, COUNT(FACE_HINTS), status), now);
	} else if (strcmp(member, "PreviewFrame") == 0) {
		show_preview(watch, signal);
	} else if (strcmp(member, "VerifyStatus") == 0 && watch->activity == GAZE_VERIFYING) {
		show_verify_result(watch, signal);
	} else if (strcmp(member, "EnrollStatus") == 0 && watch->activity == GAZE_ENROLLING) {
		show_enrolment_status(watch, signal, now);
	}
}

void gaze_watch_handle(struct gaze_watch *watch, sd_bus_message *message, double now)
{
	const char *interface = sd_bus_message_get_interface(message);
	const char *member = sd_bus_message_get_member(message);
	const char *sender = sd_bus_message_get_sender(message);
	uint8_t type = 0;
	if (!interface || !member || !sender || strcmp(interface, GAZE_BUS_NAME) != 0 ||
		sd_bus_message_get_type(message, &type) < 0)
		return;

	if (type == SD_BUS_MESSAGE_METHOD_CALL)
		handle_call(watch, message, member, sender, now);
	else if (type == SD_BUS_MESSAGE_SIGNAL && is_working(watch) && strcmp(sender, watch->daemon_name) == 0)
		handle_signal(watch, message, member, now);
}

void gaze_watch_tick(struct gaze_watch *watch, double now)
{
	if (is_working(watch) && now - watch->hint_sent_at >= HINT_REPEAT_SECONDS)
		show_hint(watch, NULL, now);
}
