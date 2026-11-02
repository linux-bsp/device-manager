/* HTTP routes connect transport to OTA, SOL and firmware services. */
#define _POSIX_C_SOURCE 200809L
#include "router.h"
#include "http.h"
#include "ota-admin.h"
#include "sol-route.h"
#include "ota-device.h"
#include "ota-page.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

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

static void index_page(int client, bool head)
{
    if (!http_headers(client, 200, "OK", "text/html; charset=utf-8", sizeof(admin_page) - 1) && !head)
        (void)http_send_all(client, admin_page, sizeof(admin_page) - 1);
}

static void upload(int client, unsigned long long length, const struct server_context *context)
{
    char temp[OTA_PATH], id[32];
    int fd = ota_draft_begin(context->directory, temp);
    if (fd < 0) {
        http_json(client, 503, "Service Unavailable", "{\"error\":\"无法创建上传目录，请检查空间或清理旧草稿（最多 256 个）。\"}", false);
        return;
    }
    char buffer[65536];
    unsigned long long left = length;
    time_t started = time(NULL);
    int failed = 0;
    while (left && (*context->running)) {
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
        http_json(client, 400, "Bad Request", "{\"error\":\"上传未完成，请重试。\"}", false);
        return;
    }
    if (ota_draft_finish(context->directory, temp, id)) {
        ota_draft_cleanup(temp);
        http_json(client, 422, "Unprocessable Content",
            "{\"error\":\"固件包校验失败。请上传包含 release.itb、kernel.itb、rootfs.itb 的 tar.gz；检查 FIT 版本、产品及组件 SHA256 是否匹配。\"}", false);
        return;
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"id\":\"%s\",\"status\":\"draft\"}\n", id);
    http_json(client, 201, "Created", json, false);
}

