/* Minimal host-side HTTP transport for fwctl FIT releases. */
#define _POSIX_C_SOURCE 200809L
#include "ota-admin.h"
#include "sol.h"
#include "ota-device.h"
#include "ota-page.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static const char *server_directory;

static void stop_server(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int send_all(int client, const void *data, size_t length)
{
    const char *cursor = data;
    while (length && running) {
        ssize_t count = send(client, cursor, length, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        cursor += count;
        length -= (size_t)count;
    }
    return length ? -1 : 0;
}

static int headers(int client, int status, const char *reason,
                   const char *type, uintmax_t length)
{
    char buffer[512];
    int count = snprintf(buffer, sizeof(buffer),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %ju\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Allow: GET, HEAD, POST\r\n\r\n", status, reason, type, length);
    if (count < 0 || (size_t)count >= sizeof(buffer))
        return -1;
    return send_all(client, buffer, (size_t)count);
}

static void text_response(int client, int status, const char *reason,
                          const char *body, bool head)
{
    if (!headers(client, status, reason, "text/plain; charset=utf-8", strlen(body)) && !head)
        (void)send_all(client, body, strlen(body));
}

static int open_artifact(int root, const char *name, struct stat *st)
{
    int fd = openat(root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (fstat(fd, st) || !S_ISREG(st->st_mode) || st->st_size <= 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void json_response(int client, int status, const char *reason, const char *body, bool head)
{
    if (!headers(client, status, reason, "application/json; charset=utf-8", strlen(body)) && !head)
        (void)send_all(client, body, strlen(body));
}

static void index_page(int client, bool head)
{
    if (!headers(client, 200, "OK", "text/html; charset=utf-8", sizeof(admin_page) - 1) && !head)
        (void)send_all(client, admin_page, sizeof(admin_page) - 1);
}

static void upload(int client, unsigned long long length)
{
    char temp[OTA_PATH], id[32];
    int fd = ota_draft_begin(server_directory, temp);
    if (fd < 0) {
        json_response(client, 503, "Service Unavailable", "{\"error\":\"无法创建上传目录，请检查空间或清理旧草稿（最多 256 个）。\"}", false);
        return;
    }
    char buffer[65536];
    unsigned long long left = length;
    time_t started = time(NULL);
    int failed = 0;
    while (left && running) {
        size_t wanted = left < sizeof(buffer) ? (size_t)left : sizeof(buffer);
        ssize_t n = recv(client, buffer, wanted, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || time(NULL) - started > 180) {
            failed = 1;
            break;
        }
        size_t pos = 0;
        while (pos < (size_t)n) {
            ssize_t written = write(fd, buffer + pos, (size_t)n - pos);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0) {
                failed = 1;
                break;
            }
            pos += (size_t)written;
        }
        if (failed)
            break;
        left -= (unsigned long long)n;
    }
    if (fsync(fd))
        failed = 1;
    if (close(fd))
        failed = 1;
    if (failed || left) {
        ota_draft_cleanup(temp);
        json_response(client, 400, "Bad Request", "{\"error\":\"上传未完成，请重试。\"}", false);
        return;
    }
    if (ota_draft_finish(server_directory, temp, id)) {
        ota_draft_cleanup(temp);
        json_response(client, 422, "Unprocessable Content",
            "{\"error\":\"固件包校验失败。请上传包含 release.itb、kernel.itb、rootfs.itb 的 tar.gz；检查 FIT 版本、产品及组件 SHA256 是否匹配。\"}", false);
        return;
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"id\":\"%s\",\"status\":\"draft\"}\n", id);
    json_response(client, 201, "Created", json, false);
}

static void handle_client(int client, int root)
{
    char request[8192], method[16], path[2048], protocol[16], extra;
    size_t used = 0;
    bool complete = false;
    struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (used < sizeof(request) - 1 && running) {
        /* Header-only reads keep binary upload bytes out of the text parser. */
        ssize_t count = recv(client, request + used, 1, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return;
        if (memchr(request + used, '\0', (size_t)count))
            return;
        used += (size_t)count;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n")) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        text_response(client, 431, "Request Header Fields Too Large", "Headers too large\n", false);
        return;
    }
    char *line_end = strstr(request, "\r\n");
    *line_end = '\0';
    if (sscanf(request, "%15s %2047s %15s %c", method, path, protocol, &extra) != 3 ||
        (strcmp(protocol, "HTTP/1.0") && strcmp(protocol, "HTTP/1.1"))) {
        text_response(client, 400, "Bad Request", "Invalid request\n", false);
        return;
    }
    bool head = !strcmp(method, "HEAD");
    unsigned long long length = 0;
    bool has_length = false, forbidden = false, admin = false, expect = false;
    const char *origin = NULL, *host = NULL, *type = NULL, *download_device = NULL, *authorization = NULL;
    struct ota_network_context network = {0};
    char *line = line_end + 2;
    while (*line) {
        char *end = strstr(line, "\r\n");
        if (!end)
            break;
        *end = 0;
        if (!*line)
            break;
        char *colon = strchr(line, ':');
        if (!colon) {
            forbidden = true;
            break;
        }
        *colon = 0;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = 0;
        if (!strcasecmp(line, "Content-Length")) {
            char *number_end;
            errno = 0;
            for (char *p = value; *p; p++)
                if (*p < '0' || *p > '9')
                    forbidden = true;
            length = strtoull(value, &number_end, 10);
            if (has_length || errno || !*value || *number_end)
                forbidden = true;
            has_length = true;
        } else if (!strcasecmp(line, "Authorization")) {
            if (authorization) forbidden = true;
            authorization = value;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            forbidden = true; /* Chunked upload is intentionally unsupported. */
        } else if (!strcasecmp(line, "X-OTA-Request")) {
            if (admin || strcmp(value, "1"))
                forbidden = true;
            admin = true;
        } else if (!strcasecmp(line, "Origin")) {
            if (origin)
                forbidden = true;
            origin = value;
        } else if (!strcasecmp(line, "Host")) {
            if (host)
                forbidden = true;
            host = value;
        } else if (!strcasecmp(line, "Content-Type")) {
            if (type)
                forbidden = true;
            type = value;
        } else if (!strcasecmp(line, "X-OTA-Device")) {
            if (download_device || !ota_identifier(value)) forbidden = true;
            download_device = value;
        } else if (!strcasecmp(line, "X-OTA-IP")) {
            if (network.ip) forbidden = true;
            network.ip = value;
        } else if (!strcasecmp(line, "X-OTA-MAC")) {
            if (network.mac) forbidden = true;
            network.mac = value;
        } else if (!strcasecmp(line, "X-OTA-Hostname")) {
            if (network.hostname) forbidden = true;
            network.hostname = value;
        } else if (!strcasecmp(line, "X-OTA-Boot")) {
            if (network.boot) forbidden = true;
            network.boot = value;
        } else if (!strcasecmp(line, "Expect")) {
            if (strcasecmp(value, "100-continue"))
                forbidden = true;
            expect = true;
        }
        line = end + 2;
    }
    if ((network.ip || network.mac || network.hostname || network.boot) &&
        (!download_device || !ota_network_valid(&network))) forbidden = true;
    struct sockaddr_storage peer; socklen_t peer_size = sizeof(peer);
    strcpy(network.peer_ip, "unknown");
    if (!getpeername(client, (struct sockaddr *)&peer, &peer_size)) {
        if (peer.ss_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, network.peer_ip, sizeof(network.peer_ip));
        else if (peer.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, network.peer_ip, sizeof(network.peer_ip));
    }
    if (forbidden) {
        json_response(client, 400, "Bad Request", "{\"error\":\"Invalid request headers\"}", head);
        return;
    }
    if (!strncmp(path, "/api/sol/", 9)) {
        char body[4097], *response = NULL;
        if (length > 4096 || expect || origin || (!strcmp(method,"POST") && !has_length)) {
            /* Browser fetch has no Origin on same-origin GET. Device POST has none. */
            json_response(client,400,"Bad Request","{\"error\":\"Invalid SOL request\"}",head);return;
        }
        struct timeval short_timeout = {.tv_sec=2};
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&short_timeout,sizeof(short_timeout));
        size_t received=0;time_t deadline=time(NULL)+5;
        while(received<length && time(NULL)<=deadline){
            ssize_t n=recv(client,body+received,(size_t)length-received,0);
            if(n<0&&errno==EINTR)continue;
            if(n<=0)break;
            received+=(size_t)n;
        }
        if(received!=length){json_response(client,400,"Bad Request","{\"error\":\"Incomplete log batch\"}",head);return;}
        body[received]=0;
        int status=sol_request(server_directory,method,path,authorization,network.peer_ip,body,received,&response);
        json_response(client,status,status==200?"OK":"SOL Request Failed",response?response:"{}",head);
        free(response);return;
    }
    if (!strncmp(path, "/api/policy/", 12)) {
        char device[64], extra, canonical[128], body[128]; unsigned long long publication;
        if ((strcmp(method, "GET") && !head) ||
            sscanf(path, "/api/policy/%63[^/]/%llu%c", device, &publication, &extra) != 2 ||
            !ota_identifier(device) || !publication || publication > 2147483647) {
            text_response(client, 400, "Bad Request", "Invalid policy query\n", head); return;
        }
        snprintf(canonical, sizeof(canonical), "/api/policy/%s/%llu", device, publication);
        if (strcmp(path, canonical)) { text_response(client, 400, "Bad Request", "Invalid policy query\n", head); return; }
        char relative[64], directory[OTA_PATH];
        snprintf(relative, sizeof(relative), "releases/%llu", publication);
        int blocked = ota_path(directory, server_directory, relative) ? -1 : ota_quarantine_blocked(server_directory, directory);
        if (blocked < 0) { text_response(client, 503, "Unavailable", "Cannot verify failure history\n", head); return; }
        snprintf(body, sizeof(body), "BSPOTAPOLICY1 %llu %s\n", publication, blocked ? "blocked" : "allow");
        text_response(client, 200, "OK", body, head); return;
    }
    if (!strncmp(path, "/api/devices/", 13)) {
        const char *id = path + 13;
        char body[OTA_REPORT_SIZE];
        if (!ota_identifier(id) || !((*id >= 'a' && *id <= 'z') ||
            (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9'))) {
            text_response(client, 400, "Bad Request", "Invalid device ID\n", head);
        } else if (!strcmp(method, "GET") || head) {
            int rc = ota_device_read(server_directory, id, body);
            text_response(client, rc == 1 ? 204 : rc ? 500 : 200,
                rc == 1 ? "No Content" : rc ? "Read Failed" : "OK", rc ? "" : body, head);
        } else if (!strcmp(method, "POST")) {
            if (!admin || origin || !has_length || !length || length >= sizeof(body) || expect) {
                text_response(client, 400, "Bad Request", "Invalid report\n", false);
                return;
            }
            size_t received = 0;
            while (received < length) {
                ssize_t n = recv(client, body + received, (size_t)length - received, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return;
                received += (size_t)n;
            }
            body[received] = 0;
            struct ota_report report;
            if (memchr(body, 0, received) || ota_report_parse(body, &report) || strcmp(report.device, id) ||
                (download_device && strcmp(download_device, id)))
                text_response(client, 400, "Bad Request", "Invalid report\n", false);
            else if (ota_catalog_request(server_directory, id, report.publication, report.boot, report.phase, &network) ||
                     ota_device_write(server_directory, &report))
                text_response(client, 500, "Write Failed", "Report not saved\n", false);
            else text_response(client, 200, "OK", "ok\n", false);
        } else text_response(client, 405, "Method Not Allowed", "Use GET or POST\n", false);
        return;
    }
    if (!strcmp(method, "POST") && (!strcmp(path, "/api/upload") || !strncmp(path, "/api/publish/", 13))) {
        char expected_origin[512];
        int count = snprintf(expected_origin, sizeof(expected_origin), "http://%s", host ? host : "");
        if (!admin || !host || count < 0 || (size_t)count >= sizeof(expected_origin) ||
            (origin && strcmp(origin, expected_origin))) {
            json_response(client, 403, "Forbidden", "{\"error\":\"请通过本服务的管理页面操作。\"}", false);
            return;
        }
        if (!strcmp(path, "/api/upload")) {
            if (!has_length || !length) {
                json_response(client, 411, "Length Required", "{\"error\":\"Missing upload length\"}", false);
                return;
            }
            if (length > OTA_UPLOAD_MAX) {
                json_response(client, 413, "Content Too Large", "{\"error\":\"固件包不能超过 512 MiB。\"}", false);
                return;
            }
            if (!type || strcmp(type, "application/octet-stream")) {
                json_response(client, 415, "Unsupported Media Type", "{\"error\":\"Expected binary archive body\"}", false);
                return;
            }
            if (expect && send_all(client, "HTTP/1.1 100 Continue\r\n\r\n", 25))
                return;
            upload(client, length);
        } else {
            if (length) {
                json_response(client, 400, "Bad Request", "{\"error\":\"Publish request must have an empty body\"}", false);
                return;
            }
            unsigned long long number = 0;
            int status = ota_admin_publish(server_directory, path + 13, &number);
            if (status == 200) {
                char json[96];
                snprintf(json, sizeof(json), "{\"publication\":%llu,\"status\":\"published\"}\n", number);
                json_response(client, 200, "OK", json, false);
            } else {
                json_response(client, status, "Publication Failed",
                    "{\"error\":\"发布失败：检查草稿、产品/布局、版本是否倒退、磁盘空间或是否有其他发布任务。\"}", false);
            }
        }
        return;
    }
    if (strcmp(method, "GET") && !head) {
        text_response(client, 405, "Method Not Allowed", "Use GET or HEAD; admin actions use POST /api/\n", false);
        return;
    }
    char *query = strchr(path, '?');
    if (query)
        *query = '\0';
    if (!strncmp(path, "/api/download/", 14)) {
        char kind[16], id[64], extra, canonical[128], filename[128], header[512];
        unsigned long long bytes;
        if (sscanf(path, "/api/download/%15[^/]/%63s%c", kind, id, &extra) != 2) {
            text_response(client, 404, "Not Found", "Firmware unavailable\n", head); return;
        }
        snprintf(canonical, sizeof(canonical), "/api/download/%s/%s", kind, id);
        if (strcmp(path, canonical)) { text_response(client, 404, "Not Found", "Firmware unavailable\n", head); return; }
        int bundle = ota_admin_bundle(server_directory, kind, id, filename, &bytes);
        if (bundle < 0) {
            text_response(client, bundle == -ENOENT ? 404 : 503, "Unavailable", "Cannot export verified firmware package\n", head); return;
        }
        int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/gzip\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\nContent-Length: %llu\r\n"
            "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", filename, bytes);
        if (n > 0 && n < (int)sizeof(header) && !send_all(client, header, (size_t)n) && !head) {
            char buffer[65536]; ssize_t got;
            while (running && (got = read(bundle, buffer, sizeof(buffer))) != 0) {
                if (got < 0 && errno == EINTR) continue;
                if (got < 0 || send_all(client, buffer, (size_t)got)) break;
            }
        }
        close(bundle); return;
    }
    if (!strcmp(path, "/") || !strcmp(path, "/firmware")) {
        index_page(client, head);
        return;
    }
    if (!strcmp(path, "/api/state")) {
        char *json = NULL;
        if (ota_admin_state(server_directory, &json))
            json_response(client, 500, "Internal Server Error", "{\"error\":\"无法读取版本列表\"}", head);
        else {
            json_response(client, 200, "OK", json, head);
            free(json);
        }
        return;
    }
    if (!strcmp(path, "/healthz")) {
        text_response(client, 200, "OK", "ok\n", head);
        return;
    }
    if (!strncmp(path, "/recovery/", 10)) {
        char directory[OTA_PATH];
        const char *filename = strlen(path) >= 11 && !strcmp(path + strlen(path) - 11, "/rootfs.itb") ? "rootfs.itb" : "kernel.itb";
        int recovery_rc = !strcmp(filename, "rootfs.itb") ?
            ota_rootfs_recovery_path(server_directory, path, directory) :
            ota_recovery_path(server_directory, path, directory);
        if (recovery_rc) {
            text_response(client, 404, "Not Found", "Exact component unavailable or ambiguous\n", head);
            return;
        }
        const char *number = strrchr(directory, '/');
        snprintf(path, sizeof(path), "/releases/%s/%s", number + 1, filename);
    }
    int artifact_root = -1;
    const char *name = NULL;
    if (!strcmp(path, "/latest")) {
        if (!head && network.ip) {
            char latest_path[OTA_PATH]; struct release latest = {0};
            if (!ota_path(latest_path, server_directory, "latest")) (void)ota_read(latest_path, &latest);
            if (ota_catalog_request(server_directory, download_device, latest.version, network.boot, "requested", &network)) {
                text_response(client, 503, "Unavailable", "Cannot persist device request audit\n", head); return;
            }
        }
        name = "latest";
        artifact_root = dup(root);
    } else {
        unsigned long long version;
        char filename[32], trailing, canonical[128], number[32];
        if (sscanf(path, "/releases/%llu/%31s%c", &version, filename, &trailing) == 2 &&
            version && version <= 2147483647) {
            snprintf(canonical, sizeof(canonical), "/releases/%llu/%s", version, filename);
            if (!strcmp(path, canonical)) {
                for (int i = 0; i < 3; i++)
                    if (!strcmp(filename, ota_files[i]))
                        name = ota_files[i];
                if (!strcmp(filename, "manifest"))
                    name = "manifest";
                if (!strcmp(filename, "version"))
                    name = "version";
                if (!strcmp(filename, "identity"))
                    name = "identity";
                if (!strcmp(filename, "info"))
                    name = "info";
            }
            if (name) {
                int releases = openat(root, "releases", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (releases >= 0) {
                    snprintf(number, sizeof(number), "%llu", version);
                    artifact_root = openat(releases, number, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    close(releases);
                }
            }
        }
    }
    if (name && !strcmp(name, "version") && artifact_root >= 0) {
        char directory[OTA_PATH], relative[128], body[160]; struct ota_catalog_identity identity;
        unsigned long long publication; sscanf(path, "/releases/%llu/", &publication);
        snprintf(relative, sizeof(relative), "releases/%llu", publication);
        if (ota_path(directory, server_directory, relative) || ota_catalog_get(server_directory, directory, &identity))
            text_response(client, 422, "Invalid FIT", "Firmware content identity unavailable\n", head);
        else {
            snprintf(body, sizeof(body), "BSPOTAVERSION1 %llu v%lu.%lu.%lu %s\n", publication,
                identity.version >> 16, (identity.version >> 8) & 255, identity.version & 255, identity.hash);
            text_response(client, 200, "OK", body, head);
        }
        close(artifact_root); return;
    }
    if (name && !strcmp(name, "identity") && artifact_root >= 0) {
        char directory[OTA_PATH], relative[128], body[512];
        struct ota_catalog_identity identity;
        unsigned long long publication;
        sscanf(path, "/releases/%llu/", &publication);
        snprintf(relative, sizeof(relative), "releases/%llu", publication);
        if (ota_path(directory, server_directory, relative) ||
            ota_catalog_get(server_directory, directory, &identity))
            text_response(client, 422, "Invalid FIT", "Firmware identity unavailable\n", head);
        else {
            snprintf(body, sizeof(body), "BSPOTAIMAGE1 %llu %lu %llu %s %llu %s\n", publication,
                identity.version, identity.kernel_bytes, identity.kernel, identity.rootfs_bytes, identity.rootfs);
            text_response(client, 200, "OK", body, head);
        }
        close(artifact_root);
        return;
    }
    if (name && !strcmp(name, "info") && artifact_root >= 0) {
        char directory[OTA_PATH], body[128];
        struct ota_catalog_identity identity;
        unsigned long long publication;
        sscanf(path, "/releases/%llu/", &publication);
        char relative[128];
        snprintf(relative, sizeof(relative), "releases/%llu", publication);
        if (ota_path(directory, server_directory, relative) || ota_catalog_get(server_directory, directory, &identity))
            text_response(client, 422, "Invalid FIT", "Firmware version unavailable\n", head);
        else {
            snprintf(body, sizeof(body), "BSPOTAINFO1 %llu %lu\n", publication, identity.version);
            text_response(client, 200, "OK", body, head);
        }
        close(artifact_root);
        return;
    }
    if (name && artifact_root >= 0 && (!strcmp(name, "latest") || strstr(name, ".itb"))) {
        char directory[OTA_PATH], relative[64]; unsigned long long publication;
        if (!strcmp(name, "latest")) {
            char manifest[OTA_PATH]; struct release active;
            if (ota_path(manifest, server_directory, "latest") || ota_read(manifest, &active)) publication = 0;
            else publication = active.version;
        } else if (sscanf(path, "/releases/%llu/", &publication) != 1) publication = 0;
        if (publication) {
            snprintf(relative, sizeof(relative), "releases/%llu", publication);
            int blocked = ota_path(directory, server_directory, relative) ? -1 : ota_quarantine_blocked(server_directory, directory);
            if (blocked) {
                text_response(client, blocked > 0 ? 410 : 503, blocked > 0 ? "Gone" : "Unavailable", "Firmware quarantined or safety check unavailable\n", head);
                close(artifact_root); return;
            }
        }
    }
    struct stat st;
    int fd = name && artifact_root >= 0 ? open_artifact(artifact_root, name, &st) : -1;
    if (artifact_root >= 0)
        close(artifact_root);
    if (fd < 0) {
        text_response(client, 404, "Not Found", "Artifact unavailable\n", head);
        return;
    }
    if (!headers(client, 200, "OK", "application/octet-stream", (uintmax_t)st.st_size) && !head) {
        char buffer[65536];
        off_t remaining = st.st_size;
        while (remaining > 0 && running) {
            size_t wanted = remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            ssize_t count = read(fd, buffer, wanted);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0 || send_all(client, buffer, (size_t)count))
                break;
            remaining -= count;
        }
        if (!remaining && !strcmp(name, "rootfs.itb")) {
            unsigned long long publication;
            if (sscanf(path, "/releases/%llu/", &publication) == 1 &&
                ota_catalog_download(server_directory, publication, download_device))
                fprintf(stderr, "[device-server] [ERROR] Cannot record completed download for #%llu\n", publication);
        }
    }
    close(fd);
}

static void usage(const char *program)
{
    printf("Usage: %s --root RELEASE_DIR [--bind IPV4] [--port PORT] [--sol-open-view | --sol-debug-no-auth]\n"
           "Defaults: --bind 127.0.0.1 --port 8090\n"
           "Open / in a browser to upload and publish firmware bundles.\n"
           "CLI publishing: device-server publish --root DIR --source RELEASE_DIR "
           "--version NUMBER --product ID --layout ID\n", program);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "sol-enroll"))
        return sol_enroll(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "publish"))
        return ota_publish(argc, argv);
    const char *directory = NULL, *bind_address = "127.0.0.1";
    long port = 8090;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--sol-debug-no-auth")) {
            sol_set_debug_no_auth(true);
            continue;
        }
        if (!strcmp(argv[i], "--sol-open-view")) {
            sol_set_open_view(true);
            continue;
        }
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 1;
        }
        if (!strcmp(argv[i], "--root"))
            directory = argv[++i];
        else if (!strcmp(argv[i], "--bind"))
            bind_address = argv[++i];
        else if (!strcmp(argv[i], "--port")) {
            char *end;
            errno = 0;
            port = strtol(argv[++i], &end, 10);
            if (errno || !argv[i][0] || *end || port < 0 || port > 65535) {
                fprintf(stderr, "Invalid port\n");
                return 1;
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!directory) {
        usage(argv[0]);
        return 1;
    }
    int root = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0) {
        perror(directory);
        return 1;
    }
    if (ota_history_seed(directory) || ota_history_incidents(directory)) {
        fprintf(stderr, "Cannot preserve device upgrade history\n");
        close(root);
        return 1;
    }
    server_directory = directory;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        fprintf(stderr, "--bind must be an IPv4 address\n");
        close(root);
        return 1;
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        close(root);
        return 1;
    }
    int enabled = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 16)) {
        perror("listen");
        close(listener);
        close(root);
        return 1;
    }
    struct sigaction action = {.sa_handler = stop_server};
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    socklen_t size = sizeof(address);
    (void)getsockname(listener, (struct sockaddr *)&address, &size);
    printf("Device Manager: http://%s:%u/ root=%s\n", bind_address, ntohs(address.sin_port), directory);
    fflush(stdout);
    while (running) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        handle_client(client, root);
        close(client);
    }
    close(listener);
    close(root);
    return 0;
}
