#include "island_link.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define ISLAND_SOCKET_PATH "/run/howdy-island/island.sock"
#define MAX_MESSAGE_BYTES 65536

bool island_link_open(struct island_link *link)
{
	link->sender_uid = 0;
	link->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	return link->fd >= 0;
}

static bool island_is_listening(void)
{
	struct stat info;
	return lstat(ISLAND_SOCKET_PATH, &info) == 0 && S_ISSOCK(info.st_mode) && info.st_uid == 0;
}

void island_link_send(const struct island_link *link, char kind, const void *payload, size_t length)
{
	static char message[MAX_MESSAGE_BYTES];
	if (length + 1 > sizeof message || !island_is_listening())
		return;
	message[0] = kind;
	memcpy(message + 1, payload, length);

	struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = ISLAND_SOCKET_PATH};
	struct iovec data = {.iov_base = message, .iov_len = length + 1};
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(struct ucred))];
	} control = {0};
	struct msghdr header = {
		.msg_name = &address,
		.msg_namelen = sizeof address,
		.msg_iov = &data,
		.msg_iovlen = 1,
		.msg_control = control.space,
		.msg_controllen = sizeof control.space,
	};

	struct cmsghdr *credentials_header = CMSG_FIRSTHDR(&header);
	credentials_header->cmsg_level = SOL_SOCKET;
	credentials_header->cmsg_type = SCM_CREDENTIALS;
	credentials_header->cmsg_len = CMSG_LEN(sizeof(struct ucred));
	struct ucred credentials = {.pid = getpid(), .uid = link->sender_uid, .gid = getgid()};
	memcpy(CMSG_DATA(credentials_header), &credentials, sizeof credentials);

	sendmsg(link->fd, &header, MSG_DONTWAIT);
}

void island_link_send_text(const struct island_link *link, char kind, const char *text)
{
	island_link_send(link, kind, text, strlen(text));
}
