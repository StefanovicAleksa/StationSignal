#ifndef ORCHESTRATION_API_H_
#define ORCHESTRATION_API_H_

#include "linked_list.h"
#include "orchestration/domain/orchestration_types.h"

/*
 * Public boundary of the orchestration layer. Other callers (main.c, a
 * future ipc_dispatcher) should only ever include this header - never reach
 * into domain/data/utils directly.
 *
 * Sequences, for a single IED: scl_bootstrap (probe hostList, fetch SCL
 * bytes) -> stage those bytes to a temp file -> ied_model (load from that
 * file) -> mms_report_client (start against the winning candidate's own
 * host) -> goose_subscriber (start on interfaceId). Both long-running
 * workers are handed the same IedModelHandle, which this layer then owns for
 * their combined lifetime.
 */

void
OrchestrationConfig_defaults(OrchestrationConfig* config);

/* Allocates only - no I/O. config: NULL means OrchestrationConfig_defaults(). */
OrchestrationHandle
Orchestration_create(const OrchestrationConfig* config, OrchestrationError* outError);

/*
 * All Orchestration_set*Callback functions must be called before
 * Orchestration_run() - mirrors MmsReportClient/GooseSubscription's own
 * "setters read only at _start()" contract: Orchestration_run() registers
 * each stored callback on the underlying sub-feature handle right before
 * calling that sub-feature's own _start(). Passthrough only - no wrapping,
 * no orchestration-specific record types; callback typedefs are reused
 * directly from the wrapped features.
 */
void Orchestration_setReportCallback(OrchestrationHandle handle,
        MmsReportClientCallback callback, void* userParam);
void Orchestration_setReportConnStateCallback(OrchestrationHandle handle,
        MmsReportClientConnStateCallback callback, void* userParam);
void Orchestration_setRcbStatusCallback(OrchestrationHandle handle,
        MmsReportClientRcbStatusCallback callback, void* userParam);
void Orchestration_setGooseRecordCallback(OrchestrationHandle handle,
        GooseSubscriberCallback callback, void* userParam);
void Orchestration_setGooseStatusCallback(OrchestrationHandle handle,
        GooseSubscriberStatusCallback callback, void* userParam);
/* Optional, observational only - fires synchronously on the calling thread
 * during the (potentially slow) bootstrap scan, same contract as
 * SclBootstrap_setProgressCallback itself. */
void Orchestration_setBootstrapProgressCallback(OrchestrationHandle handle,
        SclBootstrapProgressCallback callback, void* userParam);

/*
 * Blocking. Runs, in order: (1) SclBootstrap_scanAndFetch over hostList/
 * mmsPort, picking the first FILE_RETRIEVED candidate; (2) stages its bytes
 * to a temp file, deleted immediately after step 3 regardless of outcome;
 * (3) IedModel_loadFromFile(tempPath, iedName, accessMode); (4)
 * MmsReportClient_create+_start against the winning candidate's own host/
 * port (not a separately supplied parameter - bootstrap and reporting
 * target the same physical IED) using the loaded model; (5)
 * GooseSubscription_create+_start on interfaceId using the same model.
 *
 * Returns once both long-running workers' own _start() calls have returned
 * - this does NOT mean a report/GOOSE frame has arrived yet or the MMS
 * association is fully up (see MmsReportClient_start/GooseSubscription_start's
 * own "non-blocking, own background thread" docs) - only that synchronous
 * setup succeeded.
 *
 * Not re-entrant: returns ORCHESTRATION_ERR_INVALID_ARGUMENT immediately if
 * the handle is already running from a prior successful _run() (call
 * Orchestration_stop() first). On any stage's failure, everything started by
 * earlier stages in THIS call is torn down (reverse order) before
 * returning - the handle is left in a clean, re-runnable state.
 *
 * outDetail is optional (NULL-safe), filled only on error.
 */
OrchestrationError
Orchestration_run(OrchestrationHandle handle, LinkedList hostList, int mmsPort,
        const char* iedName, const char* interfaceId, AccessMode accessMode,
        OrchestrationErrorDetail* outDetail);

/*
 * Stops goose_subscriber, then mms_report_client, then releases ied_model -
 * exact reverse of startup order. Blocking (both underlying _stop() calls
 * block). Must be called from the caller's own thread, never from within a
 * registered callback (same deadlock rule as the two wrapped features).
 * Safe to call repeatedly / on a never-run handle (no-op).
 */
void
Orchestration_stop(OrchestrationHandle handle);

/* Implies Orchestration_stop() if still running. Frees the handle. */
void
Orchestration_destroy(OrchestrationHandle handle);

#endif /* ORCHESTRATION_API_H_ */
