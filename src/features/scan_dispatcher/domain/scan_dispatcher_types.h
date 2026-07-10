#ifndef SCAN_DISPATCHER_TYPES_H_
#define SCAN_DISPATCHER_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Domain vocabulary for this feature is entirely its own (ScanDeviceFoundEvent
 * is the JSON contract itself), same reasoning as ipc_dispatcher_types.h: the
 * one third-party output (cJSON) is touched nowhere in this file or in
 * domain/scan_dispatcher_usecases.c - see data/scan_dispatcher_json_writer.h
 * for the one place cJSON.h is included, and data/scan_dispatcher_ws_server.h
 * for the one place libwebsockets.h is included.
 *
 * This feature is a near-verbatim duplicate of ipc_dispatcher's ring-buffer +
 * websocket-server transport, deliberately NOT extracted into a shared
 * cross-feature module - see this codebase's existing convention of
 * duplicating small third-party-integration code per feature (ACSE-auth setup
 * is already duplicated 4x) rather than inventing a new "shared" directory
 * concept that has no precedent anywhere in this codebase.
 */

typedef enum {
    SCAN_DISPATCHER_OK = 0,
    SCAN_DISPATCHER_ERR_INVALID_ARGUMENT,
    SCAN_DISPATCHER_ERR_OUT_OF_MEMORY,
    SCAN_DISPATCHER_ERR_THREAD_CREATE_FAILED,
    SCAN_DISPATCHER_ERR_SOCKET_BIND_FAILED,
    SCAN_DISPATCHER_ERR_ALREADY_RUNNING
} ScanDispatcherError;

typedef struct {
    uint16_t port;           /* default 8766 - deliberately distinct from ipc_dispatcher's
                                 8765, since both can be bound in the same process (main.c's
                                 scan flow runs before Orchestration_run starts ipc_dispatcher) */
    int ringBufferCapacity;  /* default 256 */
    int maxConnections;      /* default 16 */
} ScanDispatcherConfig;

/* One "device found" event - the JSON contract's own vocabulary, not
 * borrowed from ied_discovery/scan_orchestration. */
typedef struct {
    uint64_t scanId;
    char* host;             /* owned copy */
    int mmsPort;
    uint64_t discoveredAtMs;
} ScanDeviceFoundEvent;

/*
 * Opaque forward declarations only - full struct defs live entirely inside
 * data/scan_dispatcher_ring_buffer.c / data/scan_dispatcher_ws_server.c,
 * same convention as ipc_dispatcher_types.h (kept as bare tags so this
 * header never pulls in hal_thread.h/libwebsockets.h).
 */
struct sScanDispatcherRingBuffer;
struct sScanDispatcherWsServer;

struct sScanDispatcherHandle {
    ScanDispatcherConfig config;
    struct sScanDispatcherRingBuffer* ringBuffer; /* owned, created in ScanDispatcher_create */
    struct sScanDispatcherWsServer* wsServer;     /* owned, created/destroyed in _start/_stop - NULL when not running */
    volatile bool running;
};

typedef struct sScanDispatcherHandle* ScanDispatcherHandle;

#endif /* SCAN_DISPATCHER_TYPES_H_ */
