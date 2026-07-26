#ifndef SCAN_ORCHESTRATION_USECASES_H_
#define SCAN_ORCHESTRATION_USECASES_H_

#include "stdbool_compat.h"

/*
 * Pure logic - zero third-party includes, unit-testable with hand-built
 * plain arrays only.
 */

/*
 * Linear scan, exact-string comparison - O(seenCount), fine for realistic
 * per-scan host counts (tens, bounded by IedDiscoveryConfig.maxHosts anyway).
 * Canonical dotted-quad IPs need no normalization. Returns true if host is
 * NULL/empty (treated as "new" - caller's own responsibility not to call
 * this with a bad host in practice) or not present in seenHosts[0..seenCount).
 */
bool
ScanOrchestrationUseCases_isHostNew(const char* const* seenHosts, int seenCount, const char* host);

#endif /* SCAN_ORCHESTRATION_USECASES_H_ */
