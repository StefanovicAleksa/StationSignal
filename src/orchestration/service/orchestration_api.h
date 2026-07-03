#ifndef ORCHESTRATION_API_H_
#define ORCHESTRATION_API_H_

#include "linked_list.h"
#include "orchestration/domain/orchestration_types.h"

/*
 * Public boundary of the orchestration layer. Other callers (main.c) should
 * only ever include this header - never reach into domain/data/utils
 * directly, and never include features/ipc_dispatcher/service/
 * ipc_dispatcher_api.h directly either: this layer owns ipc_dispatcher's
 * entire lifecycle end-to-end (create/start/stop/destroy, plus wiring its
 * callbacks onto mms_report_client/goose_subscriber), the same way it owns
 * ied_model/mms_report_client/goose_subscriber's lifecycles - main.c only
 * ever configures it via OrchestrationConfig.ipcDispatcherConfig.
 *
 * Sequences, for a single IED: ipc_dispatcher (bind + start its websocket
 * service thread - first, deliberately, so a bind failure fails fast before
 * touching the network-facing MMS/GOOSE side at all) -> scl_bootstrap (probe
 * hostList, fetch SCL bytes) -> stage those bytes to a temp file -> ied_model
 * (load from that file) -> mms_report_client (start against the winning
 * candidate's own host, with its report callback unconditionally wired to
 * ipc_dispatcher) -> goose_subscriber (start on interfaceId, same wiring).
 * Both long-running workers are handed the same IedModelHandle, which this
 * layer then owns for their combined lifetime.
 */

void
OrchestrationConfig_defaults(OrchestrationConfig* config);

/*
 * Allocates only - no I/O for scl_bootstrap/ied_model/mms_report_client/
 * goose_subscriber, matching their own "no I/O at create" contracts.
 * ipc_dispatcher is the one exception worth calling out: its own ring buffer
 * allocation happens here (still no I/O - IpcDispatcher_create itself never
 * binds), but its real bind only happens later, at Orchestration_run's first
 * stage. config: NULL means OrchestrationConfig_defaults().
 */
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
 *
 * Note there is no Orchestration_setReportCallback/_setGooseRecordCallback -
 * those DATA-record slots are always, unconditionally wired to
 * IpcDispatcher_onMmsReport/_onGooseRecord by Orchestration_run itself (see
 * ipc_dispatcher's own ownership-transfer contract: exactly one consumer may
 * ever own+destroy a given record, and ipc_dispatcher is that consumer).
 * Only the diagnostic/status callbacks below remain caller-configurable.
 */
void Orchestration_setReportConnStateCallback(OrchestrationHandle handle,
        MmsReportClientConnStateCallback callback, void* userParam);
void Orchestration_setRcbStatusCallback(OrchestrationHandle handle,
        MmsReportClientRcbStatusCallback callback, void* userParam);
void Orchestration_setGooseStatusCallback(OrchestrationHandle handle,
        GooseSubscriberStatusCallback callback, void* userParam);
/* Optional, observational only - fires synchronously on the calling thread
 * during the (potentially slow) bootstrap scan, same contract as
 * SclBootstrap_setProgressCallback itself. */
void Orchestration_setBootstrapProgressCallback(OrchestrationHandle handle,
        SclBootstrapProgressCallback callback, void* userParam);

/*
 * Blocking. Runs, in order: (0) IpcDispatcher_start (bind 127.0.0.1:
 * config.ipcDispatcherConfig.port, start its service thread); (1)
 * SclBootstrap_scanAndFetch over hostList/mmsPort, picking the first
 * FILE_RETRIEVED candidate; (2) stages its bytes to a temp file, deleted
 * immediately after step 3 regardless of outcome; (3)
 * IedModel_loadFromFile(tempPath, iedName, accessMode); (4)
 * MmsReportClient_create+_start against the winning candidate's own host/
 * port (not a separately supplied parameter - bootstrap and reporting
 * target the same physical IED) using the loaded model, with its report
 * callback unconditionally set to IpcDispatcher_onMmsReport; (5)
 * GooseSubscription_create+_start on interfaceId using the same model, same
 * unconditional IpcDispatcher_onGooseRecord wiring.
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
 * returning - the handle is left in a clean, re-runnable state (this
 * includes stopping ipc_dispatcher if a LATER stage fails, since it's always
 * started first).
 *
 * outDetail is optional (NULL-safe), filled only on error.
 */
OrchestrationError
Orchestration_run(OrchestrationHandle handle, LinkedList hostList, int mmsPort,
        const char* iedName, const char* interfaceId, AccessMode accessMode,
        OrchestrationErrorDetail* outDetail);

/*
 * Stops goose_subscriber, then mms_report_client, then ipc_dispatcher (in
 * that order - guarantees no more producer-thread calls can land on
 * ipc_dispatcher once it's torn down), then releases ied_model. Blocking
 * (every underlying _stop() call blocks). Must be called from the caller's
 * own thread, never from within a registered callback (same deadlock rule
 * as the wrapped features). Safe to call repeatedly / on a never-run handle
 * (no-op) - including tearing down ipc_dispatcher even if Orchestration_run
 * was never called or failed before reaching later stages, since
 * ipc_dispatcher exists from Orchestration_create onward.
 */
void
Orchestration_stop(OrchestrationHandle handle);

/* Implies Orchestration_stop() if still running. Frees the handle, including
 * ipc_dispatcher (which always exists once Orchestration_create succeeds). */
void
Orchestration_destroy(OrchestrationHandle handle);

#endif /* ORCHESTRATION_API_H_ */
