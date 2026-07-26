#ifndef IED_DISCOVERY_API_H_
#define IED_DISCOVERY_API_H_

#include "linked_list.h"
#include "features/ied_discovery/domain/ied_discovery_types.h"

/*
 * Public boundary of the ied_discovery feature. Other callers (scan_orchestration's
 * own worker - see its Architecture bullet) should only ever include this
 * header - never reach into domain/data/utils directly.
 *
 * Pure request/response, no I/O prompting anywhere in here - there is no
 * in-process interactive medium at all anymore (main.c's old terminal-prompt
 * adapter, main_discovery_prompt.c, was removed once control_dispatcher's
 * START_SCAN/STOP_SCAN commands became the daemon's only scan-driving
 * interface); discovery results reach a caller exclusively over
 * scan_dispatcher's websocket now.
 *
 * This is network HOST discovery on an already-named local interface
 * (getifaddrs + CIDR math + TCP probe + a real MMS/ACSE association) - it
 * never touches SCL or the data model, so it's distinct from CLAUDE.md's
 * "no over-the-wire tree discovery" Hard Rule (that rule is about not
 * walking a connected device's data-model tree over MMS before SCL is
 * loaded; this has zero SCL/data-model involvement). Once a host is picked,
 * it's handed to the existing, unmodified scl_bootstrap/orchestration
 * pipeline as a plain host string, exactly like a manually-typed host today.
 */

void
IedDiscoveryConfig_defaults(IedDiscoveryConfig* config);

/* Allocates only - no I/O. config: NULL means IedDiscoveryConfig_defaults(). */
IedDiscoveryHandle
IedDiscovery_create(const IedDiscoveryConfig* config, IedDiscoveryError* outError);

/*
 * Enumerates every host strictly between the network/broadcast address of
 * interfaceId's own IPv4 subnet (getifaddrs + CIDR math), excluding the
 * interface's own address, then verifies each the same two-stage way as
 * IedDiscovery_verifyHost: a cheap bounded-concurrency TCP probe first, then
 * a real MMS/ACSE association for TCP survivors only. Blocking/synchronous.
 *
 * Returns a LinkedList of owned IedDiscoveredHost* - a host is included
 * either if it fully associated (authRequired = false) or if the real
 * MMS/ACSE association was specifically access-denied (authRequired = true,
 * see IED_DISCOVERY_HOST_ACCESS_DENIED) - both are real IEC 61850 MMS
 * devices, the latter just needs credentials this handle doesn't have (or
 * has wrong). A subnet with zero such devices is a valid, non-NULL, empty
 * list, not an error. Caller owns the list: IedDiscovery_destroyHostList(list),
 * not a bare LinkedList_destroyDeep(list, free) - each element owns its own
 * host string.
 *
 * NULL + *outError for: bad interfaceId/mmsPort, interface not found/down/
 * no-IPv4 (IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND), or the subnet's host
 * count exceeding config.maxHosts (IED_DISCOVERY_ERR_SUBNET_TOO_LARGE) -
 * a safety valve against silently probing an accidentally huge range.
 */
LinkedList
IedDiscovery_scanSubnet(IedDiscoveryHandle handle, const char* interfaceId, int mmsPort,
        IedDiscoveryError* outError);

/*
 * Verifies one caller-supplied host/port identically to a scanned candidate
 * (TCP probe, then a real MMS/ACSE association) - no special-cased trust for
 * a manually-typed entry, so typos/wrong IPs are caught the same way.
 * Returns the specific outcome so a manual-add caller can show *why* a
 * candidate was rejected. *outError set only for argument-level failures
 * (NULL handle/host, bad port).
 */
IedDiscoveryHostStatus
IedDiscovery_verifyHost(IedDiscoveryHandle handle, const char* host, int mmsPort,
        IedDiscoveryError* outError);

/* Frees a LinkedList returned by IedDiscovery_scanSubnet, including each
 * element's owned host string. Safe to call with NULL. */
void
IedDiscovery_destroyHostList(LinkedList list);

/* Frees the handle. */
void
IedDiscovery_destroy(IedDiscoveryHandle handle);

#endif /* IED_DISCOVERY_API_H_ */
