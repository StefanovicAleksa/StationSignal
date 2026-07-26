#include <stdlib.h>
#include <string.h>
#include "libwebsockets.h"
#include "hal_thread.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ws_server.h"

/* Per-connection session data (lws's per_session_data_size mechanism) - only
 * a read cursor into the shared ring buffer is needed. No pending-write
 * buffer: lws_write's own doc comment (lws-write.h) confirms partial sends
 * are buffered and retried autonomously by the library itself (an extra
 * WRITEABLE callback fires once the buffered remainder completes) - so this
 * callback only ever needs to issue at most one lws_write per WRITEABLE
 * turn and let lws handle the rest. */
typedef struct {
    uint64_t cursor;
} IpcDispatcherSession;

struct sIpcDispatcherWsServer {
    struct lws_context* context;
    IpcDispatcherRingBuffer ringBuffer; /* borrowed */
    struct lws_protocols protocols[2];  /* [1] is the LWS_PROTOCOL_LIST_TERM
                                            terminator - all-zero, already
                                            satisfied by calloc, never
                                            explicitly assigned */
    int maxConnections;
    int currentConnections;

    volatile bool stopRequested;
    volatile bool serviceExited;
    Thread serviceThread;
};

static int
ipcDispatcherCallback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
    (void) in;
    (void) len;

    struct sIpcDispatcherWsServer* server =
            (struct sIpcDispatcherWsServer*) lws_context_user(lws_get_context(wsi));
    IpcDispatcherSession* session = (IpcDispatcherSession*) user;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            if (server->currentConnections >= server->maxConnections) return -1; /* reject: over the cap */
            server->currentConnections++;
            /* Start-from-now: a newly connected client gets no backlog
             * replay in v1 - only messages pushed after this point. */
            if (session) session->cursor = IpcDispatcherRingBuffer_headSeq(server->ringBuffer);
            break;

        case LWS_CALLBACK_CLOSED:
            server->currentConnections--;
            break;

        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            /* Woken by either a producer's IpcDispatcherWsServer_wake (new
             * message pushed) or _stop (exit request, checked in the service
             * loop itself, not here). Cheap - a connection with nothing new
             * just no-ops on its own WRITEABLE turn below, so this never
             * busy-loops. */
            lws_callback_on_writable_all_protocol(server->context, &server->protocols[0]);
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (!session) break;

            uint64_t dropped = 0;
            char* json = IpcDispatcherRingBuffer_readNext(server->ringBuffer, &session->cursor, &dropped);
            /* dropped (lagging-client messages skipped) is not surfaced over
             * the wire in v1 - a future message `type` could report it. */
            if (json) {
                size_t payloadLen = strlen(json);
                unsigned char* buf = malloc(LWS_PRE + payloadLen);
                if (buf) {
                    memcpy(buf + LWS_PRE, json, payloadLen);
                    lws_write(wsi, buf + LWS_PRE, payloadLen, LWS_WRITE_TEXT);
                    free(buf);
                }
                free(json);

                /* More still buffered for this connection - keep draining
                 * without waiting for another external wake. */
                if (IpcDispatcherRingBuffer_headSeq(server->ringBuffer) != session->cursor) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;
        }

        default:
            break;
    }

    return 0;
}

static void*
serviceLoop(void* parameter) {
    struct sIpcDispatcherWsServer* server = (struct sIpcDispatcherWsServer*) parameter;

    while (!server->stopRequested) {
        /* 1000ms is a bounded safety-net timeout for lws_service's own
         * blocking wait, not a data-driving poll - real wakeups come from
         * lws_cancel_service (new message OR stop request). Transport-loop
         * plumbing, not GOOSE/MMS data reception, so this isn't a "no cyclic
         * polling" hard-rule violation - same class of narrow exception as
         * goose_subscriber's liveness thread, narrower still since it never
         * gates or delays delivery itself. */
        lws_service(server->context, 1000);
    }

    server->serviceExited = true;
    return NULL;
}

IpcDispatcherWsServer
IpcDispatcherWsServer_create(uint16_t port, int maxConnections,
        IpcDispatcherRingBuffer ringBuffer, IpcDispatcherError* outError) {
    if (!ringBuffer || maxConnections <= 0) {
        if (outError) *outError = IPC_DISPATCHER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    struct sIpcDispatcherWsServer* server = calloc(1, sizeof(struct sIpcDispatcherWsServer));
    if (!server) {
        if (outError) *outError = IPC_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    server->ringBuffer = ringBuffer;
    server->maxConnections = maxConnections;

    server->protocols[0].name = "ipc-dispatcher-v1";
    server->protocols[0].callback = ipcDispatcherCallback;
    server->protocols[0].per_session_data_size = sizeof(IpcDispatcherSession);
    /* protocols[1] left all-zero by calloc - the required LWS_PROTOCOL_LIST_TERM terminator */

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port;
    info.iface = "127.0.0.1"; /* internal-only: loopback bind, not caller-configurable to anything else */
    info.protocols = server->protocols;
    info.gid = (gid_t) -1;
    info.uid = (uid_t) -1;
    info.user = server; /* retrieved back via lws_context_user() in the callback */

    struct lws_context* context = lws_create_context(&info);
    if (!context) {
        free(server);
        if (outError) *outError = IPC_DISPATCHER_ERR_SOCKET_BIND_FAILED;
        return NULL;
    }

    server->context = context;

    if (outError) *outError = IPC_DISPATCHER_OK;
    return server;
}

IpcDispatcherError
IpcDispatcherWsServer_start(IpcDispatcherWsServer server) {
    if (!server) return IPC_DISPATCHER_ERR_INVALID_ARGUMENT;

    server->stopRequested = false;
    server->serviceExited = false;

    server->serviceThread = Thread_create(serviceLoop, server, false);
    if (!server->serviceThread) return IPC_DISPATCHER_ERR_THREAD_CREATE_FAILED;

    Thread_start(server->serviceThread);
    return IPC_DISPATCHER_OK;
}

void
IpcDispatcherWsServer_wake(IpcDispatcherWsServer server) {
    if (!server || !server->context) return;
    lws_cancel_service(server->context);
}

void
IpcDispatcherWsServer_stop(IpcDispatcherWsServer server) {
    if (!server || server->stopRequested) return;

    server->stopRequested = true;
    if (server->context) lws_cancel_service(server->context); /* prompt exit even if idle */

    if (server->serviceThread) {
        while (!server->serviceExited) Thread_sleep(20);
    }
}

void
IpcDispatcherWsServer_destroy(IpcDispatcherWsServer server) {
    if (!server) return;

    IpcDispatcherWsServer_stop(server);

    if (server->serviceThread) {
        Thread_destroy(server->serviceThread);
        server->serviceThread = NULL;
    }
    if (server->context) {
        lws_context_destroy(server->context);
        server->context = NULL;
    }
    free(server);
}
