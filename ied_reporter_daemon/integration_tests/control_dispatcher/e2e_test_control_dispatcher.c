#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/control_dispatcher/service/control_dispatcher_api.h"
#include "device_manager/service/device_manager_api.h"
#include "scan_orchestration/service/scan_orchestration_api.h"
#include "hal_thread.h"
#include "cJSON.h"
#include "sim_types.h"

/*
 * E2E test: real bind of control_dispatcher on a test port, a hand-rolled
 * RFC6455 client that both SENDS command frames (the first E2E test in this
 * repo to send a client->server frame - every other dispatcher is push-only,
 * see domain/control_dispatcher_types.h's own top comment) and receives
 * responses, driven against a real DeviceManagerHandle wired to a real
 * "Reporter1" IED simulator instance (sim_types.h/sim_server.c, see
 * integration_tests/ied_simulator/).
 *
 * Covers: malformed JSON and unknown-action error envelopes (no network I/O
 * involved - these fail entirely on the lws thread before ever reaching
 * device_manager), then a real START_REPORTING round trip (asserting the
 * returned wsPort streams real report/GOOSE JSON, proving the whole chain:
 * control message -> device_manager -> orchestration -> per-device
 * ipc_dispatcher) and a real STOP_REPORTING round trip.
 *
 * REQUIRES CAP_NET_RAW for the START_REPORTING case (inherited transitively
 * from the GOOSE subscriber step every DeviceManager_startReporting call
 * reaches via orchestration) - run with sudo:
 *   sudo make run
 * The malformed-JSON/unknown-action cases need no such privilege and no IED
 * simulator at all.
 */

#define TEST_PORT 18867
#define TEST_MMS_PORT 10503
#define TEST_INTERFACE "lo"
#define IED_NAME "Reporter1"
#define DM_WS_PORT_RANGE_START 19600
#define DM_WS_PORT_RANGE_END 19699
#define EXPECTED_GOCB_REF "Reporter1LD1/LLN0$GO$gcbInd"
#define TEST_MMS_PORT_AUTH_WRONG 10504
#define TEST_MMS_PORT_AUTH_CORRECT 10505
#define TEST_PASSWORD "secret123"

/* ---- hand-rolled minimal RFC6455 client: connect+upgrade, send a MASKED
 * client->server text frame, and read one unmasked server->client text
 * frame (same reader shape as every other E2E test in this repo). ---- */

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

/* Client->server frames MUST be masked per RFC6455 - the one thing no
 * existing E2E test in this repo needed until now (every other dispatcher
 * is push-only, server->client only). */
