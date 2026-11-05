/* Process lifecycle and listener for the Device Manager host service. */
#define _POSIX_C_SOURCE 200809L
#include "ota-admin.h"
#include "sol.h"
#include "ota-device.h"
#include "router.h"
#include "admission.h"
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

static void stop_server(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void usage(const char *program)
{
    printf("Usage: %s --root RELEASE_DIR [--bind IPV4] [--port PORT] [--sol-open-view | --sol-debug-no-auth]\n"
           "Defaults: --bind 127.0.0.1 --port 8090\n"
           "Open / in a browser to upload and publish firmware bundles.\n"
           "CLI publishing: device-server publish --root DIR --source RELEASE_DIR "
           "--version NUMBER\n", program);
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
    const char *history_step = "seed";
    int history_status = ota_history_seed(directory);
    if (!history_status) {
        history_step = "incident replay";
        history_status = ota_history_incidents(directory);
    }
    if (history_status) {
        fprintf(stderr, "[device-server] [ERROR] Upgrade history %s failed in %s\n", history_step, directory);
        close(root);
        return 1;
    }
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
    struct server_context context = {.directory = directory, .root = root, .running = &running};
    int rc = server_accept_loop(listener, &context);
    if (rc) fprintf(stderr, "[device-server] [ERROR] listener failed: %s\n", strerror(-rc));
    close(listener);
    close(root);
    return rc ? 1 : 0;
}
