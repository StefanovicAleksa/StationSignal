#ifndef IED_DISCOVERY_CIDR_H_
#define IED_DISCOVERY_CIDR_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "linked_list.h"

/*
 * Pure IPv4/CIDR arithmetic - no sockets, no getifaddrs, no third-party
 * includes at all, fully unit-testable without a real network interface.
 * address/netmask are host-byte-order uint32_t (the data layer ntohl's them
 * from getifaddrs' sockaddr_in before calling in here).
 */

uint32_t
IedDiscoveryCidr_networkAddress(uint32_t address, uint32_t netmask);

uint32_t
IedDiscoveryCidr_broadcastAddress(uint32_t address, uint32_t netmask);

/* Usable hosts strictly between network and broadcast; 0 for /31 and /32. */
uint32_t
IedDiscoveryCidr_hostCount(uint32_t netmask);

/*
 * True for the IPv4 link-local block 169.254.0.0/16 (RFC 3927). Used by the
 * data layer to decide which of an interface's addresses to derive a sweep
 * range from: every box in this deployment permanently carries a fixed
 * 169.254.1.1/24 recovery address alongside its real static IP (see
 * deploy/setup.sh), so "the interface's address" is genuinely ambiguous and
 * sweeping the link-local one finds nothing.
 */
bool
IedDiscoveryCidr_isLinkLocal(uint32_t address);

/* Set bits in a contiguous netmask, i.e. 0xFFFFFF00 -> 24. Diagnostics only. */
uint32_t
IedDiscoveryCidr_prefixLength(uint32_t netmask);

/*
 * Every address strictly between network and broadcast, excluding
 * excludeAddress (the interface's own address - no point probing self), as
 * owned char* dotted-quad strings, ascending order. A netmask with zero
 * usable hosts (/31, /32) yields a valid, non-NULL, empty list - not an
 * error; only IedDiscoveryCidr_hostCount(netmask) > maxHosts or an
 * allocation failure returns NULL.
 *
 * Caller owns the list: LinkedList_destroyDeep(list, free).
 */
LinkedList
IedDiscoveryCidr_buildCandidateList(uint32_t address, uint32_t netmask, uint32_t excludeAddress, uint32_t maxHosts);

#endif /* IED_DISCOVERY_CIDR_H_ */
