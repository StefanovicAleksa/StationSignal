#ifndef SCAN_DISPATCHER_API_H_
#define SCAN_DISPATCHER_API_H_

#include "features/scan_dispatcher/domain/scan_dispatcher_types.h"

/*
 * Public boundary of scan_dispatcher. Other code (scan_orchestration)
 * should only ever include this header - never reach into domain/data
 * directly.
 *
 * Relays "device found" scan events over a loopback-only websocket, shared
 * across however many concurrent scans scan_orchestration is running -
 * this feature itself knows nothing about scans/interfaces/refcounting,
 * purely transport (see CLAUDE.md's scan_orchestration bullet for the
 * refcounting logic that decides when to call _start/_stop). Push-only (no
 * client-sent message handling). Near-verbatim structural duplicate of
 * ipc_dispatcher - see scan_dispatcher_types.h's own top comment for why
 * this is duplicated rather than shared.
 */

/* Fills config with the recommended defaults (port=8766, ringBufferCapacity=256,
 * maxConnections=16). Caller may then override individual fields before
 * passing to ScanDispatcher_create. */
void
ScanDispatcherConfig_defaults(ScanDispatcherConfig* config);

/*
 * Allocates only - no socket/thread yet (creates the ring buffer, which is
 * pure allocation, no I/O). config: NULL means ScanDispatcherConfig_defaults().
 * Returns NULL and sets *outError on argument/allocation failure only.
 */
ScanDispatcherHandle
ScanDispatcher_create(const ScanDispatcherConfig* config, ScanDispatcherError* outError);

/*
 * Binds 127.0.0.1:config.port and starts the dedicated libwebsockets
 * service-loop thread. Non-blocking once the synchronous bind succeeds.
 * Returns SCAN_DISPATCHER_ERR_ALREADY_RUNNING if called twice without an
 * intervening _stop(). Returns SCAN_DISPATCHER_ERR_SOCKET_BIND_FAILED if the
 * port is unavailable.
 */
ScanDispatcherError
ScanDispatcher_start(ScanDispatcherHandle handle);

/*
 * Stops the service-loop thread and closes the listening socket + every
 * connected client. Blocking. MUST be called from the caller's own thread,
 * never from within a callback (deadlock). Safe to call more than once / on
 * a never-started handle (no-op).
 */
void
ScanDispatcher_stop(ScanDispatcherHandle handle);

/* Implies ScanDispatcher_stop() if still running. Frees the handle
 * (including the ring buffer). */
void
ScanDispatcher_destroy(ScanDispatcherHandle handle);

/*
 * Non-blocking: serializes + enqueues + wakes the service thread. Safe to
 * call from any scan worker thread concurrently. No-op if handle is NULL or
 * not currently running.
 */
void
ScanDispatcher_publishDeviceFound(ScanDispatcherHandle handle, uint64_t scanId, const char* host, int mmsPort,
        bool authRequired);

#endif /* SCAN_DISPATCHER_API_H_ */
