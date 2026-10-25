#include "feed.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SYSTEMD_FIRST_FD 3
#define MAX_MESSAGE_BYTES 65536
#define FRAME_HEADER_BYTES 4
#define VALUES_PER_FACE 4
#define NO_SENDER ((uid_t)-1)

static bool started_by_systemd(void)
{
	const char *listen_pid = getenv("LISTEN_PID");
	const char *listen_fds = getenv("LISTEN_FDS");
	return listen_pid && listen_fds && atoi(listen_pid) == getpid() && atoi(listen_fds) == 1;
}

static int bind_socket(const char *socket_path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	if (snprintf(address.sun_path, sizeof address.sun_path, "%s", socket_path) >= (int)sizeof address.sun_path)
		return -1;

	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	unlink(socket_path);
	if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0 || chmod(socket_path, 0666) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool feed_open(struct feed *feed, const char *socket_path)
{
	feed->scan_sender = NO_SENDER;
	if (started_by_systemd()) {
		feed->fd = SYSTEMD_FIRST_FD;
		fcntl(feed->fd, F_SETFL, fcntl(feed->fd, F_GETFL) | O_NONBLOCK);
		fcntl(feed->fd, F_SETFD, FD_CLOEXEC);
	} else {
		feed->fd = bind_socket(socket_path);
	}

	int enabled = 1;
	return feed->fd >= 0 && setsockopt(feed->fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof enabled) == 0;
}

bool feed_scan_started_by_root(const struct feed *feed)
{
	return feed->scan_sender == 0;
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

/* A new scan replaces the running one. Root may scan anyone; a user, only themselves. */
static void begin(struct feed *feed, struct island *island, uid_t sender, const char *text)
{
	char *user;
	long uid = strtol(text, &user, 10);
	if (user == text || *user != ' ' || uid < 0)
		return;
	if (sender != 0 && sender != (uid_t)uid)
		return;

	feed->scan_sender = sender;
	island_begin(island, (uid_t)uid, user + 1);
}

static void show_scan(struct island *island, const char *text)
{
	double timeout = 0;
	double threshold = 0;
	if (sscanf(text, "%lf %lf", &timeout, &threshold) == 2)
		island_scan(island, timeout, threshold);
}

static void dispatch(struct feed *feed, struct island *island, uid_t sender, char kind, const uint8_t *payload,
	size_t length)
{
	const char *text = (const char *)payload;
	if (kind == 'S') {
		begin(feed, island, sender, text);
		return;
	}
	if (sender != feed->scan_sender)
		return;

	switch (kind) {
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

/* Reads one datagram and who sent it. Returns its length, or -1 when none is left. */
static ssize_t receive_one(int fd, uint8_t *message, uid_t *sender)
{
	struct iovec data = {.iov_base = message, .iov_len = MAX_MESSAGE_BYTES};
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(struct ucred))];
	} control;
	struct msghdr header = {
		.msg_iov = &data,
		.msg_iovlen = 1,
		.msg_control = control.space,
		.msg_controllen = sizeof control.space,
	};

	ssize_t length = recvmsg(fd, &header, 0);
	if (length < 0)
		return -1;

	*sender = NO_SENDER;
	for (struct cmsghdr *item = CMSG_FIRSTHDR(&header); item; item = CMSG_NXTHDR(&header, item)) {
		if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS) {
			struct ucred credentials;
			memcpy(&credentials, CMSG_DATA(item), sizeof credentials);
			*sender = credentials.uid;
		}
	}
	return length;
}

int feed_receive(struct feed *feed, struct island *island)
{
	static uint8_t message[MAX_MESSAGE_BYTES + 1];
	int count = 0;
	for (;;) {
		uid_t sender;
		ssize_t length = receive_one(feed->fd, message, &sender);
		if (length < 0)
			return count;
		if (length == 0 || sender == NO_SENDER)
			continue;

		message[length] = '\0';
		dispatch(feed, island, sender, (char)message[0], message + 1, (size_t)length - 1);
		count++;
	}
}
