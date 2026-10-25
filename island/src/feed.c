#include "feed.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SYSTEMD_FIRST_FD 3
#define MAX_MESSAGE_BYTES 65536
#define FRAME_HEADER_BYTES 4
#define VALUES_PER_FACE 4

static bool started_by_systemd(void)
{
	const char *listen_pid = getenv("LISTEN_PID");
	const char *listen_fds = getenv("LISTEN_FDS");
	return listen_pid && listen_fds && atoi(listen_pid) == getpid() && atoi(listen_fds) == 1;
}

int feed_open(const char *socket_path)
{
	if (started_by_systemd()) {
		fcntl(SYSTEMD_FIRST_FD, F_SETFL, fcntl(SYSTEMD_FIRST_FD, F_GETFL) | O_NONBLOCK);
		fcntl(SYSTEMD_FIRST_FD, F_SETFD, FD_CLOEXEC);
		return SYSTEMD_FIRST_FD;
	}

	struct sockaddr_un address = {.sun_family = AF_UNIX};
	if (snprintf(address.sun_path, sizeof address.sun_path, "%s", socket_path) >= (int)sizeof address.sun_path)
		return -1;

	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	unlink(socket_path);
	if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0 || chmod(socket_path, 0600) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Frame message: width and height as little-endian 16-bit numbers, then one gray byte per pixel. */
static void show_frame(struct island *island, const uint8_t *payload, size_t length)
{
	if (length < FRAME_HEADER_BYTES)
		return;
	int width = payload[0] | payload[1] << 8;
	int height = payload[2] | payload[3] << 8;
	if ((size_t)width * height != length - FRAME_HEADER_BYTES)
		return;
	island_show_frame(island, width, height, payload + FRAME_HEADER_BYTES);
}

static bool read_face(const char **text, struct face_box *face)
{
	double values[VALUES_PER_FACE];
	for (int i = 0; i < VALUES_PER_FACE; i++) {
		char *end;
		values[i] = strtod(*text, &end);
		if (end == *text)
			return false;
		*text = end;
	}
	*face = (struct face_box){values[0], values[1], values[2], values[3]};
	return true;
}

static void show_faces(struct island *island, const char *text)
{
	struct face_box faces[ISLAND_MAX_FACES];
	int count = 0;
	while (count < ISLAND_MAX_FACES && read_face(&text, &faces[count]))
		count++;
	island_show_faces(island, faces, count);
}

static void begin(struct island *island, const char *text)
{
	char *user;
	long uid = strtol(text, &user, 10);
	if (user == text || *user != ' ' || uid < 0)
		return;
	island_begin(island, (uid_t)uid, user + 1);
}

static void show_scan(struct island *island, const char *text)
{
	double timeout = 0;
	double threshold = 0;
	if (sscanf(text, "%lf %lf", &timeout, &threshold) == 2)
		island_scan(island, timeout, threshold);
}

static void dispatch(struct island *island, char kind, const uint8_t *payload, size_t length)
{
	const char *text = (const char *)payload;
	switch (kind) {
	case 'S':
		begin(island, text);
		break;
	case 'R':
		show_scan(island, text);
		break;
	case 'G':
		show_frame(island, payload, length);
		break;
	case 'D':
		show_faces(island, text);
		break;
	case 'C':
		island_show_certainty(island, strtod(text, NULL));
		break;
	case 'E':
		island_finish(island, (int)strtol(text, NULL, 10));
		break;
	case 'N':
		island_enrol(island);
		break;
	case 'P':
		island_show_enrolment_progress(island, strtod(text, NULL));
		break;
	case 'A':
		island_finish_enrolment(island, text);
		break;
	}
}

int feed_receive(int fd, struct island *island)
{
	static uint8_t message[MAX_MESSAGE_BYTES + 1];
	int count = 0;
	for (;;) {
		ssize_t length = recv(fd, message, MAX_MESSAGE_BYTES, 0);
		if (length < 0)
			return count;
		if (length == 0)
			continue;

		message[length] = '\0';
		dispatch(island, (char)message[0], message + 1, (size_t)length - 1);
		count++;
	}
}
