#define _GNU_SOURCE
#include "admission.h"
#include "http.h"
#include "downloads.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CLIENTS 32
#define PEER_CLIENTS 16
#define HEADER_MS 15000
struct pending {
    int fd, draining;
    uint32_t peer;
    int64_t started;
    size_t used;
    char header[sizeof(((struct dm_transfer_http_request *)0)->storage)];
};

static int64_t now_ms(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 :
        (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Admission errors must never wait for a slow peer to read their response. */
static void reject(int fd, int status)
{
    const char *reply = status == 408 ? "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
        status == 431 ? "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
        status == 400 ? "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
        "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    ssize_t ignored = send(fd, reply, strlen(reply), MSG_DONTWAIT | MSG_NOSIGNAL);
    (void)ignored;
}

static void accept_some(int listener, struct pending *clients)
{
    for (int budget = 0; budget < 8; budget++) {
        struct sockaddr_in peer; socklen_t length = sizeof(peer);
        int fd = accept4(listener, (struct sockaddr *)&peer, &length, SOCK_CLOEXEC);
        if (fd < 0) return;
        int available = -1, same_peer = 0;
        for (int i = 0; i < CLIENTS; i++) {
            if (clients[i].fd < 0) { if (available < 0) available = i; }
            else if (clients[i].peer == peer.sin_addr.s_addr) same_peer++;
        }
        int64_t now = now_ms();
        if (available < 0 || same_peer >= PEER_CLIENTS || now < 0) {
            reject(fd, 503); close(fd); continue;
        }
        clients[available] = (struct pending){.fd=fd, .peer=peer.sin_addr.s_addr, .started=now};
    }
}

int server_accept_loop(int listener, const struct server_context *context)
{
    int flags = fcntl(listener, F_GETFL);
    if (flags < 0 || fcntl(listener, F_SETFL, flags | O_NONBLOCK)) return -errno;
    struct pending *clients = calloc(CLIENTS, sizeof(*clients));
    if (!clients) return -ENOMEM;
    for (int i = 0; i < CLIENTS; i++) clients[i].fd = -1;
    int result = 0, cursor = 0;
    while (*context->running) {
        server_downloads_poll();
        int64_t now = now_ms();
        if (now < 0) { result = -EIO; break; }
        struct pollfd events[CLIENTS + 1] = {{.fd=listener, .events=POLLIN}};
        int timeout = 1000;
        for (int i = 0; i < CLIENTS; i++) {
            struct pending *p = &clients[i];
            int limit = p->draining ? 1000 : HEADER_MS;
            if (p->fd >= 0 && now - p->started >= limit) {
                if (!p->draining) reject(p->fd, 408);
                close(p->fd); p->fd = -1;
            }
            events[i+1] = (struct pollfd){.fd=p->fd, .events=POLLIN};
            if (p->fd >= 0 && limit - (now - p->started) < timeout)
                timeout = (int)(limit - (now - p->started));
        }
        int ready = poll(events, CLIENTS + 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { result = -errno; break; }
        if (!*context->running) break;
        if (events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { result = -EIO; break; }
        /* Rotate service order. One bounded header step per connection per
         * turn; there are no threads and no concurrent database mutations. */
        for (int count = 0; count < CLIENTS && *context->running; count++) {
            int i = (cursor + count) % CLIENTS;
            struct pending *p = &clients[i];
            if (p->fd < 0 || events[i+1].fd != p->fd || !events[i+1].revents) continue;
            if (p->draining) {
                char discard[2048];
                ssize_t got = recv(p->fd, discard, sizeof(discard), MSG_DONTWAIT);
                if (got > 0) p->used += (size_t)got;
                if ((!got || (got < 0 && errno != EAGAIN && errno != EINTR)) || p->used >= 65536) {
                    close(p->fd); p->fd = -1;
                }
                continue;
            }
            now = now_ms();
            int rc = now < 0 || now - p->started >= HEADER_MS ? 408 :
                dm_transfer_http_header_step(p->fd, p->header, sizeof(p->header), &p->used);
            if (!rc) continue;
            int deferred = 0;
            if (rc == 1) {
                struct http_request request;
                rc = http_parse_request(p->fd, &request, p->header, p->used);
                if (!rc) {
                    server_downloads_poll();
                    struct server_context owned = *context;
                    owned.response_deferred = &deferred;
                    server_route_request(p->fd, &owned, &request);
                    server_downloads_poll();
                }
            }
            if (rc > 0) reject(p->fd, rc);
            if (deferred || rc < 0) { close(p->fd); p->fd = -1; }
            else {
                /* Deliver a final response before discarding a rejected body's
                 * remaining bytes; never block other clients on lingering IO. */
                (void)shutdown(p->fd, SHUT_WR);
                p->draining = 1; p->used = 0; p->started = now_ms();
            }
        }
        if (events[0].revents & POLLIN) accept_some(listener, clients);
        cursor = (cursor + 1) % CLIENTS;
    }
    for (int i = 0; i < CLIENTS; i++) if (clients[i].fd >= 0) close(clients[i].fd);
    server_downloads_stop();
    free(clients); return result;
}
