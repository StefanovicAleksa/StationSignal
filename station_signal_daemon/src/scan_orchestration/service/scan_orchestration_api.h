#ifndef SCAN_ORCHESTRATION_API_H_
#define SCAN_ORCHESTRATION_API_H_

#include <stdint.h>
#include "scan_orchestration/domain/scan_orchestration_types.h"

/*
 * Public boundary of scan_orchestration. Other code (main.c, or whatever
 * later replaces it - see this feature's own CLAUDE.md Architecture bullet)
 * should only ever include this header - never reach into domain/data
 * directly.
 *
 * Sequences ied_discovery (domain: subnet enumeration + host verification)
 * and scan_dispatcher (transport) into a continuous, background,
 * reference-counted, multi-scan-capable service - a top-level sibling of
 * src/orchestration/, not a feature under src/features/, per CLAUDE.md's
 * explicit convention that cross-feature sequencing lives outside
 * src/features/. ied_discovery itself is untouched by this layer.
 */

/* Fills config with recommended defaults: scanDispatcherConfig via
 * ScanDispatcherConfig_defaults (port 8766), defaultSweepIntervalMs=10000,
 * discoveryConfigTemplate via IedDiscoveryConfig_defaults (its own
 * .acseAuthPassword is later ignored per-scan - see ScanRequest's own doc
 * comment). Caller may override individual fields before passing to
 * ScanOrchestration_create. */
void
ScanOrchestrationConfig_defaults(ScanOrchestrationConfig* config);

/* Allocates only: creates the registry and calls ScanDispatcher_create
 * (ring-buffer allocation only, no bind - matches every sibling feature's
 * "no I/O at create" contract). config: NULL means
 * ScanOrchestrationConfig_defaults(). */
ScanOrchestrationHandle
ScanOrchestration_create(const ScanOrchestrationConfig* config, ScanOrchestrationError* outError);

/* Must be called before any ScanOrchestration_startScan - see
 * ScanOrchestrationDeviceFoundCallback's own doc comment (one process-wide
 * slot, snapshotted per-worker at startScan time). */
void
ScanOrchestration_setDeviceFoundCallback(ScanOrchestrationHandle handle,
        ScanOrchestrationDeviceFoundCallback callback, void* userParam);

/*
 * Starts a new continuous background scan: sweep subnet -> diff against this
 * scan's own seen-set -> publish each genuinely new host to the shared
 * ScanDispatcher (and to the optional foundCallback) -> interruptible sleep
 * for sweepIntervalMs -> repeat, until ScanOrchestration_stopScan(scanId) is
 * called. On the very first active scan (0->1), also starts the shared
 * ScanDispatcher's websocket (bind + service thread) - a bind failure here
 * fails the WHOLE startScan call (SCAN_ORCHESTRATION_ERR_DISPATCHER_START_FAILED),
 * no worker is left running. Multiple concurrent scans (different
 * interfaces/mmsPorts) are fully supported and share the same one
 * ScanDispatcher instance. *outScanId is set only on SCAN_ORCHESTRATION_OK.
 */
ScanOrchestrationError
ScanOrchestration_startScan(ScanOrchestrationHandle handle, const ScanRequest* request, uint64_t* outScanId);

/*
 * Stops the scan identified by scanId. BLOCKING - see
 * ScanOrchestrationWorker_stop's own doc comment for the exact bound (an
 * in-flight sweep cannot be cancelled). If this was the last active scan
 * (1->0), also stops (fully tears down, same as ScanDispatcher_stop) the
 * shared ScanDispatcher. Returns SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND if
 * scanId isn't currently active (already stopped, or never existed).
 */
ScanOrchestrationError
ScanOrchestration_stopScan(ScanOrchestrationHandle handle, uint64_t scanId);

/*
 * Thread-safe read-only snapshot of scanId's currently-announced host list -
 * for a LOCAL, in-process caller (e.g. a discovery-prompt adapter) that
 * needs the live, growing list WITHOUT connecting to the daemon's own
 * websocket as a client of itself. *outHosts and *outCount are set only on
 * SCAN_ORCHESTRATION_OK. Caller frees via
 * ScanOrchestration_freeDiscoveredHostsSnapshot.
 *
 * CALLER CONTRACT: must not be called concurrently with
 * ScanOrchestration_stopScan for the SAME scanId from a different thread
 * (the worker object this reads from is destroyed by stopScan) - fine for a
 * single-threaded prompt loop that only calls stopScan after its own loop
 * has returned, same class of caller contract as ScanDispatcher_stop's own
 * "never call from within a callback" rule.
 */
ScanOrchestrationError
ScanOrchestration_snapshotDiscoveredHosts(ScanOrchestrationHandle handle, uint64_t scanId,
        char*** outHosts, int* outCount);

void
ScanOrchestration_freeDiscoveredHostsSnapshot(char** hosts, int count);

/* Stops every still-active scan (draining via the public stopScan path, one
 * at a time), destroys the registry, destroys the shared ScanDispatcher
 * (implies _stop if still running - harmless no-op if the drain above
 * already brought it to 0), frees the handle. NULL-safe. */
void
ScanOrchestration_destroy(ScanOrchestrationHandle handle);

#endif /* SCAN_ORCHESTRATION_API_H_ */
