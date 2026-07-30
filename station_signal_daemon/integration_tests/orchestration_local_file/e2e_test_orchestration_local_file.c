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
 * End-to-end verification, entirely local (no live vendor device needed), of
 * the fix for a real-world failure found running this daemon against a live
 * ABB REC650 exposed via IED Scout: with Orchestration_runFromLocalFile
 * (used because that device's MMS server doesn't implement file services, so
 * scl_bootstrap can't fetch its SCL), mms_report_client connected fine but
 * every single RCB's getRCBValues call failed with
 * IED_ERROR_OBJECT_DOES_NOT_EXIST (22).
 *
 * A live directory dump of the real device (tools/smoke_tests/
 * mms_directory_dump_smoke_test.c) showed why: the SCD declares
 * <ReportControl name="rcb_A" ...><RptEnabled max="5"></ReportControl>, but
 * the live server only ever answers on the wire to "rcb_A01".."rcb_A05" -
 * never the bare "rcb_A" name. ied_model used to ignore <RptEnabled max>
 * entirely and always build the bare-name object reference; it's now fixed
 * (data/ied_model_scl_loader.c's resolveRcbRuntimeName) to append a
 * zero-padded 2-digit "01" instance suffix whenever max > 1.
 *
 * This test proves the fix works, fully under our control, with no vendor
 * device involved: integration_tests/ied_simulator/src/sim_server.c's
 * "Reporter1" IED has a second RCB built to mimic exactly this real-world
 * shape - live wire object "rcbMulti01" (see that file's own comment) - and
 * this test's own local fixture (fixtures/reporter1_rcb_instance_mismatch.cid)
 * declares it the same way the real SCD does: name="rcbMulti",
 * <RptEnabled max="5">. Loading that fixture via Orchestration_runFromLocalFile
 * against our own simulator now enables rcbMulti successfully (as
 * "rcbMulti01") and delivers a real report for it, side by side with
 * brcbMain (a normal, max="1" RCB, proving the fix doesn't regress the
 * no-suffix case).
 *
 * Also confirms Orchestration_runFromLocalFile's own host/port wiring is
 * correct while at it: the model is loaded from a local file, but the live
 * MMS connection and the resulting report both target the simulator's real
 * host/port, never anything read out of the loaded model itself.
 *
 * REQUIRES CAP_NET_RAW (raw AF_PACKET socket) - inherited transitively from
 * the GOOSE subscriber step orchestration always starts, same requirement as
 * goose_subscriber's/orchestration's own E2E tests - run with sudo:
 *   sudo make run
 */

#define TEST_PORT 10501 /* distinct from every other E2E test's port (10203/10204/10301/10399/10401) */
#define TEST_WS_PORT 18791
#define TEST_INTERFACE "lo"
#define IED_NAME "Reporter1"
#define FIXTURE_PATH "fixtures/reporter1_rcb_instance_mismatch.cid"
#define EXPECTED_BRCB_REF "Reporter1LD1/LLN0.BR.brcbMain"
#define EXPECTED_RCB_MULTI_REF "Reporter1LD1/LLN0.BR.rcbMulti01"
#define POLL_INTERVAL_MS 100
#define POLL_MAX_ATTEMPTS 100 /* 100 * 100ms = 10s bound on each wait */

static volatile bool brcbMainEnabled;
static volatile bool rcbMultiEnabled;
static volatile bool gooseValid;

static void
onRcbStatus(void* userParam, const char* rcbReference, bool enabled, IedClientError lastError) {
    (void) userParam;
    (void) lastError;
    if (!rcbReference || !enabled) return;

    if (strcmp(rcbReference, EXPECTED_BRCB_REF) == 0) {
        brcbMainEnabled = true;
    } else if (strcmp(rcbReference, EXPECTED_RCB_MULTI_REF) == 0) {
        rcbMultiEnabled = true;
    }
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

/* ---- hand-rolled minimal websocket test client (same approach as
 * integration_tests/orchestration/e2e_test_orchestration.c and
 * integration_tests/ipc_dispatcher/e2e_test_ipc_dispatcher.c - deliberately
 * NOT libwebsockets client mode) ---- */

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
    brcbMainEnabled = false;
    rcbMultiEnabled = false;
    gooseValid = false;
}

void
tearDown(void) {}

void
test_localFile_enablesRptEnabledMaxInstanceRcb_usingSuffixedReference(void) {
    SimServer sim = SimServer_create();
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

    OrchestrationErrorDetail detail;
    bool mmsAvailable = false;
    bool gooseAvailable = false;
    OrchestrationError runError = Orchestration_runFromLocalFile(handle, FIXTURE_PATH, "127.0.0.1", TEST_PORT,
            IED_NAME, TEST_INTERFACE, IED_MODEL_ACCESS_REPORT_ONLY, &detail, &mmsAvailable, &gooseAvailable);
    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_OK, runError,
            "Orchestration_runFromLocalFile failed - if stage==GOOSE_SUBSCRIBER_START, this test needs "
            "CAP_NET_RAW (run with sudo)");
    TEST_ASSERT_TRUE_MESSAGE(mmsAvailable, "fixture declares <ReportControl> - mmsAvailable should be true");
    TEST_ASSERT_TRUE_MESSAGE(gooseAvailable, "fixture declares <GSEControl> - gooseAvailable should be true");

    int ws = connectAndUpgrade(TEST_WS_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to orchestration's own ipc_dispatcher failed");

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&brcbMainEnabled),
            "expected the normal, RptEnabled max=1 RCB (brcbMain) to enable normally - this must keep working, "
            "proving the RptEnabled max>1 fix doesn't regress the no-suffix case");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbMultiEnabled),
            "expected rcbMulti (RptEnabled max=5) to enable using the suffixed reference "
            "'Reporter1LD1/LLN0.BR.rcbMulti01' - the only object the simulator actually exposes for it; "
            "if this never fires, ied_model is still building the unsuffixed reference");

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&gooseValid),
            "expected the frame adapter to observe a VALID GOOSE feed within the timeout "
            "(confirms orchestration itself started cleanly, unrelated to the RCB mismatch under test)");

    SimServer_setIndication(sim, true);

    bool sawBrcbMainReport = false;
    bool sawRcbMultiReport = false;

    for (int i = 0; i < 10 && !(sawBrcbMainReport && sawRcbMultiReport); i++) {
        if (!waitReadable(ws, 3000)) break;
        char* json = readOneTextFrame(ws);
        if (!json) break;

        cJSON* parsed = cJSON_Parse(json);
        free(json);
        if (!parsed) continue;

        cJSON* type = cJSON_GetObjectItem(parsed, "type");
        cJSON* source = cJSON_GetObjectItem(parsed, "source");
        if (type && source && type->valuestring && strcmp(type->valuestring, "MMS_REPORT") == 0) {
            cJSON* rcbRef = cJSON_GetObjectItem(source, "rcbReference");
            if (rcbRef && rcbRef->valuestring) {
                if (strcmp(rcbRef->valuestring, EXPECTED_BRCB_REF) == 0) sawBrcbMainReport = true;
                if (strcmp(rcbRef->valuestring, EXPECTED_RCB_MULTI_REF) == 0) sawRcbMultiReport = true;
            }
        }

        cJSON_Delete(parsed);
    }

    TEST_ASSERT_TRUE_MESSAGE(sawBrcbMainReport,
            "expected a real MMS_REPORT for brcbMain after flipping GGIO1.Ind1.stVal - this also confirms "
            "Orchestration_runFromLocalFile's live connection genuinely targets the simulator's host/port, "
            "not anything read out of the locally-loaded model");
    TEST_ASSERT_TRUE_MESSAGE(sawRcbMultiReport,
            "expected a real MMS_REPORT for rcbMulti too, attributed to the suffixed reference "
            "'Reporter1LD1/LLN0.BR.rcbMulti01' - proving the fix produces a reference the live server "
            "actually accepts, not just one that happens to enable without error");

    close(ws);
    Orchestration_destroy(handle);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_localFile_enablesRptEnabledMaxInstanceRcb_usingSuffixedReference);

    return UNITY_END();
}
