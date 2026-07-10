#ifndef SCAN_ORCHESTRATION_REGISTRY_H_
#define SCAN_ORCHESTRATION_REGISTRY_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "scan_orchestration/domain/scan_orchestration_types.h"
#include "scan_orchestration/data/scan_orchestration_worker.h"

/*
 * Mutex-guarded bookkeeping of every currently-active scan: scanId
 * generation, the active-scan array, and the 0<->1 ScanDispatcher start/stop
 * refcounting. Opaque outside this file.
 *
 * TWO-PHASE LOCKING IS THE KEY DESIGN POINT: ScanOrchestrationWorker_stop can
 * block for the duration of an in-flight sweep (see that function's own doc
 * comment), so this registry must NEVER hold its lock across that wait -
 * otherwise one slow scan's stop would serialize every other concurrent
 * scan's start/stop behind it, directly violating "multiple concurrent scans
 * must run independently." _remove() therefore only removes the worker from
 * the bookkeeping array under a SHORT lock and hands it back to the caller,
 * who stops/destroys it (the potentially-slow part) entirely OUTSIDE any
 * lock, afterward.
 */
typedef struct sScanOrchestrationRegistry* ScanOrchestrationRegistry;

ScanOrchestrationRegistry
ScanOrchestrationRegistry_create(void);

/* Does NOT stop/destroy any workers still registered - caller
 * (ScanOrchestration_destroy) must drain every active scan via the public
 * stopScan path first. Only frees this registry's own bookkeeping array +
 * lock. NULL-safe. */
void
ScanOrchestrationRegistry_destroy(ScanOrchestrationRegistry registry);

/* Monotonic counter, starts at 1 (0 is never a valid scanId), bumped under
 * the lock - pure counter bump, no I/O, O(1). */
uint64_t
ScanOrchestrationRegistry_nextScanId(ScanOrchestrationRegistry registry);

/*
 * Atomically: if this would be the registry's FIRST active entry (0->1
 * transition), calls ScanDispatcher_start(scanDispatcher) BEFORE registering
 * anything - a bind failure here means nothing gets registered at all, and
 * *worker is left for the caller to destroy. On success, appends `worker` to
 * the internal array (realloc-doubling). Held under the registry lock for
 * its ENTIRE body - safe because every operation inside (a counter check, at
 * most one ScanDispatcher_start bind+thread-create, an array append) is fast
 * and bounded, unlike ScanOrchestrationWorker_stop's unbounded wait (see
 * _remove below for why THAT one is deliberately kept OUTSIDE this lock).
 */
ScanOrchestrationError
ScanOrchestrationRegistry_addAndMaybeStartDispatcher(ScanOrchestrationRegistry registry,
        ScanDispatcherHandle scanDispatcher, ScanOrchestrationWorker worker);

/*
 * Removes scanId's entry (if present) under the lock and returns it via
 * *outWorker - caller stops/destroys it OUTSIDE any lock (see this file's
 * own top comment). Sets *outNowEmpty=true if this removal brought the
 * active count to zero, so the caller can stop the dispatcher itself in a
 * SEPARATE, later, short critical section. Returns false (leaves *outWorker
 * untouched) if scanId isn't currently active.
 */
bool
ScanOrchestrationRegistry_remove(ScanOrchestrationRegistry registry, uint64_t scanId,
        ScanOrchestrationWorker* outWorker, bool* outNowEmpty);

/* Thread-safe lookup by scanId - NULL if not currently active. The returned
 * worker is borrowed; caller must not stop/destroy it via this pointer
 * (only _remove hands out ownership). */
ScanOrchestrationWorker
ScanOrchestrationRegistry_find(ScanOrchestrationRegistry registry, uint64_t scanId);

/* Number of currently-registered (active) scans - IS the scanDispatcher's
 * own reference count; no separate counter needed. */
int
ScanOrchestrationRegistry_activeCount(ScanOrchestrationRegistry registry);

/* Returns any one currently-active scanId (used only by
 * ScanOrchestration_destroy to drain the registry one scan at a time via
 * the public stopScan path) - 0 if empty (0 is never a valid scanId). */
uint64_t
ScanOrchestrationRegistry_anyActiveScanId(ScanOrchestrationRegistry registry);

#endif /* SCAN_ORCHESTRATION_REGISTRY_H_ */
