#ifndef IED_DISCOVERY_CIDR_H_
#define IED_DISCOVERY_CIDR_H_

#include <stdint.h>
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
