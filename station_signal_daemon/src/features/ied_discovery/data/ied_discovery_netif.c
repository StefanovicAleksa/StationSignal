#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "features/ied_discovery/data/ied_discovery_netif.h"

bool
IedDiscoveryNetif_getInterfaceIpv4(const char* interfaceId, uint32_t* outAddress, uint32_t* outNetmask) {
    if (!interfaceId || !interfaceId[0] || !outAddress || !outNetmask) return false;

    struct ifaddrs* addrs;
    if (getifaddrs(&addrs) != 0) return false;

    bool found = false;
    for (struct ifaddrs* ifa = addrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, interfaceId) != 0) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        struct sockaddr_in* addr = (struct sockaddr_in*) (void*) ifa->ifa_addr;
        struct sockaddr_in* netmask = (struct sockaddr_in*) (void*) ifa->ifa_netmask;

        *outAddress = ntohl(addr->sin_addr.s_addr);
        *outNetmask = ntohl(netmask->sin_addr.s_addr);
        found = true;
        break;
    }

    freeifaddrs(addrs);
    return found;
}
