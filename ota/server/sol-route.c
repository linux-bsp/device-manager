#define _POSIX_C_SOURCE 200809L
#include "sol-route.h"
#include "sol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

bool sol_route_client(int client, const struct server_context *context,
                      const struct http_request *request)
{
    if (strncmp(request->path, "/api/sol/", 9))
        return false;

    bool deleting=!strncmp(request->path,"/api/sol/records/",17);
    if(deleting){
        char expected[512];snprintf(expected,sizeof(expected),"http://%s",request->host?request->host:"");
        if(strcmp(request->method,"DELETE")||!request->admin||!request->host||request->length||
           (request->origin&&strcmp(request->origin,expected))){
            http_json(client,403,"Forbidden","{\"error\":\"Invalid deletion origin or request\"}",false);return true;
        }
    }
    char body[4097], *response = NULL;
    if (request->length > 4096 || request->expect || (request->origin&&!deleting) ||
        (!strcmp(request->method, "POST") && !request->has_length)) {
        http_json(client, 400, "Bad Request", "{\"error\":\"Invalid SOL request\"}", request->head);
        return true;
    }

    struct timeval timeout = {.tv_sec = 2};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    size_t received = 0;
    time_t deadline = time(NULL) + 5;
    while (received < request->length && time(NULL) <= deadline) {
        ssize_t count = recv(client, body + received, (size_t)request->length - received, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += (size_t)count;
    }
    if (received != request->length) {
        http_json(client, 400, "Bad Request", "{\"error\":\"Incomplete log batch\"}", request->head);
        return true;
    }
    body[received] = '\0';
    int status = sol_request(context->directory, request->method, request->path,
                             request->authorization, request->peer_ip, body, received, &response);
    http_json(client, status, status == 200 ? "OK" : "SOL Request Failed",
              response ? response : "{}", request->head);
    free(response);
    return true;
}
