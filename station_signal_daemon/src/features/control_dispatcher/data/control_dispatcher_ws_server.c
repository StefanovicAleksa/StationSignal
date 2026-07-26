#include <stdlib.h>
#include <string.h>
#include "libwebsockets.h"
#include "hal_thread.h"
#include "features/control_dispatcher/data/control_dispatcher_ws_server.h"
#include "features/control_dispatcher/data/control_dispatcher_json_parser.h"
#include "features/control_dispatcher/data/control_dispatcher_json_writer.h"

/* Bounded per-connection accumulation buffer for one inbound text frame
 * (control commands are small - this is a safety cap against a runaway/
 * malicious sender, not a realistic legitimate message size). */
#define CONTROL_DISPATCHER_MAX_MESSAGE_SIZE 8192

typedef struct {
    uint64_t cursor;              /* read cursor into the shared ring buffer, same as every
                                      other dispatcher's own per-connection session */
    char accumBuf[CONTROL_DISPATCHER_MAX_MESSAGE_SIZE];
    size_t accumLen;
    bool overflowed;               /* this message already exceeded the cap - drop remaining
                                       fragments, emit one error at the final fragment */
} ControlDispatcherSession;

struct sControlDispatcherWsServer {
    struct lws_context* context;
    ControlDispatcherRingBuffer ringBuffer;     /* borrowed */
    ControlDispatcherRequestQueue requestQueue; /* borrowed */
    ControlDispatcherRequestQueuedCallback onRequestQueued;
    void* onRequestQueuedParam;
    struct lws_protocols protocols[2];   /* [1] is the LWS_PROTOCOL_LIST_TERM terminator -
                                             all-zero, already satisfied by calloc */
    int maxConnections;
    int currentConnections;

    volatile bool stopRequested;
    volatile bool serviceExited;
    Thread serviceThread;
};

static const char*
parseErrorToCode(ControlParseError err) {
    switch (err) {
        case CONTROL_PARSE_ERR_MALFORMED_JSON: return "MALFORMED_REQUEST";
        case CONTROL_PARSE_ERR_MISSING_REQUEST_ID: return "MALFORMED_REQUEST";
        case CONTROL_PARSE_ERR_UNKNOWN_ACTION: return "UNKNOWN_ACTION";
        case CONTROL_PARSE_ERR_INVALID_PARAMS: return "INVALID_ARGUMENT";
        default: return "MALFORMED_REQUEST";
    }
}

static const char*
parseErrorToMessage(ControlParseError err) {
    switch (err) {
        case CONTROL_PARSE_ERR_MALFORMED_JSON: return "invalid JSON";
        case CONTROL_PARSE_ERR_MISSING_REQUEST_ID: return "missing or empty \"requestId\"";
        case CONTROL_PARSE_ERR_UNKNOWN_ACTION: return "missing or unrecognized \"action\"";
        case CONTROL_PARSE_ERR_INVALID_PARAMS: return "missing/invalid \"params\" for the given action";
        default: return "malformed request";
    }
}

static void
pushAndTriggerWritable(struct sControlDispatcherWsServer* server, char* json) {
    if (!json) return;
    ControlDispatcherRingBuffer_push(server->ringBuffer, json);
    lws_callback_on_writable_all_protocol(server->context, &server->protocols[0]);
}

static void
handleCompleteMessage(struct sControlDispatcherWsServer* server, const char* buf, size_t len) {
    char* recoveredRequestId = NULL;
    char* recoveredAction = NULL;
    ControlRequest* request = NULL;

    ControlParseError parseErr =
            ControlDispatcherJsonParser_parse(buf, len, &recoveredRequestId, &recoveredAction, &request);

    if (parseErr != CONTROL_PARSE_OK) {
        char* json = ControlDispatcherJsonWriter_writeError(recoveredRequestId, recoveredAction,
                parseErrorToCode(parseErr), parseErrorToMessage(parseErr), NULL, NULL);
        free(recoveredRequestId);
        free(recoveredAction);
        pushAndTriggerWritable(server, json);
        return;
    }

    if (!ControlDispatcherRequestQueue_push(server->requestQueue, request)) {
        const char* action = (request->type == CONTROL_REQ_START_REPORTING) ? "START_REPORTING" : "STOP_REPORTING";
        char* json = ControlDispatcherJsonWriter_writeError(request->requestId, action, "SERVER_BUSY",
                "request queue is full - try again shortly", NULL, NULL);
        ControlDispatcherRequest_destroy(request);
        pushAndTriggerWritable(server, json);
        return;
    }

    if (server->onRequestQueued) server->onRequestQueued(server->onRequestQueuedParam);
}

