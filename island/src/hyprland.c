#include "hyprland.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define USER_RUNTIME_ROOT "/run/user"
#define REPLY_TIMEOUT_MICROSECONDS 300000

static const char ABOVE_LOCK_REQUEST[] =
	"/eval hl.layer_rule({ name = \"howdy-island\", match = { namespace = \"^howdy-island$\" }, "
	"above_lock = 1, no_anim = true })";

static void send_request(const char *socket_path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	if (snprintf(address.sun_path, sizeof address.sun_path, "%s", socket_path) >= (int)sizeof address.sun_path)
		return;

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;

	struct timeval timeout = {.tv_usec = REPLY_TIMEOUT_MICROSECONDS};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

	if (connect(fd, (struct sockaddr *)&address, sizeof address) == 0 &&
		write(fd, ABOVE_LOCK_REQUEST, sizeof ABOVE_LOCK_REQUEST - 1) > 0) {
		char reply[256];
		while (read(fd, reply, sizeof reply) > 0) {
		}
	}
	close(fd);
}

static void allow_in_user_instances(const char *user_dir_name)
{
	char instances_path[256];
	snprintf(instances_path, sizeof instances_path, "%s/%s/hypr", USER_RUNTIME_ROOT, user_dir_name);
	DIR *instances = opendir(instances_path);
	if (!instances)
		return;

	struct dirent *instance;
	while ((instance = readdir(instances))) {
		if (instance->d_name[0] == '.')
			continue;
		char socket_path[512];
		snprintf(socket_path, sizeof socket_path, "%s/%s/.socket.sock", instances_path, instance->d_name);
		send_request(socket_path);
	}
	closedir(instances);
}

void hyprland_allow_above_lock(void)
{
	DIR *root = opendir(USER_RUNTIME_ROOT);
	if (!root)
		return;

	struct dirent *user_dir;
	while ((user_dir = readdir(root))) {
		if (user_dir->d_name[0] != '.')
			allow_in_user_instances(user_dir->d_name);
	}
	closedir(root);
}
