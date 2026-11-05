#define _POSIX_C_SOURCE 200809L
#include "dm/transfer.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define RESPONSE_LIMIT (16U * 1024U * 1024U)
#define UPLOAD_LIMIT (512ULL * 1024ULL * 1024ULL)

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: ota-cli [--server URL] [--json] COMMAND [ARG]\n"
        "  list                 Show drafts, publications and device upgrade history\n"
        "  upload PACKAGE.tar.gz Upload and validate a draft (does not publish)\n"
        "  publish draft-ID     Publish to every HWID declared in the firmware\n"
        "Server defaults to OTA_SERVER, or http://127.0.0.1:8090.\n"
        "--json preserves the server JSON for automation; default output is indented.\n"
        "Uses the shared transfer library; no GUI, local feed directory or root access needed.\n");
}

static int draft_id(const char *id)
{
    if (strncmp(id, "draft-", 6) || !id[6] || strlen(id) > 31) return 0;
    for (const unsigned char *p = (const unsigned char *)id + 6; *p; p++)
        if (!isalnum(*p) && *p != '-' && *p != '_') return 0;
    return 1;
}

/* Format whitespace only; preserve JSON strings/escapes, including UTF-8. */
static void print_json(const char *text, int raw)
{
    if (raw) { fputs(text, stdout); return; }
    unsigned depth = 0;
    int quoted = 0, escaped = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned char c = *p;
        if (quoted) {
            putchar(c);
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') quoted = 0;
            continue;
        }
        if (c == '"') { quoted = 1; putchar(c); }
        else if (c == '{' || c == '[') {
            putchar(c); depth++;
            putchar('\n'); printf("%*s", (int)(depth > 32 ? 64 : depth * 2), "");
        } else if (c == '}' || c == ']') {
            if (depth) depth--;
            putchar('\n'); printf("%*s%c", (int)(depth > 32 ? 64 : depth * 2), "", c);
        } else if (c == ',') {
            puts(","); printf("%*s", (int)(depth > 32 ? 64 : depth * 2), "");
        } else if (c == ':') fputs(": ", stdout);
        else if (!isspace(c)) putchar(c);
    }
    putchar('\n');
}

static int request(const char *url, const char *command, int upload_fd, int raw)
{
    char *body = calloc(RESPONSE_LIMIT+1, 1);
    if (!body) { perror("ota-cli: response allocation"); return 1; }
    struct dm_transfer_request request; struct dm_transfer_result response;
    dm_transfer_request_init(&request);
    request.url = url; request.response = body; request.response_capacity = RESPONSE_LIMIT+1;
    request.output_limit = RESPONSE_LIMIT; request.timeout_ms = 300000;
    const char *headers[] = {"X-OTA-Request: 1", "Content-Type: application/octet-stream"};
    if (strcmp(command, "list")) {
        request.method = "POST"; request.headers = headers; request.header_count = upload_fd >= 0 ? 2 : 1;
    }
    if (upload_fd >= 0) {
        struct stat st;
        if (fstat(upload_fd, &st)) { free(body); return 1; }
        request.upload_fd = upload_fd; request.upload_size = (uint64_t)st.st_size;
    }
    int rc = dm_transfer_perform(&request, &response);
    long expected = !strcmp(command, "upload") ? 201 : 200;
    long code = response.http_status;
    int failed = rc || code != expected || !*body;
    if (failed) {
        fprintf(stderr, "ota-cli: %s failed (HTTP %ld).%s\n", command, code,
            strcmp(command, "list") ? " If the connection was lost, check list before retrying." : "");
        if (body && *body) fprintf(stderr, "%s\n", body);
    } else {
        print_json(body, raw);
        if (ferror(stdout)) failed = 1;
    }
    free(body);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *server = getenv("OTA_SERVER"), *command = NULL, *arg = NULL;
    int raw = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        if (!strcmp(argv[i], "--json")) raw = 1;
        else if (!strcmp(argv[i], "--server") && i + 1 < argc) server = argv[++i];
        else if (!command && argv[i][0] != '-') command = argv[i];
        else if (command && !arg && argv[i][0] != '-') arg = argv[i];
        else { usage(stderr); return 2; }
    }
    if (!server || !*server) server = "http://127.0.0.1:8090";
    size_t len = strlen(server);
    while (len && server[len - 1] == '/') len--;
    if (!command || (strcmp(command, "list") && strcmp(command, "upload") && strcmp(command, "publish")) ||
        ((!strcmp(command, "list")) ? arg != NULL : arg == NULL) || len > 3000 ||
        (strncmp(server, "http://", 7) && strncmp(server, "https://", 8)) ||
        strchr(server, '?') || strchr(server, '#') || strchr(server, '\n') || strchr(server, '\r')) {
        usage(stderr); return 2;
    }
    char url[4096];
    int upload_fd = -1;
    if (!strcmp(command, "publish")) {
        if (!draft_id(arg)) { fprintf(stderr, "ota-cli: expected draft-ID from upload/list\n"); return 2; }
        snprintf(url, sizeof(url), "%.*s/api/publish/%s", (int)len, server, arg);
    } else {
        snprintf(url, sizeof(url), "%.*s/api/%s", (int)len, server,
            !strcmp(command, "list") ? "state" : "upload");
        if (arg) {
            upload_fd = open(arg, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            struct stat st;
            if (upload_fd < 0 || fstat(upload_fd, &st) || !S_ISREG(st.st_mode) ||
                st.st_size <= 0 || (unsigned long long)st.st_size > UPLOAD_LIMIT) {
                fprintf(stderr, "ota-cli: upload must be a regular file of 1 byte to 512 MiB\n");
                if (upload_fd >= 0) close(upload_fd);
                return 2;
            }
        }
    }
    int rc = request(url, command, upload_fd, raw);
    if (upload_fd >= 0) close(upload_fd);
    return rc;
}
