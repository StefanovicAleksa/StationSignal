#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "features/ied_discovery/data/ied_discovery_netif.h"
#include "features/ied_discovery/domain/ied_discovery_cidr.h"

/*
 * One interface, several IPv4 addresses: take the routable one.
 *
 * This used to stop at the first AF_INET address getifaddrs reported for the
 * name, which is only correct when an interface has exactly one. Every box in
 * this deployment permanently carries a fixed 169.254.1.1/24 recovery address
 * next to its real static IP (deploy/setup.sh), and the kernel lists the
 * link-local one FIRST:
 *
 *   2: enp34s0: ...
 *       inet 169.254.1.1/24  scope link
 *       inet 192.168.1.50/24 scope global
 *
 * so every sweep enumerated 169.254.1.1-254 and confirmed nothing - a /24 is
 * well under maxHosts, so it raised no error either. Scanning simply stopped
 * finding devices while every other feature (which is handed an explicit host)
 * kept working.
 *
 * A link-local address is still used when it is the interface's ONLY one, so a
 * genuinely link-local-only segment still scans. Loopback is deliberately NOT
 * excluded: 127.0.0.1 is the only address on "lo", and both the unit test and
 * scan_orchestration's integration test depend on "lo" resolving (the latter
 * asserts its /8 is rejected as SUBNET_TOO_LARGE, which requires getting an
 * address back in the first place).
 */
bool
IedDiscoveryNetif_getInterfaceIpv4(const char* interfaceId, uint32_t* outAddress, uint32_t* outNetmask) {
    if (!interfaceId || !interfaceId[0] || !outAddress || !outNetmask) return false;

    struct ifaddrs* addrs;
    if (getifaddrs(&addrs) != 0) return false;

    bool foundRoutable = false;
    bool foundLinkLocal = false;
    uint32_t linkLocalAddress = 0;
    uint32_t linkLocalNetmask = 0;

    for (struct ifaddrs* ifa = addrs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, interfaceId) != 0) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        struct sockaddr_in* addr = (struct sockaddr_in*) (void*) ifa->ifa_addr;
        struct sockaddr_in* netmask = (struct sockaddr_in*) (void*) ifa->ifa_netmask;
        uint32_t hostAddress = ntohl(addr->sin_addr.s_addr);
        uint32_t hostNetmask = ntohl(netmask->sin_addr.s_addr);

        if (IedDiscoveryCidr_isLinkLocal(hostAddress)) {
            if (!foundLinkLocal) {
                linkLocalAddress = hostAddress;
                linkLocalNetmask = hostNetmask;
                foundLinkLocal = true;
            }
            continue; /* keep looking for something routable */
        }

        *outAddress = hostAddress;
        *outNetmask = hostNetmask;
        foundRoutable = true;
        break;
    }

    freeifaddrs(addrs);

    if (foundRoutable) return true;
    if (foundLinkLocal) {
        *outAddress = linkLocalAddress;
        *outNetmask = linkLocalNetmask;
        return true;
    }
    return false;
}
