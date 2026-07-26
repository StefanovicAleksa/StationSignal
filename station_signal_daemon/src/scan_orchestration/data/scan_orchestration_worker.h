#ifndef SCAN_ORCHESTRATION_WORKER_H_
#define SCAN_ORCHESTRATION_WORKER_H_

#include <stdint.h>
#include "scan_orchestration/domain/scan_orchestration_types.h"

/*
 * One continuously-running background scan: owns a PRIVATE IedDiscoveryHandle
 * and its own seen-set (hosts already announced), looping
 * sweep -> diff against seen-set -> publish each genuinely new host (to the
 * shared ScanDispatcher and the optional caller callback) -> interruptible
 * sleep -> repeat, until stopped. Opaque outside this file.
 */
typedef struct sScanOrchestrationWorker* ScanOrchestrationWorker;

/*
 * Allocates + creates its own private IedDiscoveryHandle (discoveryConfigTemplate
 * copied verbatim except .acseAuthPassword, which is overridden from
 * request->acseAuthPassword) - no thread started yet, no I/O beyond that one
 * allocation (mirrors every sibling feature's "no I/O at create" contract).
 * request->interfaceId/acseAuthPassword are deep-copied internally - the
 * request struct itself need not outlive this call. scanDispatcher is
 * borrowed (owned by the caller's ScanOrchestrationHandle). Returns NULL +
 * *outError on bad arguments, allocation failure, or IedDiscovery_create
 * failure.
 */
ScanOrchestrationWorker
ScanOrchestrationWorker_create(uint64_t scanId, const ScanRequest* request,
        const IedDiscoveryConfig* discoveryConfigTemplate, uint32_t defaultSweepIntervalMs,
        ScanDispatcherHandle scanDispatcher,
        ScanOrchestrationDeviceFoundCallback foundCallback, void* foundCallbackParam,
        ScanOrchestrationError* outError);

/* Thread_create/Thread_start running the sweep loop. Non-blocking. */
ScanOrchestrationError
ScanOrchestrationWorker_start(ScanOrchestrationWorker worker);

/*
 * Signals stopRequested, blocks until the worker thread has genuinely
 * exited. KNOWN, ACCEPTED LIMITATION: IedDiscovery_scanSubnet has no
 * cancellation hook, so if a sweep is in flight when this is called, this
 * call blocks until that sweep's own IedDiscovery_scanSubnet call returns on
 * its own - worst case bounded by
 * (tcpProbeTimeoutMs * ceil(hostCount / maxConcurrentTcpProbes)) +
 * (mmsConnectTimeoutMs * tcpSurvivorCount), which at this feature's own
 * defaults (500ms/64/3000ms) against a full /24 could be several seconds,
 * and against a larger allowed subnet could be tens of seconds. MUST be
 * called from the caller's own thread, never from within foundCallback
 * (deadlock - same rule as every _stop() in this codebase). Safe to call
 * more than once / before start (no-op).
 */
void
ScanOrchestrationWorker_stop(ScanOrchestrationWorker worker);

/* Implies _stop() if still running. Destroys the private IedDiscoveryHandle,
 * frees the seen-set array + its strings, frees the handle. NULL-safe. */
void
ScanOrchestrationWorker_destroy(ScanOrchestrationWorker worker);

uint64_t
ScanOrchestrationWorker_scanId(ScanOrchestrationWorker worker);

/*
 * Thread-safe snapshot of every host announced so far by this worker (its
 * own seen-set, guarded by a lock private to this worker - NOT the
 * registry's lock). Returns a heap array of heap-owned copied strings
 * (*outCount entries; NULL/0 if empty - not an error). Caller owns both
 * array and strings: ScanOrchestrationWorker_freeSnapshot.
 */
char**
ScanOrchestrationWorker_snapshotHosts(ScanOrchestrationWorker worker, int* outCount);

void
ScanOrchestrationWorker_freeSnapshot(char** hosts, int count);

#endif /* SCAN_ORCHESTRATION_WORKER_H_ */
