/* SPDX-License-Identifier: MIT */

#define _GNU_SOURCE

#include "peripheral_ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

struct osa_ipc_wire_header {
	uint32_t magic;
	uint16_t version;
	uint16_t message_type;
	uint16_t service_id;
	uint16_t flags;
	uint32_t request_id;
	uint32_t payload_length;
	int32_t status;
	uint64_t instance_id;
} __attribute__((packed));

_Static_assert(sizeof(struct osa_ipc_wire_header) == 32U,
		"OSA IPC wire header must remain 32 bytes");

static uint64_t osa_htonll(uint64_t value)
{
	uint32_t high = htonl((uint32_t)(value >> 32));
	uint32_t low = htonl((uint32_t)value);

	return ((uint64_t)low << 32) | high;
}

static uint64_t osa_ntohll(uint64_t value)
{
	return osa_htonll(value);
}

static int osa_wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd pollfd = { .fd = fd, .events = events };
	int ret;

	for (;;) {
		ret = poll(&pollfd, 1, timeout_ms);
		if (ret >= 0)
			break;
		if (errno != EINTR)
			return -errno;
	}
	if (!ret)
		return -ETIMEDOUT;
	if (pollfd.revents & POLLNVAL)
		return -EBADF;
	/* A one-shot peer may send its final packet and close before poll().
	 * POLLIN and POLLHUP can then be set together; drain the queued packet
	 * before reporting closure. This applies to READY/HEARTBEAT reports and
	 * to the supervisor's final health snapshot alike.
	 */
	if (pollfd.revents & events)
		return 0;
	if (pollfd.revents & (POLLERR | POLLHUP))
		return -ECONNRESET;
	return -EIO;
}

static int osa_copy_socket_path(char *destination, size_t capacity,
				const char *path)
{
	if (!path || !path[0] || strlen(path) >= capacity)
		return -EINVAL;
	strncpy(destination, path, capacity - 1U);
	destination[capacity - 1U] = '\0';
	return 0;
}

static void osa_encode_header(struct osa_ipc_wire_header *wire,
				      enum osa_ipc_message_type message_type,
				      enum osa_ipc_service_id service_id, uint16_t flags,
				      uint32_t request_id, uint64_t instance_id,
				      int32_t status, size_t payload_length)
{
	wire->magic = htonl(OSA_IPC_MAGIC);
	wire->version = htons(OSA_IPC_PROTOCOL_VERSION);
	wire->message_type = htons((uint16_t)message_type);
	wire->service_id = htons((uint16_t)service_id);
	wire->flags = htons(flags);
	wire->request_id = htonl(request_id);
	wire->payload_length = htonl((uint32_t)payload_length);
	wire->status = htonl((uint32_t)status);
	wire->instance_id = osa_htonll(instance_id);
}

static int osa_decode_header(const struct osa_ipc_wire_header *wire,
				     struct osa_ipc_header *header)
{
	header->magic = ntohl(wire->magic);
	header->version = ntohs(wire->version);
	header->message_type = ntohs(wire->message_type);
	header->service_id = ntohs(wire->service_id);
	header->flags = ntohs(wire->flags);
	header->request_id = ntohl(wire->request_id);
	header->payload_length = ntohl(wire->payload_length);
	header->status = (int32_t)ntohl((uint32_t)wire->status);
	header->instance_id = osa_ntohll(wire->instance_id);
	if (header->magic != OSA_IPC_MAGIC ||
	    header->version != OSA_IPC_PROTOCOL_VERSION ||
	    header->payload_length > OSA_IPC_MAX_PAYLOAD)
		return -EPROTO;
	if (header->message_type > OSA_IPC_MESSAGE_HEALTH_SNAPSHOT ||
	    header->service_id > OSA_IPC_SERVICE_SYSTEM_MANAGER)
		return -EPROTO;
	return 0;
}

int osa_ipc_server_open(struct osa_ipc_server *server, const char *path,
			mode_t mode)
{
	struct sockaddr_un address;
	int ret;

	if (!server)
		return -EINVAL;
	memset(server, 0, sizeof(*server));
	server->fd = -1;
	ret = osa_copy_socket_path(server->path, sizeof(server->path), path);
	if (ret)
		return ret;
	server->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (server->fd < 0)
		return -errno;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, server->path, sizeof(address.sun_path) - 1U);
	(void)unlink(server->path);
	if (bind(server->fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(server->fd, 8) < 0) {
		ret = -errno;
		osa_ipc_server_close(server);
		return ret;
	}
	if (chmod(server->path, mode) < 0) {
		ret = -errno;
		osa_ipc_server_close(server);
		return ret;
	}
	return 0;
}

