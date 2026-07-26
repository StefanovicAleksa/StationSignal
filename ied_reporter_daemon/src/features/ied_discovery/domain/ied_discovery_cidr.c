#include <stdlib.h>
#include <stdio.h>
#include "features/ied_discovery/domain/ied_discovery_cidr.h"

uint32_t
IedDiscoveryCidr_networkAddress(uint32_t address, uint32_t netmask) {
    return address & netmask;
}

uint32_t
IedDiscoveryCidr_broadcastAddress(uint32_t address, uint32_t netmask) {
    return (address & netmask) | ~netmask;
}

uint32_t
IedDiscoveryCidr_hostCount(uint32_t netmask) {
    uint32_t rangeSize = ~netmask; /* e.g. 0xFF for a /24 */
    if (rangeSize <= 1) return 0; /* /31 (rangeSize=1) or /32 (rangeSize=0): no usable hosts */
    return rangeSize - 1;
}

static char*
formatDottedQuad(uint32_t address) {
    char* buf = malloc(16); /* "255.255.255.255" + NUL */
    if (!buf) return NULL;
    snprintf(buf, 16, "%u.%u.%u.%u",
            (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF);
    return buf;
}

LinkedList
IedDiscoveryCidr_buildCandidateList(uint32_t address, uint32_t netmask, uint32_t excludeAddress, uint32_t maxHosts) {
    if (IedDiscoveryCidr_hostCount(netmask) > maxHosts) return NULL;

    uint32_t network = IedDiscoveryCidr_networkAddress(address, netmask);
    uint32_t broadcast = IedDiscoveryCidr_broadcastAddress(address, netmask);

    LinkedList candidates = LinkedList_create();
    if (!candidates) return NULL;

    for (uint32_t host = network + 1; host < broadcast; host++) {
        if (host == excludeAddress) continue;

        char* ip = formatDottedQuad(host);
        if (!ip) {
            LinkedList_destroyDeep(candidates, free);
            return NULL;
        }
        LinkedList_add(candidates, ip);
    }

    return candidates;
}
