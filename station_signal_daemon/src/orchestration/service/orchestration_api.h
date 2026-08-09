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

/*
 * Convenience wrapper for the one caller-side use case that needs the
 * connState slot but has no callback of its own to supply: routes
 * MmsReportClientConnState transitions to ipc_dispatcher's own
 * IpcDispatcher_onConnStateChange (CONNECTION_STATUS push on the per-device
 * stream), the same way report/GOOSE data is always wired to ipc_dispatcher -
 * without exposing ipc_dispatcher's handle/header to the caller (see this
 * header's own top comment on why that boundary matters). Mutually exclusive
 * with Orchestration_setReportConnStateCallback - the underlying connState
 * slot is singular; whichever was called most recently before Orchestration_run()
 * wins. Must be called before Orchestration_run(), same as every setter here.
 */
void Orchestration_wireConnStatusToIpcDispatcher(OrchestrationHandle handle);
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
 * FILE_RETRIEVED candidate; (2) stages its bytes to a temp file; (2.5) if
 * iedName is NULL/empty, IedModel_listIedNames(tempPath) - exactly one
 * result is used silently, zero or more than one fails with
 * ORCHESTRATION_ERR_IED_NAME_RESOLUTION_FAILED (no interactive retry - an
 * accepted limitation, see this feature's own Architecture bullet in
 * CLAUDE.md); (3) IedModel_loadFromFile(tempPath, resolved iedName,
 * accessMode) - the temp file is deleted immediately after this step
 * regardless of outcome; (4) MmsReportClient_create+_start against the
 * winning candidate's own host/port (not a separately supplied parameter -
 * bootstrap and reporting target the same physical IED) using the loaded
 * model, with its report callback unconditionally set to
 * IpcDispatcher_onMmsReport; (5) GooseSubscription_create+_start on
 * interfaceId using the same model, same unconditional
 * IpcDispatcher_onGooseRecord wiring.
 *
 * iedName: NULL or "" triggers auto-detection from the staged SCL (see step
 * 2.5 above) - useful when the caller (e.g. ied_discovery's interactive
 * picker) knows a host to bootstrap from but not its exact SCL-declared IED
 * name. A non-empty iedName is used exactly as before, unchanged.
 *
 * lnCategoryFilter: passed straight through to step (3)'s
 * IedModel_loadFromFile call, stored on the resulting handle - IED_MODEL_LN_CATEGORY_ALL
 * matches every LN (today's unfiltered behavior); a narrower mask excludes
 * non-matching LNs from steps (4)/(5)'s subscription target lists and from
 * mms_report_client's own whole-device dynamic-dataset clustering. See
 * ied_model's own categoryFilter field (ied_model_types.h) for where
 * filtering actually happens.
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
 *
 * outMmsAvailable/outGooseAvailable are optional (NULL-safe) and, on
 * ORCHESTRATION_OK, report which of the two protocols this device's SCL
 * actually declared targets for. A device with SCL declaring zero
 * <ReportControl> blocks (or zero <GSEControl> blocks) is no longer a fatal
 * error on its own - that one protocol is simply skipped and the
 * corresponding out-param is set false, while the other one still starts
 * normally. Only a device with BOTH zero - nothing to monitor at all - fails,
 * with ORCHESTRATION_ERR_NO_CAPABILITIES. Left unset (stale) on any other
 * failure path.
 */
OrchestrationError
Orchestration_run(OrchestrationHandle handle, LinkedList hostList, int mmsPort,
        const char* iedName, const char* interfaceId, AccessMode accessMode, LnCategoryMask lnCategoryFilter,
        OrchestrationErrorDetail* outDetail, bool* outMmsAvailable, bool* outGooseAvailable);

/*
 * Same end state as Orchestration_run, but skips scl_bootstrap/staging
 * entirely: sclFilePath is read directly (never modified or deleted - it's
 * the caller's own file, not staged/owned by this handle) instead of being
 * fetched over MMS from host. Exists for IEDs/simulators whose MMS server
 * doesn't implement file services (so scl_bootstrap can never succeed
 * against them) but whose SCL is available locally some other way (e.g.
 * exported from the tool driving the simulation).
 *
 * host/mmsPort are still required and still drive the real
 * mms_report_client/goose_subscriber connections for live reporting -
 * loading the model locally only replaces *how the SCL description is
 * obtained*, not the live MMS/GOOSE connection to the real device. iedName
 * empty still triggers the same auto-detect-from-SCL behavior as
 * Orchestration_run.
 *
 * Blocking, same stage semantics as Orchestration_run (IPC_DISPATCHER_START
 * -> IED_NAME_RESOLUTION (if iedName empty) -> MODEL_LOAD ->
 * REPORT_CLIENT_START -> GOOSE_SUBSCRIBER_START) - BOOTSTRAP/STAGING stages
 * are simply never reached. Same fail-hard rollback contract, same
 * re-runnable-after-failure guarantee, same re-entrancy check.
 *
 * outMmsAvailable/outGooseAvailable: same contract as Orchestration_run's own.
 */
OrchestrationError
Orchestration_runFromLocalFile(OrchestrationHandle handle, const char* sclFilePath, const char* host,
        int mmsPort, const char* iedName, const char* interfaceId, AccessMode accessMode,
        LnCategoryMask lnCategoryFilter, OrchestrationErrorDetail* outDetail, bool* outMmsAvailable,
        bool* outGooseAvailable);

/*
 * A THIRD way to obtain the model - for real, connectable IEC 61850 devices
 * whose MMS server doesn't implement file services at all (so scl_bootstrap
 * can never succeed against them, unlike Orchestration_runFromLocalFile's
 * scenario, which still needs an operator to have a local SCL file some
 * other way - e.g. OMICRON IED Scout's "Simulate IED" mode, confirmed to
 * return SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND from an otherwise fully
 * working, associable device). Instead of parsing an SCL file at all, walks
 * the live device's own MMS ACSI directory/model-discovery services
 * (ied_model_online_loader) to reconstruct an equivalent model directly -
 * see that feature's own service header for the full mechanism, and
 * CLAUDE.md's "No over-the-wire tree discovery" Hard Rule for why this is a
 * narrow, explicit exception rather than a general capability.
 *
 * This is a deliberately EXPLICIT, caller-invoked fallback, never a silent
 * branch inside Orchestration_run itself: online discovery is materially
 * slower and more request-heavy than one SCL file transfer + local parse
 * (many sequential MMS round-trips, scaling with model size), and changing
 * Orchestration_run's own behavior based on how bootstrap happened to fail
 * would violate this layer's existing fail-hard, no-silent-branching
 * contract. The intended caller-side policy (e.g. main.c or a future wiring
 * adapter) is: call Orchestration_run first; only on
 * ORCHESTRATION_ERR_BOOTSTRAP_FAILED with
 * outDetail.lastCandidateStatus == SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND,
 * retry via this function against the same host.
 *
 * host/mmsPort/interfaceId are required, same as Orchestration_runFromLocalFile
 * - there is no bootstrap step to derive them from a scanned candidate here
 * either. iedName only labels the constructed model (there is no SCL <IED>
 * list to auto-detect from over a live connection, so - unlike the other two
 * entry points - an empty iedName is NOT a resolution stage of its own; it's
 * passed straight through, defaulting inside ied_model_online_loader).
 * acseAuthPassword mirrors ied_discovery/scl_bootstrap's own optional
 * ACSE-password config, applied unconditionally to the one connection this
 * makes (no retry-on-rejection - this call always targets one already-known
 * host, the same posture mms_report_client already takes for the same
 * reason).
 *
 * Blocking. Stage sequence: IPC_DISPATCHER_START -> ONLINE_DISCOVERY (replaces
 * BOOTSTRAP/STAGING/MODEL_LOAD entirely) -> REPORT_CLIENT_START ->
 * GOOSE_SUBSCRIBER_START. Same fail-hard rollback contract, same
 * re-runnable-after-failure guarantee, same re-entrancy check as the other
 * two entry points.
 *
 * outMmsAvailable/outGooseAvailable: same contract as Orchestration_run's own.
 */
OrchestrationError
Orchestration_runFromOnlineDiscovery(OrchestrationHandle handle, const char* host, int mmsPort,
        const char* iedName, const char* interfaceId, AccessMode accessMode, LnCategoryMask lnCategoryFilter,
        const char* acseAuthPassword, OrchestrationErrorDetail* outDetail,
        bool* outMmsAvailable, bool* outGooseAvailable);

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
