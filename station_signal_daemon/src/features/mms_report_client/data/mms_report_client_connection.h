#ifndef MMS_REPORT_CLIENT_CONNECTION_H_
#define MMS_REPORT_CLIENT_CONNECTION_H_

#include "features/mms_report_client/domain/mms_report_client_types.h"

/*
 * All IedConnection/libiec61850 third-party integration for this feature
 * lives here: connection creation, the reconnect supervisor thread +
 * semaphore, and the per-RCB enable sequence. Deliberately not unit-tested
 * in depth - proven E2E, mirroring ied_model's scl_loader convention (a
 * live IedConnection can't be meaningfully faked in a hermetic unit test).
 */

/* Creates the IedConnection (IedConnection_createEx(NULL, true) - library-
 * internal reception thread, no manual tick() polling) and installs the
 * state-changed handler. Does not connect yet. */
MmsReportClientError
MmsReportClientConnection_create(MmsReportClientHandle handle);

/* Starts the reconnect supervisor thread, which immediately attempts the
 * first connect (no backoff delay on the first try) and, on success, runs
 * the RCB-enable sequence for every cached target; on failure or later
 * disconnect, retries with exponential backoff (capped). Non-blocking -
 * returns as soon as the thread is created. */
MmsReportClientError
MmsReportClientConnection_start(MmsReportClientHandle handle);

/* Stops the supervisor thread, uninstalls all report handlers, closes the
 * connection. Blocking - waits (bounded) for the supervisor thread to exit.
 * MUST be called from the caller's own thread, never from within a
 * registered callback (deadlock risk). Safe to call more than once. */
void
MmsReportClientConnection_stop(MmsReportClientHandle handle);

/* Destroys the IedConnection and the wake semaphore. Call after _stop(). */
void
MmsReportClientConnection_destroy(MmsReportClientHandle handle);

#endif /* MMS_REPORT_CLIENT_CONNECTION_H_ */
