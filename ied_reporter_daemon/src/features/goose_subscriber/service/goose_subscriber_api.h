#ifndef GOOSE_SUBSCRIBER_API_H_
#define GOOSE_SUBSCRIBER_API_H_

#include "features/ied_model/service/ied_model_api.h"
#include "features/goose_subscriber/domain/goose_subscriber_types.h"

/*
 * Public boundary of the goose_subscriber feature. Other features
 * (ipc_dispatcher, ...) should only ever include this header - never reach
 * into domain/data/utils directly.
 *
 * Subscribes to every GOOSE Control Block discovered by ied_model
 * (IedModel_getGooseSubscriptionTargets - never re-parses SCL, never
 * discovers GoCBs over the wire) on one Ethernet interface, and delivers
 * normalized GooseSubscriberRecords via a caller-registered callback. Works
 * with an IedModelHandle in ANY AccessMode, including
 * IED_MODEL_ACCESS_REPORT_ONLY - it only ever calls
 * IedModel_getGooseSubscriptionTargets, which is always available regardless
 * of mode.
 */

/* Fills config with the recommended defaults. Caller may then override
 * individual fields before passing to GooseSubscription_create. */
void
GooseSubscriberConfig_defaults(GooseSubscriberConfig* config);

/*
 * iedModel: borrowed - caller retains ownership and must not release it
 * before calling GooseSubscription_destroy. goose_subscriber only reads
 * targets from it once, at GooseSubscription_start().
 * interfaceId: e.g. "eth0" - copied internally. Required (SCL has no
 * interface-name parsing; only the publisher-side VLAN/APPID/MAC addressing
 * is parsed by ied_model, and that's used for per-target filtering, not for
 * choosing which local NIC to listen on).
 * config: NULL means GooseSubscriberConfig_defaults().
 * Returns NULL and sets *outError on argument/allocation failure only - this
 * does NOT open any socket yet (see GooseSubscription_start).
 */
GooseSubscriberHandle
GooseSubscription_create(IedModelHandle iedModel, const char* interfaceId,
        const GooseSubscriberConfig* config, GooseSubscriberError* outError);

/* Must be called before GooseSubscription_start(). Invoked once per received
 * GOOSE frame that decodes successfully (test=true/needsCommission=true
 * frames ARE still forwarded - filtering those is the caller's policy
 * decision, not silently dropped here, see goose_subscriber_frame_adapter.c).
 * The delivered GooseSubscriberRecord is OWNED BY THE CALLER after the
 * callback returns - it must eventually call GooseSubscription_destroyRecord.
 * Fires from GooseReceiver's internal reception thread - must be fast, must
 * not block, must not call back into this feature's own API. */
void
GooseSubscription_setRecordCallback(GooseSubscriberHandle handle,
        GooseSubscriberCallback callback, void* userParam);

/* Optional (may be left unset/NULL). Fires on a VALID<->non-VALID transition
 * for a single target from the liveness timer thread (GOOSE has no
 * connection-lost push signal, so staleness is detected by a narrow, low-rate
 * poll of GooseSubscriber_isValid() instead - see
 * data/goose_subscriber_connection.c) - purely observational, never required
 * for record delivery. Must not block or call back into this feature's API. */
void
GooseSubscription_setStatusCallback(GooseSubscriberHandle handle,
        GooseSubscriberStatusCallback callback, void* userParam);

/*
 * Reads IedModel_getGooseSubscriptionTargets(iedModel) once, builds one
 * GooseSubscriber per target (applying dst-MAC/APPID filters from SCL
 * addressing when available), attaches them all to one GooseReceiver, calls
 * GooseReceiver_start(), then starts the liveness timer thread. Blocking only
 * for this synchronous setup (socket bind + thread create) - reception itself
 * is async via the record callback. Returns an error for an empty target
 * list, socket/thread-create failure. Idempotent: calling twice on an
 * already-started handle is a no-op returning GOOSE_SUBSCRIBER_OK.
 */
GooseSubscriberError
GooseSubscription_start(GooseSubscriberHandle handle);

/*
 * Stops the liveness timer thread (bounded, prompt) and the GooseReceiver.
 * Blocking. MUST be called from the caller's own thread, never from within a
 * registered callback (deadlock). Safe to call more than once / on a
 * never-started handle (no-op).
 */
void
GooseSubscription_stop(GooseSubscriberHandle handle);

/* Implies GooseSubscription_stop() if still running. Frees the handle
 * (including the GooseReceiver, which cascades to every GooseSubscriber). */
void
GooseSubscription_destroy(GooseSubscriberHandle handle);

/* Frees a GooseSubscriberRecord delivered via the record callback, including
 * its entries array and every entry's cloned MmsValue. NULL-safe. */
void
GooseSubscription_destroyRecord(GooseSubscriberRecord* record);

#endif /* GOOSE_SUBSCRIBER_API_H_ */
