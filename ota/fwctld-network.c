#define _DEFAULT_SOURCE
#include "fwctld-network.h"
#include "ota-common.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>

/* This gates local link/address readiness, not server reachability. HTTP errors
 * remain visible once the interface is ready. Never depend on ping or ip tools. */
int fwctld_network_ready(const char *interface)
{
    struct ifaddrs *addresses, *a;
    int ready = 0;
    if (getifaddrs(&addresses)) return 0;
    for (a = addresses; a; a = a->ifa_next) {
        if (!a->ifa_name || strcmp(a->ifa_name, interface) || !a->ifa_addr ||
            (a->ifa_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
            continue;
        if (a->ifa_addr->sa_family == AF_INET) {
            const struct sockaddr_in *v4 = (const void *)a->ifa_addr;
            ready = v4->sin_addr.s_addr != htonl(INADDR_ANY);
        } else if (a->ifa_addr->sa_family == AF_INET6) {
            const struct sockaddr_in6 *v6 = (const void *)a->ifa_addr;
            ready = !IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr) &&
                    !IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr);
        }
        if (ready) break;
    }
    freeifaddrs(addresses);
    return ready;
}

/* Refresh before each poll; no shell utilities or device-ID-derived MACs. */
static char context[5][320];
static int context_count;
void fwctld_network_context(const char *interface, const char *device)
{
    char ip[INET6_ADDRSTRLEN] = "unknown", mac[32] = "unknown";
    char hostname[254] = "unknown", boot[64] = "unknown", selected[IF_NAMESIZE] = "";
    struct ifaddrs *addresses;
    context_count = 0;
    if (!device) return;
    if (interface) snprintf(selected, sizeof(selected), "%s", interface);
    if (!getifaddrs(&addresses)) {
        for (struct ifaddrs *a = addresses; a; a = a->ifa_next) {
            if (!a->ifa_addr || !a->ifa_name || !(a->ifa_flags & IFF_UP) ||
                (selected[0] ? strcmp(selected, a->ifa_name) : !!(a->ifa_flags & IFF_LOOPBACK))) continue;
            int family = a->ifa_addr->sa_family;
            if (family != AF_INET && family != AF_INET6) continue;
            const void *addr = family == AF_INET ?
                (const void *)&((struct sockaddr_in *)a->ifa_addr)->sin_addr :
                (const void *)&((struct sockaddr_in6 *)a->ifa_addr)->sin6_addr;
            if (family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL((const struct in6_addr *)addr)) continue;
            if (!inet_ntop(family, addr, ip, sizeof(ip))) continue;
            snprintf(selected, sizeof(selected), "%s", a->ifa_name);
            if (family == AF_INET) break;
        }
        for (struct ifaddrs *a = addresses; a; a = a->ifa_next) {
            if (!a->ifa_addr || !a->ifa_name || strcmp(selected, a->ifa_name) || a->ifa_addr->sa_family != AF_PACKET) continue;
            const struct sockaddr_ll *ll = (const void *)a->ifa_addr;
            if (ll->sll_halen == 6) snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                ll->sll_addr[0], ll->sll_addr[1], ll->sll_addr[2], ll->sll_addr[3], ll->sll_addr[4], ll->sll_addr[5]);
        }
        freeifaddrs(addresses);
    }
    if (gethostname(hostname, sizeof(hostname))) strcpy(hostname, "unknown");
    hostname[sizeof(hostname)-1] = 0;
    if (!*hostname || strspn(hostname, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(hostname)) strcpy(hostname, "unknown");
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (f) { if (!fgets(boot, sizeof(boot), f)) strcpy(boot, "unknown"); fclose(f); }
    boot[strcspn(boot, "\r\n")] = 0;
    if (!ota_identifier(boot)) strcpy(boot, "unknown");
    snprintf(context[0], sizeof(context[0]), "X-OTA-IP: %s", ip);
    snprintf(context[1], sizeof(context[1]), "X-OTA-MAC: %s", mac);
    snprintf(context[2], sizeof(context[2]), "X-OTA-Hostname: %s", hostname);
    snprintf(context[3], sizeof(context[3]), "X-OTA-Boot: %s", boot);
    snprintf(context[4], sizeof(context[4]), "X-OTA-Device: %s", device);
    context_count = 5;
}
int fwctld_request(char *const args[], char *output, unsigned size)
{
    char *request[96]; unsigned n = 0;
    while (args[n]) { if (n >= 80) return -1; request[n] = args[n]; n++; }
    for (int i = 0; i < context_count; i++) { request[n++] = "--header"; request[n++] = context[i]; }
    request[n] = NULL;
    return ota_run(request, output, size);
}