static bool
sendTextFrame(int sock, const char* payload) {
    size_t payloadLen = strlen(payload);
    unsigned char maskKey[4] = { 0x12, 0x34, 0x56, 0x78 };

    unsigned char header[10];
    size_t headerLen = 0;
    header[headerLen++] = 0x81; /* FIN + text opcode */

    if (payloadLen <= 125) {
        header[headerLen++] = (unsigned char) (0x80 | payloadLen); /* MASK bit + length */
    } else if (payloadLen <= 0xFFFF) {
        header[headerLen++] = 0x80 | 126;
        header[headerLen++] = (unsigned char) ((payloadLen >> 8) & 0xFF);
        header[headerLen++] = (unsigned char) (payloadLen & 0xFF);
    } else {
        return false; /* not needed for this test's small command payloads */
    }

    unsigned char* frame = malloc(headerLen + 4 + payloadLen);
    if (!frame) return false;
    memcpy(frame, header, headerLen);
    memcpy(frame + headerLen, maskKey, 4);
    for (size_t i = 0; i < payloadLen; i++) {
        frame[headerLen + 4 + i] = (unsigned char) (payload[i] ^ maskKey[i % 4]);
    }

    bool ok = send(sock, frame, headerLen + 4 + payloadLen, 0) == (ssize_t) (headerLen + 4 + payloadLen);
    free(frame);
    return ok;
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

/* Reads frames until one whose "requestId" matches is found (the broadcast
 * ring buffer means any other request's response could theoretically
 * interleave in a busier test - here there's only ever one client/request
 * in flight at a time, but this stays robust regardless). */
static cJSON*
waitForResponse(int sock, const char* expectedRequestId) {
    for (int i = 0; i < 10; i++) {
        if (!waitReadable(sock, 3000)) return NULL;
        char* json = readOneTextFrame(sock);
        if (!json) return NULL;

        cJSON* parsed = cJSON_Parse(json);
        free(json);
        if (!parsed) continue;

        cJSON* requestId = cJSON_GetObjectItemCaseSensitive(parsed, "requestId");
        if (expectedRequestId == NULL) {
            if (cJSON_IsNull(requestId)) return parsed;
        } else if (requestId && cJSON_IsString(requestId)
                && strcmp(requestId->valuestring, expectedRequestId) == 0) {
            return parsed;
        }
        cJSON_Delete(parsed);
    }
    return NULL;
}

static DeviceManagerHandle fixtureDeviceManager;
static ScanOrchestrationHandle fixtureScanOrchestration;
static ControlDispatcherHandle fixtureControlDispatcher;
static SimServer fixtureSim;

void
setUp(void) {
    DeviceManagerConfig dmConfig;
    DeviceManagerConfig_defaults(&dmConfig);
    dmConfig.wsPortRangeStart = DM_WS_PORT_RANGE_START;
    dmConfig.wsPortRangeEnd = DM_WS_PORT_RANGE_END;
    fixtureDeviceManager = DeviceManager_create(&dmConfig, NULL);

    fixtureScanOrchestration = ScanOrchestration_create(NULL, NULL);

    ControlDispatcherConfig cdConfig;
    ControlDispatcherConfig_defaults(&cdConfig);
    cdConfig.port = TEST_PORT;
    fixtureControlDispatcher = ControlDispatcher_create(&cdConfig, fixtureDeviceManager, fixtureScanOrchestration,
            NULL);
    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_OK, ControlDispatcher_start(fixtureControlDispatcher));

    fixtureSim = NULL;
}

void
tearDown(void) {
    if (fixtureControlDispatcher) {
        ControlDispatcher_destroy(fixtureControlDispatcher);
        fixtureControlDispatcher = NULL;
    }
    if (fixtureScanOrchestration) {
        ScanOrchestration_destroy(fixtureScanOrchestration);
        fixtureScanOrchestration = NULL;
    }
    if (fixtureDeviceManager) {
        DeviceManager_destroy(fixtureDeviceManager);
        fixtureDeviceManager = NULL;
    }
    if (fixtureSim) {
        SimServer_stop(fixtureSim);
        SimServer_destroy(fixtureSim);
        fixtureSim = NULL;
    }
}

void
test_malformedJson_returnsMalformedRequestError(void) {
    int ws = connectAndUpgrade(TEST_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to control_dispatcher failed");

    TEST_ASSERT_TRUE(sendTextFrame(ws, "{not valid json"));

    cJSON* response = waitForResponse(ws, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(response, "expected an error response for malformed JSON");

    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "success")));
    cJSON* error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("MALFORMED_REQUEST", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);

    cJSON_Delete(response);
    close(ws);
}

void
test_unknownAction_returnsUnknownActionError(void) {
    int ws = connectAndUpgrade(TEST_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to control_dispatcher failed");

    TEST_ASSERT_TRUE(sendTextFrame(ws,
            "{\"requestId\":\"req-unknown\",\"action\":\"DO_A_BARREL_ROLL\",\"params\":{}}"));

    cJSON* response = waitForResponse(ws, "req-unknown");
    TEST_ASSERT_NOT_NULL_MESSAGE(response, "expected an error response for an unknown action");

    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "success")));
    cJSON* error = cJSON_GetObjectItemCaseSensitive(response, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("UNKNOWN_ACTION", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);

    cJSON_Delete(response);
    close(ws);
}

