#ifndef IED_DISCOVERY_MMS_PROBE_H_
#define IED_DISCOVERY_MMS_PROBE_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Phase 2 of verification: a real MMS/ACSE association (IedConnection_connect)
 * against (host, port), immediately closed - no file browsing, no SCL fetch.
 * That heavier work is scl_bootstrap's own job, deferred until a host is
 * actually picked (see this feature's own Architecture bullet in CLAUDE.md
 * for why: keeps the scan phase itself lightweight, at the cost of the
 * picked host getting associated-with twice overall - an accepted tradeoff,
 * not a bug).
 *
 * Retries once with ownedAuthPassword if the first attempt is rejected at
 * the ACSE level (same IED_ERROR_CONNECTION_REJECTED/IED_ERROR_ACCESS_DENIED
 * heuristic scl_bootstrap_mms_session.c documents), mirroring scl_bootstrap's
 * own one-retry-on-rejection policy - a real device that requires auth
 * shouldn't be wrongly excluded from the discovered list just because
 * discovery doesn't know yet whether a password applies.
 *
 * Returns true only if a real MMS/ACSE association actually succeeded
 * (with or without the retry).
 *
 * outAccessDenied (may be NULL): set to true iff the final attempt (the
 * retry, if one was made; otherwise the only attempt) failed specifically
 * because of an ACSE-level access rejection - i.e. this is a real IEC 61850
 * MMS device that needs credentials, not "nothing here". Always set to false
 * whenever this function returns true. Only meaningful once the function has
 * returned - the first attempt is always unauthenticated, so a transient
 * access-denial on that attempt alone doesn't mean the overall outcome is
 * denied.
 */
bool
IedDiscoveryMmsProbe_associate(const char* host, int port, uint32_t connectTimeoutMs,
        const char* ownedAuthPassword, bool* outAccessDenied);

#endif /* IED_DISCOVERY_MMS_PROBE_H_ */
