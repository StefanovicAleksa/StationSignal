#ifndef CONTROL_DISPATCHER_TYPES_H_
#define CONTROL_DISPATCHER_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "device_manager/service/device_manager_api.h"

/*
 * Domain vocabulary for this feature is its own JSON-command contract
 * (ControlRequest) plus device_manager's public API (this feature's whole
 * job is relaying StartReporting/StopReporting calls over a websocket, so
 * depending on device_manager - a top-level sibling, not another
 * src/features/ peer - is the point, not a layering violation). The two
 * third-party outputs (cJSON parse AND serialize) are touched nowhere in
 * this file - see data/control_dispatcher_json_parser.h/
 * data/control_dispatcher_json_writer.h for the only two places cJSON.h is
 * included, and data/control_dispatcher_ws_server.h for the one place
 * libwebsockets.h is included.
 *
 * Unlike ipc_dispatcher/scan_dispatcher (push-only transports), this feature
 * is bidirectional: it RECEIVES JSON commands over its one well-known
 * websocket port and pushes back JSON acks/errors - see this feature's own
 * Architecture bullet in CLAUDE.md for the full threading design.
 */

typedef enum {
    CONTROL_DISPATCHER_OK = 0,
    CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT,
    CONTROL_DISPATCHER_ERR_OUT_OF_MEMORY,
    CONTROL_DISPATCHER_ERR_THREAD_CREATE_FAILED,
    CONTROL_DISPATCHER_ERR_SOCKET_BIND_FAILED,
    CONTROL_DISPATCHER_ERR_ALREADY_RUNNING
} ControlDispatcherError;

typedef struct {
    uint16_t port;              /* default 8767 - next after ipc_dispatcher's 8765 and
                                    scan_dispatcher's 8766; ONE shared control channel, not
                                    per-device (only the per-device report websockets, from
                                    ipc_dispatcher, are per-device) */
    int ringBufferCapacity;     /* default 256 */
    int maxConnections;         /* default 16 */
    int requestQueueCapacity;   /* default 64 - bounded FIFO between the lws thread and the
                                    worker thread; full queue -> SERVER_BUSY, never blocks the
                                    lws thread (see data/control_dispatcher_worker.h) */
} ControlDispatcherConfig;

typedef enum {
    CONTROL_REQ_START_REPORTING,
    CONTROL_REQ_STOP_REPORTING
} ControlRequestType;

/*
 * One fully-parsed inbound command, queued from the lws thread to the
 * worker thread. Every string field is an owned copy (the raw JSON buffer
 * it was parsed from is freed immediately after parsing, in the lws
 * callback) - see data/control_dispatcher_json_parser.h.
 */
typedef struct {
    char* requestId;             /* owned, never NULL once parsed successfully - echoed back
                                     verbatim in every response derived from this request */
    ControlRequestType type;

    /* CONTROL_REQ_START_REPORTING fields - unused/zero for STOP_REPORTING */
    char* host;                  /* owned */
    int mmsPort;
    char* iedName;                /* owned, may be NULL (auto-detect) */
    char* interfaceId;            /* owned */
    char* sclFilePath;             /* owned, may be NULL */
    char* acseAuthPassword;        /* owned, may be NULL */
    AccessMode accessMode;

    /* CONTROL_REQ_STOP_REPORTING fields - unused/zero for START_REPORTING */
    uint64_t deviceId;
} ControlRequest;

/*
 * Opaque forward declarations only - full struct defs live entirely inside
 * data/control_dispatcher_ring_buffer.c / data/control_dispatcher_ws_server.c /
 * data/control_dispatcher_request_queue.c / data/control_dispatcher_worker.c,
 * same convention as ipc_dispatcher_types.h (kept as bare tags so this
 * header never pulls in hal_thread.h/libwebsockets.h).
 */
struct sControlDispatcherRingBuffer;
struct sControlDispatcherWsServer;
struct sControlDispatcherRequestQueue;
struct sControlDispatcherWorker;

struct sControlDispatcherHandle {
    ControlDispatcherConfig config;
    struct sControlDispatcherRingBuffer* ringBuffer;     /* owned, created in _create */
    struct sControlDispatcherRequestQueue* requestQueue; /* owned, created in _create */
    struct sControlDispatcherWsServer* wsServer;         /* owned, created/destroyed in _start/_stop -
                                                             NULL when not running */
    struct sControlDispatcherWorker* worker;             /* owned, created/destroyed in _start/_stop -
                                                             NULL when not running */
    DeviceManagerHandle deviceManager;                   /* borrowed - caller (main.c) owns its
                                                             lifetime, must outlive this handle */
    volatile bool running;
};
typedef struct sControlDispatcherHandle* ControlDispatcherHandle;

#endif /* CONTROL_DISPATCHER_TYPES_H_ */