void server_route_client(int client, const struct server_context *context)
{
    struct http_request request;
    int status = http_read_request(client, &request);
    if (status < 0)
        return;
    if (status == 431) {
        http_text(client, 431, "Request Header Fields Too Large", "Headers too large\n", false);
        return;
    }
    if (status == 400) {
        http_text(client, 400, "Bad Request", "Invalid request\n", false);
        return;
    }
    char *method = request.method;
    char *path = request.path;
    bool head = request.head;
    unsigned long long length = request.length;
    bool has_length = request.has_length, admin = request.admin, expect = request.expect;
    const char *origin = request.origin, *host = request.host, *type = request.type;
    const char *download_device = request.device;
    struct ota_network_context network = {
        .ip = request.ip, .mac = request.mac, .hostname = request.hostname, .boot = request.boot
    };
    memcpy(network.peer_ip, request.peer_ip, sizeof(network.peer_ip));
    int root = context->root;
    if ((download_device && !ota_identifier(download_device)) ||
        ((network.ip || network.mac || network.hostname || network.boot) &&
         (!download_device || !ota_network_valid(&network)))) {
        http_json(client, 400, "Bad Request", "{\"error\":\"Invalid request headers\"}", head);
        return;
    }
    if (sol_route_client(client, context, &request))
        return;
    if (!strncmp(path, "/api/policy/", 12)) {
        char device[64], extra, canonical[128], body[128]; unsigned long long publication;
        if ((strcmp(method, "GET") && !head) ||
            sscanf(path, "/api/policy/%63[^/]/%llu%c", device, &publication, &extra) != 2 ||
            !ota_identifier(device) || !publication || publication > 2147483647) {
            http_text(client, 400, "Bad Request", "Invalid policy query\n", head); return;
        }
        snprintf(canonical, sizeof(canonical), "/api/policy/%s/%llu", device, publication);
        if (strcmp(path, canonical)) { http_text(client, 400, "Bad Request", "Invalid policy query\n", head); return; }
        char relative[64], directory[OTA_PATH];
        snprintf(relative, sizeof(relative), "releases/%llu", publication);
        int blocked = ota_path(directory, context->directory, relative) ? -1 : ota_quarantine_blocked(context->directory, directory);
        if (blocked < 0) { http_text(client, 503, "Unavailable", "Cannot verify failure history\n", head); return; }
        snprintf(body, sizeof(body), "BSPOTAPOLICY1 %llu %s\n", publication, blocked ? "blocked" : "allow");
        http_text(client, 200, "OK", body, head); return;
    }
    if (!strncmp(path, "/api/devices/", 13)) {
        const char *id = path + 13;
        char body[OTA_REPORT_SIZE];
        if (!ota_identifier(id) || !((*id >= 'a' && *id <= 'z') ||
            (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9'))) {
            http_text(client, 400, "Bad Request", "Invalid device ID\n", head);
        } else if (!strcmp(method, "GET") || head) {
            int rc = ota_device_read(context->directory, id, body);
            http_text(client, rc == 1 ? 204 : rc ? 500 : 200,
                rc == 1 ? "No Content" : rc ? "Read Failed" : "OK", rc ? "" : body, head);
        } else if (!strcmp(method, "POST")) {
            if (!admin || origin || !has_length || !length || length >= sizeof(body) || expect) {
                http_text(client, 400, "Bad Request", "Invalid report\n", false);
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
                http_text(client, 400, "Bad Request", "Invalid report\n", false);
            else if (ota_catalog_request(context->directory, id, report.publication, report.boot, report.phase, &network) ||
                     ota_device_write(context->directory, &report))
                http_text(client, 500, "Write Failed", "Report not saved\n", false);
            else http_text(client, 200, "OK", "ok\n", false);
        } else http_text(client, 405, "Method Not Allowed", "Use GET or POST\n", false);
        return;
    }
    if (!strcmp(method, "POST") && (!strcmp(path, "/api/upload") || !strncmp(path, "/api/publish/", 13))) {
        char expected_origin[512];
        int count = snprintf(expected_origin, sizeof(expected_origin), "http://%s", host ? host : "");
        if (!admin || !host || count < 0 || (size_t)count >= sizeof(expected_origin) ||
            (origin && strcmp(origin, expected_origin))) {
            http_json(client, 403, "Forbidden", "{\"error\":\"请通过本服务的管理页面操作。\"}", false);
            return;
        }
        if (!strcmp(path, "/api/upload")) {
            if (!has_length || !length) {
                http_json(client, 411, "Length Required", "{\"error\":\"Missing upload length\"}", false);
                return;
            }
            if (length > OTA_UPLOAD_MAX) {
                http_json(client, 413, "Content Too Large", "{\"error\":\"固件包不能超过 512 MiB。\"}", false);
                return;
            }
            if (!type || strcmp(type, "application/octet-stream")) {
                http_json(client, 415, "Unsupported Media Type", "{\"error\":\"Expected binary archive body\"}", false);
                return;
            }
            if (expect && http_send_all(client, "HTTP/1.1 100 Continue\r\n\r\n", 25))
                return;
            upload(client, length, context);
        } else {
            if (length) {
                http_json(client, 400, "Bad Request", "{\"error\":\"Publish request must have an empty body\"}", false);
                return;
            }
            unsigned long long number = 0;
            int status = ota_admin_publish(context->directory, path + 13, &number);
            if (status == 200) {
                char json[96];
                snprintf(json, sizeof(json), "{\"publication\":%llu,\"status\":\"published\"}\n", number);
                http_json(client, 200, "OK", json, false);
            } else {
                http_json(client, status, "Publication Failed",
                    "{\"error\":\"发布失败：检查草稿、产品/布局、版本是否倒退、磁盘空间或是否有其他发布任务。\"}", false);
            }
        }
        return;
    }
    if (strcmp(method, "GET") && !head) {
        http_text(client, 405, "Method Not Allowed", "Use GET or HEAD; admin actions use POST /api/\n", false);
        return;
    }
    char *query = strchr(path, '?');
    if (query)
        *query = '\0';
    if (!strncmp(path, "/api/download/", 14)) {
        char kind[16], id[64], extra, canonical[128], filename[128], header[512];
        unsigned long long bytes;
        if (sscanf(path, "/api/download/%15[^/]/%63s%c", kind, id, &extra) != 2) {
            http_text(client, 404, "Not Found", "Firmware unavailable\n", head); return;
        }
        snprintf(canonical, sizeof(canonical), "/api/download/%s/%s", kind, id);
        if (strcmp(path, canonical)) { http_text(client, 404, "Not Found", "Firmware unavailable\n", head); return; }
        int bundle = ota_admin_bundle(context->directory, kind, id, filename, &bytes);
        if (bundle < 0) {
            http_text(client, bundle == -ENOENT ? 404 : 503, "Unavailable", "Cannot export verified firmware package\n", head); return;
        }
        int n = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/gzip\r\n"
            "Content-Disposition: attachment; filename=\"%s\"\r\nContent-Length: %llu\r\n"
            "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", filename, bytes);
        if (n > 0 && n < (int)sizeof(header) && !http_send_all(client, header, (size_t)n) && !head) {
            char buffer[65536]; ssize_t got;
            while ((*context->running) && (got = read(bundle, buffer, sizeof(buffer))) != 0) {
                if (got < 0 && errno == EINTR) continue;
                if (got < 0 || http_send_all(client, buffer, (size_t)got)) break;
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
        if (ota_admin_state(context->directory, &json))
            http_json(client, 500, "Internal Server Error", "{\"error\":\"无法读取版本列表\"}", head);
        else {
            http_json(client, 200, "OK", json, head);
            free(json);
        }
        return;
    }
    if (!strcmp(path, "/healthz")) {
        http_text(client, 200, "OK", "ok\n", head);
        return;
    }
    if (!strncmp(path, "/recovery/", 10)) {
        char directory[OTA_PATH];
        const char *filename = strlen(path) >= 11 && !strcmp(path + strlen(path) - 11, "/rootfs.itb") ? "rootfs.itb" : "kernel.itb";
        int recovery_rc = !strcmp(filename, "rootfs.itb") ?
            ota_rootfs_recovery_path(context->directory, path, directory) :
            ota_recovery_path(context->directory, path, directory);
        if (recovery_rc) {
            http_text(client, 404, "Not Found", "Exact component unavailable or ambiguous\n", head);
            return;
        }
        const char *number = strrchr(directory, '/');
        snprintf(path, sizeof(request.path), "/releases/%s/%s", number + 1, filename);
    }
    int artifact_root = -1;
    const char *name = NULL;
    if (!strcmp(path, "/latest")) {
        if (!head && network.ip) {
            char latest_path[OTA_PATH]; struct release latest = {0};
            if (!ota_path(latest_path, context->directory, "latest")) (void)ota_read(latest_path, &latest);
            if (ota_catalog_request(context->directory, download_device, latest.version, network.boot, "requested", &network)) {
                http_text(client, 503, "Unavailable", "Cannot persist device request audit\n", head); return;
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
        if (ota_path(directory, context->directory, relative) || ota_catalog_get(context->directory, directory, &identity))
            http_text(client, 422, "Invalid FIT", "Firmware content identity unavailable\n", head);
        else {
            snprintf(body, sizeof(body), "BSPOTAVERSION1 %llu v%lu.%lu.%lu %s\n", publication,
                identity.version >> 16, (identity.version >> 8) & 255, identity.version & 255, identity.hash);
            http_text(client, 200, "OK", body, head);
        }
        close(artifact_root); return;
    }
    if (name && !strcmp(name, "identity") && artifact_root >= 0) {
        char directory[OTA_PATH], relative[128], body[512];
        struct ota_catalog_identity identity;
        unsigned long long publication;
        sscanf(path, "/releases/%llu/", &publication);
        snprintf(relative, sizeof(relative), "releases/%llu", publication);
        if (ota_path(directory, context->directory, relative) ||
            ota_catalog_get(context->directory, directory, &identity))
            http_text(client, 422, "Invalid FIT", "Firmware identity unavailable\n", head);
        else {
            snprintf(body, sizeof(body), "BSPOTAIMAGE1 %llu %lu %llu %s %llu %s\n", publication,
                identity.version, identity.kernel_bytes, identity.kernel, identity.rootfs_bytes, identity.rootfs);
            http_text(client, 200, "OK", body, head);
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
        if (ota_path(directory, context->directory, relative) || ota_catalog_get(context->directory, directory, &identity))
            http_text(client, 422, "Invalid FIT", "Firmware version unavailable\n", head);
        else {
            snprintf(body, sizeof(body), "BSPOTAINFO1 %llu %lu\n", publication, identity.version);
            http_text(client, 200, "OK", body, head);
        }
        close(artifact_root);
        return;
    }
    if (name && artifact_root >= 0 && (!strcmp(name, "latest") || strstr(name, ".itb"))) {
        char directory[OTA_PATH], relative[64]; unsigned long long publication;
        if (!strcmp(name, "latest")) {
            char manifest[OTA_PATH]; struct release active;
            if (ota_path(manifest, context->directory, "latest") || ota_read(manifest, &active)) publication = 0;
            else publication = active.version;
        } else if (sscanf(path, "/releases/%llu/", &publication) != 1) publication = 0;
        if (publication) {
            snprintf(relative, sizeof(relative), "releases/%llu", publication);
            int blocked = ota_path(directory, context->directory, relative) ? -1 : ota_quarantine_blocked(context->directory, directory);
            if (blocked) {
                http_text(client, blocked > 0 ? 410 : 503, blocked > 0 ? "Gone" : "Unavailable", "Firmware quarantined or safety check unavailable\n", head);
                close(artifact_root); return;
            }
        }
    }
    struct stat st;
    int fd = name && artifact_root >= 0 ? open_artifact(artifact_root, name, &st) : -1;
    if (artifact_root >= 0)
        close(artifact_root);
    if (fd < 0) {
        http_text(client, 404, "Not Found", "Artifact unavailable\n", head);
        return;
    }
    if (!http_headers(client, 200, "OK", "application/octet-stream", (uintmax_t)st.st_size) && !head) {
        char buffer[65536];
        off_t remaining = st.st_size;
        while (remaining > 0 && (*context->running)) {
            size_t wanted = remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
            ssize_t count = read(fd, buffer, wanted);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0 || http_send_all(client, buffer, (size_t)count))
                break;
            remaining -= count;
        }
        if (!remaining && !strcmp(name, "rootfs.itb")) {
            unsigned long long publication;
            if (sscanf(path, "/releases/%llu/", &publication) == 1 &&
                ota_catalog_download(context->directory, publication, download_device))
                fprintf(stderr, "[device-server] [ERROR] Cannot record completed download for #%llu\n", publication);
        }
    }
    close(fd);
}