void
test_startReporting_thenStopReporting_realRoundTrip(void) {
    fixtureSim = SimServer_create();
    SimServer_setFilestoreBasepath(fixtureSim, "fixtures/served_files/");
    SimServer_start(fixtureSim, TEST_MMS_PORT);
    Thread_sleep(200);

    int ws = connectAndUpgrade(TEST_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to control_dispatcher failed");

    char startCommand[512];
    snprintf(startCommand, sizeof(startCommand),
            "{\"requestId\":\"req-start\",\"action\":\"START_REPORTING\","
            "\"params\":{\"host\":\"127.0.0.1\",\"mmsPort\":%d,\"iedName\":\"%s\",\"interfaceId\":\"%s\"}}",
            TEST_MMS_PORT, IED_NAME, TEST_INTERFACE);
    TEST_ASSERT_TRUE(sendTextFrame(ws, startCommand));

    cJSON* startResponse = waitForResponse(ws, "req-start");
    TEST_ASSERT_NOT_NULL_MESSAGE(startResponse, "expected a START_REPORTING response");
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(startResponse, "success")),
            "START_REPORTING failed - if error.stage is GOOSE_SUBSCRIBER_START, this test needs "
            "CAP_NET_RAW (run with sudo)");

    cJSON* result = cJSON_GetObjectItemCaseSensitive(startResponse, "result");
    TEST_ASSERT_NOT_NULL(result);
    double deviceId = cJSON_GetObjectItemCaseSensitive(result, "deviceId")->valuedouble;
    int wsPort = (int) cJSON_GetObjectItemCaseSensitive(result, "wsPort")->valuedouble;
    cJSON_Delete(startResponse);

    /* Connect to the per-device ipc_dispatcher port and confirm real GOOSE
     * JSON flows - proves the whole chain: control message -> device_manager
     * -> orchestration -> per-device ipc_dispatcher. */
    int reportWs = connectAndUpgrade(wsPort);
    TEST_ASSERT_TRUE_MESSAGE(reportWs >= 0, "websocket handshake to the device's own ipc_dispatcher failed");

    /* goose_subscriber bootstrap-suppresses the very first frame it ever
     * observes for a target (cached==NULL -> silently seed, never forward -
     * GOOSE's equivalent of MMS's GI suppression, see CLAUDE.md). This
     * simulator's GSE MinTime/MaxTime is 10ms/5000ms (sim_server.c), so
     * nothing guarantees a natural heartbeat lands between goose_subscriber
     * starting reception (just now, inside START_REPORTING) and this flip -
     * if none does, THIS flip's own resulting frame becomes the "first frame
     * ever" and would be silently suppressed, timing out sawGoose below with
     * no further change ever occurring to reveal it.
     * integration_tests/goose_subscriber/'s own dedicated E2E test avoids
     * this by waiting for a VALID liveness status before flipping; this test
     * has no per-device liveness signal to wait on (only the outward-facing
     * ipc_dispatcher websocket), so it flips twice instead: the first flip is
     * a throwaway seed (whether or not it's actually the bootstrap frame),
     * the second is what's actually asserted on. */
    SimServer_setIndication(fixtureSim, true);
    Thread_sleep(500);
    SimServer_setIndication(fixtureSim, false);

    bool sawGoose = false;
    for (int i = 0; i < 15 && !sawGoose; i++) {
        if (!waitReadable(reportWs, 2000)) break;
        char* json = readOneTextFrame(reportWs);
        if (!json) break;
        cJSON* parsed = cJSON_Parse(json);
        free(json);
        if (!parsed) continue;
        cJSON* type = cJSON_GetObjectItem(parsed, "type");
        cJSON* source = cJSON_GetObjectItem(parsed, "source");
        if (type && type->valuestring && strcmp(type->valuestring, "GOOSE") == 0 && source) {
            cJSON* goCbRef = cJSON_GetObjectItem(source, "goCbRef");
            if (goCbRef && goCbRef->valuestring && strcmp(goCbRef->valuestring, EXPECTED_GOCB_REF) == 0) {
                sawGoose = true;
            }
        }
        cJSON_Delete(parsed);
    }
    TEST_ASSERT_TRUE_MESSAGE(sawGoose, "expected a real GOOSE JSON message from the started device");
    close(reportWs);

    char stopCommand[128];
    snprintf(stopCommand, sizeof(stopCommand),
            "{\"requestId\":\"req-stop\",\"action\":\"STOP_REPORTING\",\"params\":{\"deviceId\":%.0f}}", deviceId);
    TEST_ASSERT_TRUE(sendTextFrame(ws, stopCommand));

    cJSON* stopResponse = waitForResponse(ws, "req-stop");
    TEST_ASSERT_NOT_NULL_MESSAGE(stopResponse, "expected a STOP_REPORTING response");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(stopResponse, "success")));
    cJSON_Delete(stopResponse);

    /* The per-device port must be torn down after stop. */
    int afterStop = connectAndUpgrade(wsPort);
    TEST_ASSERT_TRUE_MESSAGE(afterStop < 0, "device's ipc_dispatcher port must be torn down after stop");

    close(ws);
}

