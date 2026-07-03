#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "orchestration/service/orchestration_api.h"
#include "cJSON.h"
#include "hal_thread.h"
#include "sim_types.h"

/*
 * End-to-end test of the full orchestration sequence: ipc_dispatcher (bind +
 * start) -> scl_bootstrap -> ied_model -> mms_report_client -> goose_subscriber,
 * against a single real "Reporter1" IED simulator (sim_types.h/sim_server.c,
 * see integration_tests/ied_simulator/, fully decoupled from src/) running in
 * this same process, over loopback.
 *
 * Unlike this test's previous incarnation, report/GOOSE DATA records are no
 * longer observed via Orchestration_setReportCallback/_setGooseRecordCallback
 * - those setters no longer exist, since orchestration now unconditionally
 * wires ipc_dispatcher onto mms_report_client/goose_subscriber internally
 * (see orchestration_api.h's own doc comment on why). Instead, this test
 * connects a hand-rolled minimal websocket client (raw TCP + HTTP-Upgrade
 * handshake + a small RFC6455 frame parser for the unmasked/unfragmented
 * text frames ipc_dispatcher ever sends - same approach as
 * integration_tests/ipc_dispatcher/e2e_test_ipc_dispatcher.c, deliberately
 * NOT libwebsockets client mode) to orchestration's own ipc_dispatcher port,
 * and asserts the real JSON envelopes for both the MMS report and the GOOSE
 * record arrive after flipping the simulator's indication. The
 * onRcbStatus/onGooseStatus diagnostic callbacks are untouched - those
 * remain directly caller-settable, unrelated to ipc_dispatcher.
 *
 * The simulator's MMS file services are pointed at fixtures/served_files/
 * (a local copy of integration_tests/scl_bootstrap's own fixture, same
 * per-test-dir fixture-duplication convention already used throughout this
 * repo) so scl_bootstrap can discover and fetch the IED's own SCL file over
 * MMS on "127.0.0.1", exactly as a real IED's file services would be used -
 * that fetched file describes the very same "Reporter1" model the simulator
 * is simultaneously serving live data/reports/GOOSE for.
 *
 * REQUIRES CAP_NET_RAW (raw AF_PACKET socket) - inherited transitively from
 * the GOOSE subscriber step, same requirement as goose_subscriber's own E2E
 * test - run with sudo:
 *   sudo make run
 */

#define TEST_PORT 10401 /* distinct from every other E2E test's port (10203/10204/10301/10399) */
#define TEST_WS_PORT 18790
#define TEST_INTERFACE "lo"
#define IED_NAME "Reporter1"
#define EXPECTED_RCB_REF "Reporter1LD1/LLN0.BR.brcbMain"
#define EXPECTED_GOCB_REF "Reporter1LD1/LLN0$GO$gcbInd"
#define POLL_INTERVAL_MS 100
#define POLL_MAX_ATTEMPTS 100 /* 100 * 100ms = 10s bound on each wait */

static volatile bool rcbEnabled;
static volatile bool gooseValid;

static void
onRcbStatus(void* userParam, const char* rcbReference, bool enabled, IedClientError lastError) {
    (void) userParam;
    (void) rcbReference;
    (void) lastError;
    if (enabled) rcbEnabled = true;
}

static void
onGooseStatus(void* userParam, const char* goCbRef, GooseSubscriberStatus status, GooseParseError lastParseError) {
    (void) userParam;
    (void) goCbRef;
    (void) lastParseError;
    if (status == GOOSE_SUBSCRIBER_STATUS_VALID) gooseValid = true;
}

static bool
waitUntil(volatile bool* flag) {
    for (int i = 0; i < POLL_MAX_ATTEMPTS; i++) {
        if (*flag) return true;
        Thread_sleep(POLL_INTERVAL_MS);
    }
    return false;
}

static LinkedList
makeHostList(const char* host) {
    LinkedList list = LinkedList_create();
    LinkedList_add(list, (void*) host);
    return list;
}

/* ---- hand-rolled minimal websocket test client (see file header comment) ---- */

static int
connectAndUpgrade(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    const char* request =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    if (send(sock, request, strlen(request), 0) < 0) {
        close(sock);
        return -1;
    }

    char buf[1024];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) {
            close(sock);
            return -1;
        }
        total += (size_t) n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    if (!strstr(buf, "101")) {
        close(sock);
        return -1;
    }

    return sock;
}

static bool
recvExact(int sock, uint8_t* out, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(sock, out + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t) r;
    }
    return true;
}

