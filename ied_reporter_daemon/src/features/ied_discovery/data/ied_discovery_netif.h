#ifndef IED_DISCOVERY_NETIF_H_
#define IED_DISCOVERY_NETIF_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Fills outAddress/outNetmask (host-byte-order, already ntohl'd) with
 * interfaceId's own IPv4 address/netmask via getifaddrs()/freeifaddrs() -
 * plain POSIX libc, not a new third-party dependency. Returns false if the
 * named interface doesn't exist, is down (no IFF_UP), or has no AF_INET
 * address - collapsed to one caller-facing failure, not distinguished
 * further (nothing downstream needs to tell these apart).
 */
bool
IedDiscoveryNetif_getInterfaceIpv4(const char* interfaceId, uint32_t* outAddress, uint32_t* outNetmask);

#endif /* IED_DISCOVERY_NETIF_H_ */