/*
 * Proves the full control-channel round trip for a password-protected device
 * with NO password supplied - the standard technician flow (host/interfaceId
 * only, no sclFilePath, so scl_bootstrap runs its normal network-discovery
 * SCL fetch) hitting a device that requires ACSE auth. This exact failure
 * mode already worked correctly at the scl_bootstrap/orchestration layer
 * (proven by scl_bootstrap's and orchestration's own E2E suites) but was
 * never exercised at the full control-channel JSON level before - this is
 * the gap the API's AUTH_REQUIRED classification (ied_reporter_api's
 * errors.go) is built to detect from exactly this response shape.
 *
 * No CAP_NET_RAW needed - bootstrap fails and the whole pipeline is torn
 * down before goose_subscriber's stage is ever reached.
 */
void
test_startReporting_missingPassword_returnsAccessDeniedAtBootstrapStage(void) {
    fixtureSim = SimServer_create();
    SimServer_setFilestoreBasepath(fixtureSim, "fixtures/served_files/");
    SimServer_requireAuthentication(fixtureSim, TEST_PASSWORD);
    SimServer_start(fixtureSim, TEST_MMS_PORT_AUTH_WRONG);
    Thread_sleep(200);

    int ws = connectAndUpgrade(TEST_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to control_dispatcher failed");

    char startCommand[512];
    snprintf(startCommand, sizeof(startCommand),
            "{\"requestId\":\"req-start-noauth\",\"action\":\"START_REPORTING\","
            "\"params\":{\"host\":\"127.0.0.1\",\"mmsPort\":%d,\"iedName\":\"%s\",\"interfaceId\":\"%s\"}}",
            TEST_MMS_PORT_AUTH_WRONG, IED_NAME, TEST_INTERFACE);
    TEST_ASSERT_TRUE(sendTextFrame(ws, startCommand));

    cJSON* startResponse = waitForResponse(ws, "req-start-noauth");
    TEST_ASSERT_NOT_NULL_MESSAGE(startResponse, "expected a START_REPORTING response");
    TEST_ASSERT_FALSE_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(startResponse, "success")),
            "expected START_REPORTING to fail with no acseAuthPassword against a password-protected device");

    cJSON* error = cJSON_GetObjectItemCaseSensitive(startResponse, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("ORCHESTRATION_FAILED", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);
    TEST_ASSERT_EQUAL_STRING("SCL bootstrap", cJSON_GetObjectItemCaseSensitive(error, "stage")->valuestring);
    TEST_ASSERT_EQUAL_STRING("access denied (auth required/rejected)",
            cJSON_GetObjectItemCaseSensitive(error, "detail")->valuestring);

    cJSON_Delete(startResponse);
    close(ws);
}

/*
 * Companion happy path: the same password-protected device, this time with
 * the correct acseAuthPassword supplied - proves the full control-channel
 * round trip still succeeds end to end for a password-protected device.
 *
 * REQUIRES CAP_NET_RAW (same as test_startReporting_thenStopReporting_realRoundTrip -
 * a successful START_REPORTING always reaches goose_subscriber's stage) - run
 * with sudo.
 */