static char*
readOneTextFrame(int sock) {
    uint8_t header[2];
    if (!recvExact(sock, header, 2)) return NULL;

    int opcode = header[0] & 0x0F;
    if (opcode != 0x1) return NULL;

    uint64_t payloadLen = header[1] & 0x7F;
    if (payloadLen == 126) {
        uint8_t ext[2];
        if (!recvExact(sock, ext, 2)) return NULL;
        payloadLen = ((uint64_t) ext[0] << 8) | ext[1];
    } else if (payloadLen == 127) {
        uint8_t ext[8];
        if (!recvExact(sock, ext, 8)) return NULL;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i];
    }

    char* payload = malloc(payloadLen + 1);
    if (!payload) return NULL;
    if (payloadLen > 0 && !recvExact(sock, (uint8_t*) payload, payloadLen)) {
        free(payload);
        return NULL;
    }
    payload[payloadLen] = '\0';

    return payload;
}

static bool
waitReadable(int sock, int timeoutMs) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sock, &readSet);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(sock + 1, &readSet, NULL, NULL, &tv) > 0;
}

void
setUp(void) {
    rcbEnabled = false;
    gooseValid = false;
}

void
tearDown(void) {}

void
test_fullSequence_bootstrapModelReportAndGoose_endToEnd(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/served_files/");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);
    config.ipcDispatcherConfig.port = TEST_WS_PORT;

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(&config, &createError);
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, createError);

    Orchestration_setRcbStatusCallback(handle, onRcbStatus, NULL);
    Orchestration_setGooseStatusCallback(handle, onGooseStatus, NULL);

    LinkedList hosts = makeHostList("127.0.0.1");

    OrchestrationErrorDetail detail;
    OrchestrationError runError = Orchestration_run(handle, hosts, TEST_PORT, IED_NAME, TEST_INTERFACE,
            IED_MODEL_ACCESS_REPORT_ONLY, &detail);
    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_OK, runError,
            "Orchestration_run failed - if stage==GOOSE_SUBSCRIBER_START, this test needs CAP_NET_RAW (run with sudo)");

    LinkedList_destroyStatic(hosts);

    int ws = connectAndUpgrade(TEST_WS_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to orchestration's own ipc_dispatcher failed");

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbEnabled),
            "expected the reconnect supervisor thread to connect and enable brcbMain within the timeout");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&gooseValid),
            "expected the frame adapter to observe a VALID GOOSE feed within the timeout");

    SimServer_setIndication(sim, true);

    /* Report and GOOSE are independent producers - read frames until one of
     * each type has been seen (or a bounded number of frames elapses). */
    bool sawReport = false;
    bool sawGoose = false;
    char lastRcbReference[256] = "";
    char lastGoCbRef[256] = "";

    for (int i = 0; i < 10 && !(sawReport && sawGoose); i++) {
        if (!waitReadable(ws, 3000)) break;
        char* json = readOneTextFrame(ws);
        if (!json) break;

        cJSON* parsed = cJSON_Parse(json);
        free(json);
        if (!parsed) continue;

        cJSON* type = cJSON_GetObjectItem(parsed, "type");
        cJSON* source = cJSON_GetObjectItem(parsed, "source");
        if (type && source && strcmp(type->valuestring, "MMS_REPORT") == 0) {
            cJSON* rcbRef = cJSON_GetObjectItem(source, "rcbReference");
            if (rcbRef && rcbRef->valuestring) {
                strncpy(lastRcbReference, rcbRef->valuestring, sizeof(lastRcbReference) - 1);
                sawReport = true;
            }
        } else if (type && source && strcmp(type->valuestring, "GOOSE") == 0) {
            cJSON* goCbRef = cJSON_GetObjectItem(source, "goCbRef");
            if (goCbRef && goCbRef->valuestring) {
                strncpy(lastGoCbRef, goCbRef->valuestring, sizeof(lastGoCbRef) - 1);
                sawGoose = true;
            }
        }

        cJSON_Delete(parsed);
    }

    TEST_ASSERT_TRUE_MESSAGE(sawReport, "expected an MMS_REPORT JSON message after flipping GGIO1.Ind1.stVal");
    TEST_ASSERT_TRUE_MESSAGE(sawGoose, "expected a GOOSE JSON message after flipping GGIO1.Ind1.stVal");
    TEST_ASSERT_EQUAL_STRING(EXPECTED_RCB_REF, lastRcbReference);
    TEST_ASSERT_EQUAL_STRING(EXPECTED_GOCB_REF, lastGoCbRef);

    close(ws);
    Orchestration_destroy(handle);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_fullSequence_bootstrapModelReportAndGoose_endToEnd);

    return UNITY_END();
}
