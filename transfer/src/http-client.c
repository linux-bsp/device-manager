// SPDX-License-Identifier: GPL-2.0+
#define _POSIX_C_SOURCE 200809L
#include "private.h"
#include <curl/curl.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int initialization;
static void initialize(void) { initialization = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -ENOMEM; }
struct exchange {
    const struct dm_transfer_request *request;
    struct dm_transfer_result *result;
    uint64_t range_start, range_end, range_total;
    int has_range, failure, discard;
};
static int fail(struct exchange *x, int rc, enum dm_transfer_error kind)
{
    x->failure = rc; x->result->kind = kind; x->result->system_error = -rc;
    return rc;
}
void dm_transfer_request_init(struct dm_transfer_request *r)
{
    memset(r, 0, sizeof(*r)); r->method = "GET"; r->upload_fd = r->output_fd = -1;
    r->connect_timeout_ms = 10000; r->timeout_ms = 60000;
}
const char *dm_transfer_error_name(enum dm_transfer_error error)
{
    static const char *const names[] = {"none", "network", "timeout", "http", "storage", "integrity", "protocol", "cancelled"};
    return error >= DM_TRANSFER_OK && error <= DM_TRANSFER_CANCELLED ? names[error] : "unknown";
}
static size_t receive(void *data, size_t size, size_t count, void *context)
{
    struct exchange *x = context;
    const struct dm_transfer_request *r = x->request;
    if (size && count > SIZE_MAX/size) { fail(x, -EOVERFLOW, DM_TRANSFER_PROTOCOL); return 0; }
    size_t n = size*count;
    if (dm_transfer_cancelled(&r->callbacks)) { fail(x, -ECANCELED, DM_TRANSFER_CANCELLED); return 0; }
    if (x->discard && (r->output_fd >= 0 || r->sink)) return n;
    if (r->output_limit && (x->result->received > r->output_limit || n > r->output_limit-x->result->received)) {
        fail(x, -EFBIG, DM_TRANSFER_INTEGRITY); return 0;
    }
    int rc = 0;
    if (r->sink) rc = r->sink(r->sink_context, data, n);
    else if (r->output_fd >= 0) rc = dm_transfer_write_all(r->output_fd, data, n);
    else if (r->response) {
        if (!r->response_capacity || n >= r->response_capacity-x->result->response_size) {
            fail(x, -EMSGSIZE, DM_TRANSFER_PROTOCOL); return 0;
        }
        memcpy(r->response+x->result->response_size, data, n);
        x->result->response_size += n; r->response[x->result->response_size] = 0;
    }
    if (rc) { fail(x, rc, rc == -ECANCELED ? DM_TRANSFER_CANCELLED : DM_TRANSFER_STORAGE); return 0; }
    x->result->received += n;
    return n;
}
static size_t provide(void *data, size_t size, size_t count, void *context)
{
    struct exchange *x = context;
    const struct dm_transfer_request *r = x->request;
    if (dm_transfer_cancelled(&r->callbacks)) { fail(x, -ECANCELED, DM_TRANSFER_CANCELLED); return CURL_READFUNC_ABORT; }
    if (size && count > SIZE_MAX/size) { fail(x, -EOVERFLOW, DM_TRANSFER_PROTOCOL); return CURL_READFUNC_ABORT; }
    uint64_t remaining = r->upload_size-x->result->sent;
    size_t wanted = size*count;
    if (remaining < wanted) wanted = (size_t)remaining;
    if (!wanted) return 0;
    ssize_t n;
    do { n = pread(r->upload_fd, data, wanted, (off_t)(r->upload_offset+x->result->sent)); } while (n < 0 && errno == EINTR);
    if (n <= 0) { fail(x, n < 0 ? -errno : -EIO, DM_TRANSFER_STORAGE); return CURL_READFUNC_ABORT; }
    x->result->sent += (size_t)n; return (size_t)n;
}
static size_t header(char *data, size_t size, size_t count, void *context)
{
    struct exchange *x = context;
    const struct dm_transfer_request *r = x->request;
    if (size && count > SIZE_MAX/size) return 0;
    size_t n = size*count;
    if (n >= 8192) { fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0; }
    if (n > 5 && !memcmp(data, "HTTP/", 5)) {
        char line[128], protocol[16], extra; long status;
        if (n >= sizeof(line)) { fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0; }
        memcpy(line, data, n); line[n] = 0;
        if (sscanf(line, "%15s %ld%c", protocol, &status, &extra) != 3 ||
            status < 100 || status > 599 || (extra != ' ' && extra != '\r')) {
            fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0;
        }
        x->result->http_status = status; x->has_range = 0;
        x->discard = status != 200 && status != 206;
    } else if (n > 14 && !strncasecmp(data, "Content-Range:", 14)) {
        char line[128];
        if (n >= sizeof(line) || x->has_range) { fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0; }
        memcpy(line, data, n); line[n] = 0;
        const char *p = line + 14;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "bytes ", 6)) { fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0; }
        p += 6;
        uint64_t *values[] = {&x->range_start, &x->range_end, &x->range_total};
        const char separators[] = "-/\r";
        for (unsigned i = 0; i < 3; i++) {
            uint64_t value = 0;
            if (*p < '0' || *p > '9') { fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0; }
            while (*p >= '0' && *p <= '9') {
                unsigned digit = (unsigned)(*p++ - '0');
                if (value > (UINT64_MAX - digit)/10) { fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0; }
                value = value*10 + digit;
            }
            *values[i] = value;
            if (*p++ != separators[i]) { fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0; }
        }
        if (strcmp(p, "\n") || x->range_start > x->range_end || x->range_end >= x->range_total) {
            fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0;
        }
        x->has_range = 1;
    } else if (n == 2 && !memcmp(data, "\r\n", 2)) {
        if (!r->ranged && x->result->http_status == 206) { fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0; }
        if (r->ranged && (x->result->http_status == 200 || x->result->http_status == 206) &&
            (x->result->http_status != 206 || !x->has_range || x->range_start != r->range_offset ||
             (r->range_total && x->range_total != r->range_total))) {
            fail(x, -EPROTO, DM_TRANSFER_PROTOCOL); return 0;
        }
    }
    return n;
}
static int progress(void *context, curl_off_t total, curl_off_t received, curl_off_t upload_total, curl_off_t sent)
{
    struct exchange *x = context;
    const struct dm_transfer_request *r = x->request;
    if (dm_transfer_cancelled(&r->callbacks)) { fail(x, -ECANCELED, DM_TRANSFER_CANCELLED); return 1; }
    if (r->callbacks.progress) {
        int upload = strcmp(r->method, "GET") && strcmp(r->method, "HEAD");
        curl_off_t done = upload ? sent : received, maximum = upload ? upload_total : total;
        r->callbacks.progress(r->callbacks.context, done < 0 ? 0 : (uint64_t)done, maximum < 0 ? 0 : (uint64_t)maximum);
    }
    return 0;
}
int dm_transfer_perform(const struct dm_transfer_request *r, struct dm_transfer_result *result)
{
    if (!result) return -EINVAL;
    memset(result, 0, sizeof(*result));
    if (!r || !r->url || !r->method || strpbrk(r->url, "\r\n") ||
        (strcmp(r->method, "GET") && strcmp(r->method, "POST") && strcmp(r->method, "PUT") && strcmp(r->method, "HEAD")) ||
        r->header_count > 64 || (r->header_count && !r->headers) ||
        (r->response && !r->response_capacity) || (r->body_size && !r->body) ||
        (r->upload_fd >= 0 && (r->body || r->body_size)) ||
        ((!strcmp(r->method, "GET") || !strcmp(r->method, "HEAD")) &&
         (r->upload_fd >= 0 || r->body || r->body_size)) ||
        (!strcmp(r->method, "PUT") && r->upload_fd < 0) ||
        r->upload_offset > INT64_MAX || r->upload_size > INT64_MAX-r->upload_offset)
    { result->kind = DM_TRANSFER_PROTOCOL; result->system_error = EINVAL; return -EINVAL; }
    if (r->response) r->response[0] = 0;
    struct exchange x = {.request=r, .result=result};
    if (dm_transfer_cancelled(&r->callbacks)) return fail(&x, -ECANCELED, DM_TRANSFER_CANCELLED);
    if (r->upload_fd >= 0) {
        struct stat st;
        if (fstat(r->upload_fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            (uint64_t)st.st_size < r->upload_offset+r->upload_size) return fail(&x, -EIO, DM_TRANSFER_STORAGE);
    }
    int rc = pthread_once(&once, initialize);
    if (rc || initialization) return fail(&x, -ENOMEM, DM_TRANSFER_STORAGE);
    CURL *curl = curl_easy_init();
    if (!curl) return fail(&x, -ENOMEM, DM_TRANSFER_STORAGE);
    struct curl_slist *headers = NULL;
    for (size_t i = 0; i < r->header_count; i++) {
        if (!r->headers[i] || strpbrk(r->headers[i], "\r\n")) { rc = -EINVAL; goto done; }
        struct curl_slist *next = curl_slist_append(headers, r->headers[i]);
        if (!next) { rc = -ENOMEM; goto done; }
        headers = next;
    }
    struct curl_slist *next = curl_slist_append(headers, "Expect:");
    if (!next) { rc = -ENOMEM; goto done; }
    headers = next;
    curl_easy_setopt(curl, CURLOPT_URL, r->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, r->allow_tftp ? "http,https,tftp" : "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, !strncmp(r->url,"https://",8) ? "https" : "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, r->follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, r->connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, r->timeout_ms);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, r->low_speed_bytes);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, r->low_speed_seconds);
    curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)r->receive_rate);
    curl_easy_setopt(curl, CURLOPT_MAX_SEND_SPEED_LARGE, (curl_off_t)r->send_rate);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &x);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &x);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &x);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    char range[40];
    if (r->ranged) { snprintf(range, sizeof(range), "%" PRIu64 "-", r->range_offset); curl_easy_setopt(curl, CURLOPT_RANGE, range); }
    if (!strcmp(r->method, "HEAD")) curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    if (!strcmp(r->method, "POST")) curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (!strcmp(r->method, "PUT")) curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    if (r->upload_fd >= 0) {
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, provide);
        curl_easy_setopt(curl, CURLOPT_READDATA, &x);
        if (!strcmp(r->method, "POST")) curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)r->upload_size);
        else curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)r->upload_size);
    } else if (!strcmp(r->method, "POST")) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, r->body ? r->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)r->body_size);
    }
    CURLcode code = curl_easy_perform(curl);
    result->backend_code = (int)code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->http_status);
    if (x.failure) rc = x.failure;
    else if (code == CURLE_OPERATION_TIMEDOUT) rc = fail(&x, -ETIMEDOUT, DM_TRANSFER_TIMEOUT);
    else if (code != CURLE_OK) rc = fail(&x, -EHOSTUNREACH, DM_TRANSFER_NETWORK);
    else rc = 0;
done:
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    return rc;
}