void
test_startReporting_correctPassword_succeeds(void) {
    fixtureSim = SimServer_create();
    SimServer_setFilestoreBasepath(fixtureSim, "fixtures/served_files/");
    SimServer_requireAuthentication(fixtureSim, TEST_PASSWORD);
    SimServer_start(fixtureSim, TEST_MMS_PORT_AUTH_CORRECT);
    Thread_sleep(200);

    int ws = connectAndUpgrade(TEST_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to control_dispatcher failed");

    char startCommand[512];
    snprintf(startCommand, sizeof(startCommand),
            "{\"requestId\":\"req-start-auth\",\"action\":\"START_REPORTING\","
            "\"params\":{\"host\":\"127.0.0.1\",\"mmsPort\":%d,\"iedName\":\"%s\",\"interfaceId\":\"%s\","
            "\"acseAuthPassword\":\"%s\"}}",
            TEST_MMS_PORT_AUTH_CORRECT, IED_NAME, TEST_INTERFACE, TEST_PASSWORD);
    TEST_ASSERT_TRUE(sendTextFrame(ws, startCommand));

    cJSON* startResponse = waitForResponse(ws, "req-start-auth");
    TEST_ASSERT_NOT_NULL_MESSAGE(startResponse, "expected a START_REPORTING response");
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(startResponse, "success")),
            "START_REPORTING with the correct acseAuthPassword failed - if error.stage is "
            "GOOSE_SUBSCRIBER_START, this test needs CAP_NET_RAW (run with sudo)");

    cJSON* result = cJSON_GetObjectItemCaseSensitive(startResponse, "result");
    TEST_ASSERT_NOT_NULL(result);
    double deviceId = cJSON_GetObjectItemCaseSensitive(result, "deviceId")->valuedouble;
    cJSON_Delete(startResponse);

    char stopCommand[128];
    snprintf(stopCommand, sizeof(stopCommand),
            "{\"requestId\":\"req-stop-auth\",\"action\":\"STOP_REPORTING\",\"params\":{\"deviceId\":%.0f}}",
            deviceId);
    TEST_ASSERT_TRUE(sendTextFrame(ws, stopCommand));

    cJSON* stopResponse = waitForResponse(ws, "req-stop-auth");
    TEST_ASSERT_NOT_NULL_MESSAGE(stopResponse, "expected a STOP_REPORTING response");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(stopResponse, "success")));
    cJSON_Delete(stopResponse);

    close(ws);
}

void
test_startScan_thenStopScan_realRoundTrip(void) {
    /* No sudo needed - a scan sweep is TCP-only (see CLAUDE.md's
     * ied_discovery/scan_orchestration Architecture bullets); "lo" is
     * expected to fail its background sweep (netmask too large) but that's
     * tolerated gracefully by the worker thread, not a synchronous failure
     * of START_SCAN itself - this proves the control-plane round trip, not
     * sweep success (same reasoning integration_tests/scan_orchestration/'s
     * own E2E test already documents). */
    int ws = connectAndUpgrade(TEST_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to control_dispatcher failed");

    TEST_ASSERT_TRUE(sendTextFrame(ws,
            "{\"requestId\":\"req-scan-start\",\"action\":\"START_SCAN\","
            "\"params\":{\"interfaceId\":\"lo\"}}"));

    cJSON* startResponse = waitForResponse(ws, "req-scan-start");
    TEST_ASSERT_NOT_NULL_MESSAGE(startResponse, "expected a START_SCAN response");
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(startResponse, "success")),
            "START_SCAN itself must succeed even though the lo sweep is expected to fail later");

    cJSON* result = cJSON_GetObjectItemCaseSensitive(startResponse, "result");
    TEST_ASSERT_NOT_NULL(result);
    double scanId = cJSON_GetObjectItemCaseSensitive(result, "scanId")->valuedouble;
    cJSON_Delete(startResponse);

    char stopCommand[128];
    snprintf(stopCommand, sizeof(stopCommand),
            "{\"requestId\":\"req-scan-stop\",\"action\":\"STOP_SCAN\",\"params\":{\"scanId\":%.0f}}", scanId);
    TEST_ASSERT_TRUE(sendTextFrame(ws, stopCommand));

    cJSON* stopResponse = waitForResponse(ws, "req-scan-stop");
    TEST_ASSERT_NOT_NULL_MESSAGE(stopResponse, "expected a STOP_SCAN response");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(stopResponse, "success")));
    cJSON_Delete(stopResponse);

    close(ws);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_malformedJson_returnsMalformedRequestError);
    RUN_TEST(test_unknownAction_returnsUnknownActionError);
    RUN_TEST(test_startReporting_thenStopReporting_realRoundTrip);
    RUN_TEST(test_startReporting_missingPassword_returnsAccessDeniedAtBootstrapStage);
    RUN_TEST(test_startReporting_correctPassword_succeeds);
    RUN_TEST(test_startScan_thenStopScan_realRoundTrip);

    return UNITY_END();
}
