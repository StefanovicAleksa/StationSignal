#ifndef MMS_REPORT_CLIENT_API_H_
#define MMS_REPORT_CLIENT_API_H_

#include "linked_list.h"
#include "features/ied_model/service/ied_model_api.h"
#include "features/mms_report_client/domain/mms_report_client_types.h"

/*
 * Public boundary of the mms_report_client feature. Other features
 * (ipc_dispatcher, ...) should only ever include this header - never reach
 * into domain/data/utils directly.
 *
 * Connects to one IED over MMS, discovers its Report Control Blocks (BRCB/
 * URCB) via the already-loaded IedModelHandle (never re-parses SCL, never
 * discovers RCBs over the wire), enables reporting on all of them, and
 * delivers normalized reports via a caller-registered callback. Works with
 * an IedModelHandle in ANY AccessMode, including IED_MODEL_ACCESS_REPORT_ONLY
 * - it only ever calls IedModel_getReportSubscriptionTargets, which is
 * always available regardless of mode.
 */

/* Fills config with the recommended defaults. Caller may then override
 * individual fields before passing to MmsReportClient_create. */
void
MmsReportClientConfig_defaults(MmsReportClientConfig* config);

/*
 * iedModel: borrowed - caller retains ownership and must not release it
 * before calling MmsReportClient_destroy. mms_report_client only reads
 * targets from it once, at MmsReportClient_start().
 * host: caller-supplied (SCL has no MMS ConnectedAP IP-address parsing yet -
 * out of scope for this feature). Copied internally.
 * config: NULL means MmsReportClientConfig_defaults().
 * Returns NULL and sets *outError on argument/allocation failure only - this
 * does NOT attempt to connect (see MmsReportClient_start).
 */
MmsReportClientHandle
MmsReportClient_create(IedModelHandle iedModel, const char* host, int port,
        const MmsReportClientConfig* config, MmsReportClientError* outError);

/* Must be called before MmsReportClient_start(). callback is invoked once per
 * fully-decoded report. The delivered MmsReportRecord is OWNED BY THE CALLER
 * after the callback returns - it must eventually call
 * MmsReportClient_destroyReportRecord on it. userParam is passed through
 * unchanged. Not safe to call from within the report callback itself. */
void
MmsReportClient_setReportCallback(MmsReportClientHandle client,
        MmsReportClientCallback callback, void* userParam);

/* Optional (may be left unset/NULL). Fires on connect/disconnect/backoff
 * transitions - purely observational (logging/metrics), never required for
 * correct report delivery. Must not block or call back into this feature's
 * API (fires from libiec61850's state-changed callback context). */
void
MmsReportClient_setConnectionStateCallback(MmsReportClientHandle client,
        MmsReportClientConnStateCallback callback, void* userParam);

/* Optional (may be left unset/NULL). Fires once per RCB after each enable
 * ATTEMPT (initial connect or post-reconnect re-enable), success or failure.
 *
 * "Attempt" is load-bearing: an RCB the device does not need this cycle - a
 * spare Dyn slot left with nothing to cover once whole-device clustering ran
 * out of clusters, which is the common case on hardware carrying dozens of
 * redundant RCB instances - is never touched at all, so it makes no attempt
 * and fires NOTHING here. It is neither enabled nor failed; it simply wasn't
 * used. Only a genuine failure (dataset needed but unobtainable, or a
 * rejected DatSet/RptEna write) fires with enabled=false. Do not infer "every
 * RCB in the model reports here exactly once per cycle" - count the enabled
 * ones, or read the daemon's own per-cycle summary log line. */
void
MmsReportClient_setRcbStatusCallback(MmsReportClientHandle client,
        MmsReportClientRcbStatusCallback callback, void* userParam);

/*
 * Reads IedModel_getReportSubscriptionTargets(iedModel) once, then starts the
 * reconnect supervisor thread, which performs the actual connect and RCB
 * enable sequence asynchronously. Non-blocking. Returns an error only for
 * synchronous setup failures (e.g. empty target list, thread-create failure)
 * - connection failures are handled by the reconnect loop, not returned here.
 */
MmsReportClientError
MmsReportClient_start(MmsReportClientHandle client);

/*
 * Stops the reconnect supervisor thread, uninstalls all report handlers,
 * closes the connection. Blocking - waits for the supervisor thread to exit.
 * MUST be called from the caller's own thread, NEVER from within a callback
 * registered above (deadlock). Safe to call more than once / on an
 * already-stopped or never-started client (no-op).
 */
void
MmsReportClient_stop(MmsReportClientHandle client);

/* Implies MmsReportClient_stop() if still running. Frees the handle. */
void
MmsReportClient_destroy(MmsReportClientHandle client);

/* Frees a MmsReportRecord delivered via the report callback, including its
 * entries array and every entry's cloned MmsValue/reference string. NULL-safe. */
void
MmsReportClient_destroyReportRecord(MmsReportRecord* record);

#endif /* MMS_REPORT_CLIENT_API_H_ */