static int
controlDispatcherCallback(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
    struct sControlDispatcherWsServer* server =
            (struct sControlDispatcherWsServer*) lws_context_user(lws_get_context(wsi));
    ControlDispatcherSession* session = (ControlDispatcherSession*) user;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            if (server->currentConnections >= server->maxConnections) return -1; /* reject: over the cap */
            server->currentConnections++;
            /* Start-from-now: a newly connected client gets no backlog
             * replay - only responses pushed after this point. */
            if (session) {
                session->cursor = ControlDispatcherRingBuffer_headSeq(server->ringBuffer);
                session->accumLen = 0;
                session->overflowed = false;
            }
            break;

        case LWS_CALLBACK_CLOSED:
            server->currentConnections--;
            break;

        case LWS_CALLBACK_RECEIVE:
            if (!session) break;

            if (session->accumLen + len > sizeof(session->accumBuf)) {
                session->overflowed = true;
            } else {
                memcpy(session->accumBuf + session->accumLen, in, len);
                session->accumLen += len;
            }

            if (lws_is_final_fragment(wsi)) {
                if (session->overflowed) {
                    char* json = ControlDispatcherJsonWriter_writeError(NULL, NULL, "MALFORMED_REQUEST",
                            "request exceeds maximum message size", NULL, NULL);
                    pushAndTriggerWritable(server, json);
                } else {
                    handleCompleteMessage(server, session->accumBuf, session->accumLen);
                }
                session->accumLen = 0;
                session->overflowed = false;
            }
            break;

        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            /* Woken by either the worker thread's ControlDispatcherWsServer_wake
             * (new response pushed) or _stop (exit request, checked in the
             * service loop itself, not here). */
            lws_callback_on_writable_all_protocol(server->context, &server->protocols[0]);
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (!session) break;

            uint64_t dropped = 0;
            char* json = ControlDispatcherRingBuffer_readNext(server->ringBuffer, &session->cursor, &dropped);
            /* dropped (lagging-client messages skipped) is not surfaced over
             * the wire in v1. */
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
                if (ControlDispatcherRingBuffer_headSeq(server->ringBuffer) != session->cursor) {
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
    struct sControlDispatcherWsServer* server = (struct sControlDispatcherWsServer*) parameter;

    while (!server->stopRequested) {
        /* 1000ms is a bounded safety-net timeout for lws_service's own
         * blocking wait, not a data-driving poll - real wakeups come from
         * lws_cancel_service (new message OR stop request). Same narrow
         * "no cyclic polling" exception class as ipc_dispatcher_ws_server.c's
         * own identical loop. */
        lws_service(server->context, 1000);
    }

    server->serviceExited = true;
    return NULL;
}

ControlDispatcherWsServer
ControlDispatcherWsServer_create(uint16_t port, int maxConnections, ControlDispatcherRingBuffer ringBuffer,
        ControlDispatcherRequestQueue requestQueue, ControlDispatcherRequestQueuedCallback onRequestQueued,
        void* onRequestQueuedParam, ControlDispatcherError* outError) {
    if (!ringBuffer || !requestQueue || maxConnections <= 0) {
        if (outError) *outError = CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    struct sControlDispatcherWsServer* server = calloc(1, sizeof(struct sControlDispatcherWsServer));
    if (!server) {
        if (outError) *outError = CONTROL_DISPATCHER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    server->ringBuffer = ringBuffer;
    server->requestQueue = requestQueue;
    server->onRequestQueued = onRequestQueued;
    server->onRequestQueuedParam = onRequestQueuedParam;
    server->maxConnections = maxConnections;

    server->protocols[0].name = "control-dispatcher-v1";
    server->protocols[0].callback = controlDispatcherCallback;
    server->protocols[0].per_session_data_size = sizeof(ControlDispatcherSession);
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
        if (outError) *outError = CONTROL_DISPATCHER_ERR_SOCKET_BIND_FAILED;
        return NULL;
    }

    server->context = context;

    if (outError) *outError = CONTROL_DISPATCHER_OK;
    return server;
}

void
ControlDispatcherWsServer_setRequestQueuedCallback(ControlDispatcherWsServer server,
        ControlDispatcherRequestQueuedCallback callback, void* param) {
    if (!server) return;
    server->onRequestQueued = callback;
    server->onRequestQueuedParam = param;
}

ControlDispatcherError
ControlDispatcherWsServer_start(ControlDispatcherWsServer server) {
    if (!server) return CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT;

    server->stopRequested = false;
    server->serviceExited = false;

    server->serviceThread = Thread_create(serviceLoop, server, false);
    if (!server->serviceThread) return CONTROL_DISPATCHER_ERR_THREAD_CREATE_FAILED;

    Thread_start(server->serviceThread);
    return CONTROL_DISPATCHER_OK;
}

void
ControlDispatcherWsServer_wake(ControlDispatcherWsServer server) {
    if (!server || !server->context) return;
    lws_cancel_service(server->context);
}

void
ControlDispatcherWsServer_stop(ControlDispatcherWsServer server) {
    if (!server || server->stopRequested) return;

    server->stopRequested = true;
    if (server->context) lws_cancel_service(server->context); /* prompt exit even if idle */

    if (server->serviceThread) {
        while (!server->serviceExited) Thread_sleep(20);
    }
}

void
ControlDispatcherWsServer_destroy(ControlDispatcherWsServer server) {
    if (!server) return;

    ControlDispatcherWsServer_stop(server);

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
