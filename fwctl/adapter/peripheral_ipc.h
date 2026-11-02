/* SPDX-License-Identifier: MIT */
#ifndef EDK_OSA_IPC_H
#define EDK_OSA_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OSA_IPC_DEFAULT_SOCKET "/run/system_manager.sock"
#define OSA_IPC_MAX_PAYLOAD 4096U
#define OSA_IPC_PROTOCOL_VERSION 0U
#define OSA_IPC_MAGIC 0x4f534131U
#define OSA_IPC_HEALTH_NAME_SIZE 32U

enum osa_ipc_message_type {
	OSA_IPC_MESSAGE_REQUEST = 0,
	OSA_IPC_MESSAGE_RESPONSE = 1,
	OSA_IPC_MESSAGE_HEALTH_READY = 2,
	OSA_IPC_MESSAGE_HEALTH_HEARTBEAT = 3,
	OSA_IPC_MESSAGE_HEALTH_STOP = 4,
	OSA_IPC_MESSAGE_HEALTH_QUERY = 5,
	OSA_IPC_MESSAGE_HEALTH_SNAPSHOT = 6,
};

enum osa_ipc_health_state {
	OSA_IPC_HEALTH_PENDING = 0,
	OSA_IPC_HEALTH_READY = 1,
	OSA_IPC_HEALTH_EXPIRED = 2,
	OSA_IPC_HEALTH_RESTARTING = 3,
	OSA_IPC_HEALTH_FAILED = 4,
};

enum osa_ipc_health_flags {
	OSA_IPC_HEALTH_FLAG_NONE = 0,
	OSA_IPC_HEALTH_FLAG_REQUIRED = 1U << 0,
};

enum osa_ipc_service_id {
	OSA_IPC_SERVICE_PTELEMETRYD = 0,
	OSA_IPC_SERVICE_PSATELLITED = 1,
	OSA_IPC_SERVICE_SYSTEM_MANAGER = 2,
};

struct osa_ipc_header {
	uint32_t magic;
	uint16_t version;
	uint16_t message_type;
	uint16_t service_id;
	uint16_t flags;
	uint32_t request_id;
	uint32_t payload_length;
	int32_t status;
	uint64_t instance_id;
};

struct osa_ipc_message {
	struct osa_ipc_header header;
	uint8_t payload[OSA_IPC_MAX_PAYLOAD];
};

struct osa_ipc_health_payload {
	uint32_t pid;
	uint32_t reserved;
	uint64_t started_ms;
};

struct osa_ipc_health_status {
	uint16_t service_id;
	uint16_t state;
	uint32_t pid;
	uint32_t flags;
	uint32_t reserved;
	uint64_t last_seen_ms;
	uint64_t instance_id;
};

struct osa_ipc_server {
	int fd;
	char path[108];
};

/* Create and bind a filesystem Unix SOCK_SEQPACKET listener. */
int osa_ipc_server_open(struct osa_ipc_server *server, const char *path,
			mode_t mode);
void osa_ipc_server_close(struct osa_ipc_server *server);

/* Accept or connect one client. timeout_ms < 0 waits indefinitely. */
int osa_ipc_server_accept(const struct osa_ipc_server *server,
			 int timeout_ms);
int osa_ipc_connect(const char *path, int timeout_ms);

/* Send and receive exactly one protocol message. */
int osa_ipc_send(int fd, enum osa_ipc_message_type message_type,
		enum osa_ipc_service_id service_id, uint16_t flags,
		uint32_t request_id, uint64_t instance_id, int32_t status,
		const void *payload, size_t payload_length);
int osa_ipc_receive(int fd, struct osa_ipc_message *message, int timeout_ms);

/* Convenience API for a one-shot service READY/HEARTBEAT/STOP event. */
int osa_ipc_report_health(const char *path,
		enum osa_ipc_message_type message_type,
		enum osa_ipc_service_id service_id, uint64_t instance_id);

#ifdef __cplusplus
}
#endif

#endif /* EDK_OSA_IPC_H */