void osa_ipc_server_close(struct osa_ipc_server *server)
{
	if (!server)
		return;
	if (server->fd >= 0)
		close(server->fd);
	server->fd = -1;
	if (server->path[0])
		(void)unlink(server->path);
}

int osa_ipc_server_accept(const struct osa_ipc_server *server,
			 int timeout_ms)
{
	int ret;

	if (!server || server->fd < 0)
		return -EINVAL;
	ret = osa_wait_fd(server->fd, POLLIN, timeout_ms);
	if (ret)
		return ret;
	return accept4(server->fd, NULL, NULL, SOCK_CLOEXEC);
}

int osa_ipc_connect(const char *path, int timeout_ms)
{
	struct sockaddr_un address;
	int fd;
	int ret;

	if (!path || !path[0] || strlen(path) >= sizeof(address.sun_path))
		return -EINVAL;
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, path, sizeof(address.sun_path) - 1U);
	if (timeout_ms >= 0) {
		struct timeval timeout = {
			.tv_sec = timeout_ms / 1000,
			.tv_usec = (timeout_ms % 1000) * 1000,
		};
		(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
				 sizeof(timeout));
	}
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}
	return fd;
}

int osa_ipc_send(int fd, enum osa_ipc_message_type message_type,
		enum osa_ipc_service_id service_id, uint16_t flags,
		uint32_t request_id, uint64_t instance_id, int32_t status,
		const void *payload, size_t payload_length)
{
	struct osa_ipc_wire_header header;
	struct iovec vectors[2];
	struct msghdr message;
	ssize_t sent;

	if (fd < 0 || payload_length > OSA_IPC_MAX_PAYLOAD ||
		(payload_length && !payload))
		return -EINVAL;
	osa_encode_header(&header, message_type, service_id, flags, request_id,
				  instance_id, status, payload_length);
	memset(&message, 0, sizeof(message));
	vectors[0].iov_base = &header;
	vectors[0].iov_len = sizeof(header);
	vectors[1].iov_base = (void *)payload;
	vectors[1].iov_len = payload_length;
	message.msg_iov = vectors;
	message.msg_iovlen = payload_length ? 2U : 1U;
	sent = sendmsg(fd, &message, MSG_NOSIGNAL);
	if (sent < 0)
		return -errno;
	if ((size_t)sent != sizeof(header) + payload_length)
		return -EIO;
	return 0;
}

int osa_ipc_receive(int fd, struct osa_ipc_message *message, int timeout_ms)
{
	struct osa_ipc_wire_header header;
	uint8_t buffer[sizeof(header) + OSA_IPC_MAX_PAYLOAD];
	ssize_t received;
	int ret;

	if (fd < 0 || !message)
		return -EINVAL;
	ret = osa_wait_fd(fd, POLLIN, timeout_ms);
	if (ret)
		return ret;
	received = recv(fd, buffer, sizeof(buffer), MSG_TRUNC);
	if (received < 0)
		return -errno;
	if (!received)
		return -ECONNRESET;
	if ((size_t)received < sizeof(header) ||
		(size_t)received > sizeof(buffer))
		return -EMSGSIZE;
	memcpy(&header, buffer, sizeof(header));
	memset(message, 0, sizeof(*message));
	ret = osa_decode_header(&header, &message->header);
	if (ret)
		return ret;
	if ((size_t)received != sizeof(header) + message->header.payload_length)
		return -EPROTO;
	if (message->header.payload_length)
		memcpy(message->payload, buffer + sizeof(header),
		       message->header.payload_length);
	return 0;
}

int osa_ipc_report_health(const char *path,
		enum osa_ipc_message_type message_type,
		enum osa_ipc_service_id service_id, uint64_t instance_id)
{
	struct osa_ipc_health_payload payload;
	int fd;
	int ret;

	if (message_type < OSA_IPC_MESSAGE_HEALTH_READY ||
	    message_type > OSA_IPC_MESSAGE_HEALTH_STOP)
		return -EINVAL;
	fd = osa_ipc_connect(path, 1000);
	if (fd < 0)
		return fd;
	memset(&payload, 0, sizeof(payload));
	payload.pid = (uint32_t)getpid();
	payload.started_ms = instance_id;
	ret = osa_ipc_send(fd, message_type, service_id, 0U, 0U, instance_id,
			   0, &payload, sizeof(payload));
	close(fd);
	return ret;
}
