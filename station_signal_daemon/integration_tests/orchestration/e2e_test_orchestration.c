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
#define ONLINE_DISCOVERY_TEST_PORT 10402
#define ONLINE_DISCOVERY_TEST_WS_PORT 18791
#define AUTH_MISMATCH_TEST_PORT 10403
#define AUTH_MISMATCH_TEST_WS_PORT 18792
#define AUTH_MISMATCH_PASSWORD_CORRECT "correct-password"
#define AUTH_MISMATCH_PASSWORD_WRONG "wrong-password"
#define MMS_ONLY_TEST_PORT 10404
#define MMS_ONLY_TEST_WS_PORT 18793
#define GOOSE_ONLY_TEST_PORT 10405
#define GOOSE_ONLY_TEST_WS_PORT 18794
#define NO_CAPABILITIES_TEST_PORT 10406
#define NO_CAPABILITIES_TEST_WS_PORT 18795
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
    bool mmsAvailable = false;
    bool gooseAvailable = false;
    OrchestrationError runError = Orchestration_run(handle, hosts, TEST_PORT, IED_NAME, TEST_INTERFACE,
            IED_MODEL_ACCESS_REPORT_ONLY, &detail, &mmsAvailable, &gooseAvailable);
    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_OK, runError,
            "Orchestration_run failed - if stage==GOOSE_SUBSCRIBER_START, this test needs CAP_NET_RAW (run with sudo)");
    TEST_ASSERT_TRUE_MESSAGE(mmsAvailable, "reporter1.cid declares brcbMain - mmsAvailable should be true");
    TEST_ASSERT_TRUE_MESSAGE(gooseAvailable, "reporter1.cid declares gcbInd - gooseAvailable should be true");

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
            /* This fixture also declares "gcbDup" (same ds1 dataset as
             * gcbInd, reproducing a real network's redundant-publisher
             * pattern) - cross-protocol/cross-GoCB duplicate suppression is
             * ipc_dispatcher's own concern now (see
             * IpcDispatcherUseCases_shouldForwardWithinProtocol), not
             * suppressed here, so gcbDup's own independent GOOSE message can
             * genuinely arrive on this same websocket. This test is
             * specifically about gcbInd, so gcbDup's own frames must be
             * ignored rather than racily overwriting lastGoCbRef. */
            cJSON* goCbRef = cJSON_GetObjectItem(source, "goCbRef");
            if (goCbRef && goCbRef->valuestring && strcmp(goCbRef->valuestring, EXPECTED_GOCB_REF) == 0) {
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

/*
 * Proves the online-discovery fallback end to end: Orchestration_run genuinely
 * fails at the bootstrap stage against a server with no SCL file at all (its
 * MMS file services point at a real, empty fixtures/no_scl_files/ directory -
 * mirroring scl_bootstrap's/ied_model_online_loader's own identical fixture,
 * proving this is the exact real-world SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND
 * precondition, not some other failure mode), then Orchestration_runFromOnlineDiscovery
 * against the SAME host/port succeeds and delivers real report/GOOSE JSON over
 * the same real ipc_dispatcher websocket the SCL-parsing path already proves.
 *
 * Expected RCB differs from the SCL-parsing test above: sim_server.c's
 * brcbMain/brcbDup/rcbMulti01 are all parented under LLN0, which itself has
 * zero FC=ST/MX data attributes of its own in this simulator (only GGIO1
 * does) - and, on the live server, EVERY RCB here has dataSetName=NULL until
 * a client explicitly assigns one (see this file's own header comment and
 * ied_model_online_loader's own bullet in CLAUDE.md). Online discovery
 * therefore reports datasetReference=NULL for all of them, and
 * mms_report_client's existing dynamic-dataset fallback
 * (getOrCreateDynamicDataset) can only synthesize a working dataset for an
 * RCB whose own parent LN actually has reportable attributes - true only for
 * urcbDyn (parented under GGIO1). brcbMain/brcbDup/rcbMulti01 (parented
 * under LLN0) fail to enable in this scenario, same as they always have for
 * any Dyn RCB with no reportable attributes on its own LN - not a defect in
 * online discovery itself. GOOSE is unaffected (gcbInd's "ds1" dataset is a
 * real, statically-configured, always-live GoCB attribute, discovered as-is).
 *
 * REQUIRES CAP_NET_RAW, same as the test above - run with sudo.
 */
void
test_onlineDiscoveryFallback_afterNoSclFileFound_endToEnd(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/no_scl_files/");
    SimServer_start(sim, ONLINE_DISCOVERY_TEST_PORT);
    Thread_sleep(200);

    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);
    config.ipcDispatcherConfig.port = ONLINE_DISCOVERY_TEST_WS_PORT;

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(&config, &createError);
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, createError);

    Orchestration_setRcbStatusCallback(handle, onRcbStatus, NULL);
    Orchestration_setGooseStatusCallback(handle, onGooseStatus, NULL);

    LinkedList hosts = makeHostList("127.0.0.1");

    OrchestrationErrorDetail detail;
    OrchestrationError runError = Orchestration_run(handle, hosts, ONLINE_DISCOVERY_TEST_PORT, IED_NAME,
            TEST_INTERFACE, IED_MODEL_ACCESS_REPORT_ONLY, &detail, NULL, NULL);
    LinkedList_destroyStatic(hosts);

    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_ERR_BOOTSTRAP_FAILED, runError,
            "expected Orchestration_run to genuinely fail bootstrap - if it didn't, this test's own "
            "fixture no longer reproduces the no-SCL-file precondition");
    TEST_ASSERT_EQUAL_MESSAGE(SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND, detail.lastCandidateStatus,
            "expected the exact real-world precondition ied_model_online_loader exists for");

    bool mmsAvailable = false;
    bool gooseAvailable = false;
    OrchestrationError fallbackError = Orchestration_runFromOnlineDiscovery(handle, "127.0.0.1",
            ONLINE_DISCOVERY_TEST_PORT, IED_NAME, TEST_INTERFACE, IED_MODEL_ACCESS_REPORT_ONLY, NULL, &detail,
            &mmsAvailable, &gooseAvailable);
    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_OK, fallbackError,
            "Orchestration_runFromOnlineDiscovery failed - if stage==GOOSE_SUBSCRIBER_START, this test "
            "needs CAP_NET_RAW (run with sudo)");
    TEST_ASSERT_TRUE_MESSAGE(mmsAvailable,
            "urcbDyn should still be discovered online even though brcbMain/brcbDup/rcbMulti01 fail to enable");
    TEST_ASSERT_TRUE_MESSAGE(gooseAvailable, "gcbInd should still be discovered online");

    int ws = connectAndUpgrade(ONLINE_DISCOVERY_TEST_WS_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to orchestration's own ipc_dispatcher failed");

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbEnabled),
            "expected urcbDyn (the only RCB whose parent LN has reportable attributes in this "
            "simulator) to enable via the dynamic-dataset fallback within the timeout");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&gooseValid),
            "expected the frame adapter to observe a VALID GOOSE feed within the timeout");

    SimServer_setIndication(sim, true);

    bool sawReport = false;
    bool sawGoose = false;
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
            sawReport = true;
        } else if (type && source && strcmp(type->valuestring, "GOOSE") == 0) {
            /* Same gcbInd-only scoping as test_fullSequence_bootstrapModelReportAndGoose_endToEnd's
             * own loop above - the online-discovered model also picks up
             * "gcbDup" (same ds1 dataset), whose own independent GOOSE
             * message can genuinely arrive on this websocket now that
             * cross-GoCB dedup is ipc_dispatcher's concern, not this
             * feature's. */
            cJSON* goCbRef = cJSON_GetObjectItem(source, "goCbRef");
            if (goCbRef && goCbRef->valuestring && strcmp(goCbRef->valuestring, EXPECTED_GOCB_REF) == 0) {
                strncpy(lastGoCbRef, goCbRef->valuestring, sizeof(lastGoCbRef) - 1);
                sawGoose = true;
            }
        }

        cJSON_Delete(parsed);
    }

    TEST_ASSERT_TRUE_MESSAGE(sawReport, "expected a real MMS_REPORT JSON message (from urcbDyn's "
            "dynamically-created dataset) after flipping GGIO1.Ind1.stVal");
    TEST_ASSERT_TRUE_MESSAGE(sawGoose, "expected a GOOSE JSON message after flipping GGIO1.Ind1.stVal");
    TEST_ASSERT_EQUAL_STRING(EXPECTED_GOCB_REF, lastGoCbRef);

    close(ws);
    Orchestration_destroy(handle);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves the daemon's fix for a real, pre-existing gap: scl_bootstrap and
 * mms_report_client each make their own independent MMS association to the
 * same IED, so a device that accepts one acseAuthPassword for SCL fetch but
 * rejects a DIFFERENT one on the actual report connection (an unusual but
 * real device configuration - device_manager_api.c never lets the two
 * diverge over the control channel, but Orchestration_create's own config
 * can, and this is the only way to reproduce the scenario in a test) used to
 * fail completely silently: Orchestration_run returned ORCHESTRATION_OK
 * (MmsReportClient_start returns OK immediately, before any real connect
 * attempt happens), and the device just never reported, with no error
 * anywhere. This test proves Orchestration_wireConnStatusToIpcDispatcher
 * closes that gap: the rejected report-client connection now pushes a
 * CONNECTION_STATUS/CONNECTION_REJECTED message on this device's own stream,
 * and no MMS_REPORT ever arrives.
 *
 * REQUIRES CAP_NET_RAW (goose_subscriber is unconditionally started too, same
 * as every other test in this file) - run with sudo.
 */
void
test_authRequired_bootstrapSucceedsButReportClientRejected_deliversConnectionStatusPush(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/served_files/");
    SimServer_requireAuthentication(sim, AUTH_MISMATCH_PASSWORD_CORRECT);
    SimServer_start(sim, AUTH_MISMATCH_TEST_PORT);
    Thread_sleep(200);

    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);
    config.ipcDispatcherConfig.port = AUTH_MISMATCH_TEST_WS_PORT;
    config.bootstrapConfig.acseAuthPassword = AUTH_MISMATCH_PASSWORD_CORRECT;
    config.reportClientConfig.acseAuthPassword = AUTH_MISMATCH_PASSWORD_WRONG;

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(&config, &createError);
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, createError);

    Orchestration_wireConnStatusToIpcDispatcher(handle);
    Orchestration_setRcbStatusCallback(handle, onRcbStatus, NULL);
    Orchestration_setGooseStatusCallback(handle, onGooseStatus, NULL);

    LinkedList hosts = makeHostList("127.0.0.1");

    OrchestrationErrorDetail detail;
    OrchestrationError runError = Orchestration_run(handle, hosts, AUTH_MISMATCH_TEST_PORT, IED_NAME,
            TEST_INTERFACE, IED_MODEL_ACCESS_REPORT_ONLY, &detail, NULL, NULL);
    LinkedList_destroyStatic(hosts);

    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_OK, runError,
            "expected Orchestration_run to still report success even though the report client's own "
            "connection will be rejected - this is the pre-existing silent-failure gap this test proves");

    int ws = connectAndUpgrade(AUTH_MISMATCH_TEST_WS_PORT);
    TEST_ASSERT_TRUE_MESSAGE(ws >= 0, "websocket handshake to orchestration's own ipc_dispatcher failed");

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&gooseValid),
            "expected the frame adapter to observe a VALID GOOSE feed within the timeout - GOOSE has no "
            "ACSE/MMS association at all, so it's unaffected by the report client's own separate auth");

    bool sawConnectionRejected = false;
    bool sawReport = false;

    for (int i = 0; i < 10 && !sawConnectionRejected; i++) {
        if (!waitReadable(ws, 3000)) break;
        char* json = readOneTextFrame(ws);
        if (!json) break;

        cJSON* parsed = cJSON_Parse(json);
        free(json);
        if (!parsed) continue;

        cJSON* type = cJSON_GetObjectItem(parsed, "type");
        if (type && type->valuestring && strcmp(type->valuestring, "MMS_REPORT") == 0) {
            sawReport = true;
        } else if (type && type->valuestring && strcmp(type->valuestring, "CONNECTION_STATUS") == 0) {
            cJSON* status = cJSON_GetObjectItem(parsed, "status");
            if (status && status->valuestring && strcmp(status->valuestring, "CONNECTION_REJECTED") == 0) {
                sawConnectionRejected = true;
            }
        }

        cJSON_Delete(parsed);
    }

    TEST_ASSERT_TRUE_MESSAGE(sawConnectionRejected,
            "expected a CONNECTION_STATUS/CONNECTION_REJECTED push after the report client's own wrong "
            "password was rejected");
    TEST_ASSERT_FALSE_MESSAGE(sawReport,
            "expected no MMS_REPORT to ever arrive - the report client's own connection is never "
            "actually authenticated");
    TEST_ASSERT_FALSE_MESSAGE(rcbEnabled,
            "expected brcbMain to never enable - the report client's own connection is never established");

    close(ws);
    Orchestration_destroy(handle);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves the fix this whole file was added for: a device whose SCL declares
 * targets for only ONE of MMS/GOOSE no longer fails START_REPORTING entirely
 * - the missing protocol is skipped (outMmsAvailable/outGooseAvailable report
 * which), and the other one still starts normally. All three cases below use
 * Orchestration_runFromLocalFile against a local fixture, no live SimServer
 * needed - MmsReportClient_start's target check (and, when it also has zero
 * targets, GooseSubscription_start's own) happens before any real connection
 * attempt or socket bind, so this is hermetic exactly like the
 * argument-validation tests in tests/orchestration/test_orchestration_api.c,
 * just proving the real (non-argument-validation) behavior instead.
 */
void
test_runFromLocalFile_mmsOnlyFixture_succeedsWithGooseUnavailable(void) {
    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);
    config.ipcDispatcherConfig.port = MMS_ONLY_TEST_WS_PORT;

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(&config, &createError);
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, createError);

    OrchestrationErrorDetail detail;
    bool mmsAvailable = false;
    bool gooseAvailable = true; /* prove it actually gets set false, not left stale */
    OrchestrationError runError = Orchestration_runFromLocalFile(handle,
            "fixtures/served_files/reporter1_mms_only.cid", "127.0.0.1", MMS_ONLY_TEST_PORT, IED_NAME,
            TEST_INTERFACE, IED_MODEL_ACCESS_REPORT_ONLY, &detail, &mmsAvailable, &gooseAvailable);

    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, runError);
    TEST_ASSERT_TRUE_MESSAGE(mmsAvailable, "fixture declares brcbMain - mmsAvailable should be true");
    TEST_ASSERT_FALSE_MESSAGE(gooseAvailable, "fixture declares no <GSEControl> - gooseAvailable should be false");

    Orchestration_destroy(handle);
}

void
test_runFromLocalFile_gooseOnlyFixture_succeedsWithMmsUnavailable(void) {
    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);
    config.ipcDispatcherConfig.port = GOOSE_ONLY_TEST_WS_PORT;

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(&config, &createError);
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, createError);

    OrchestrationErrorDetail detail;
    bool mmsAvailable = true; /* prove it actually gets set false, not left stale */
    bool gooseAvailable = false;
    OrchestrationError runError = Orchestration_runFromLocalFile(handle,
            "fixtures/served_files/reporter1_goose_only.cid", "127.0.0.1", GOOSE_ONLY_TEST_PORT, IED_NAME,
            TEST_INTERFACE, IED_MODEL_ACCESS_REPORT_ONLY, &detail, &mmsAvailable, &gooseAvailable);

    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_OK, runError,
            "if this failed at stage==GOOSE_SUBSCRIBER_START, this test needs CAP_NET_RAW (run with sudo)");
    TEST_ASSERT_FALSE_MESSAGE(mmsAvailable, "fixture declares no <ReportControl> - mmsAvailable should be false");
    TEST_ASSERT_TRUE_MESSAGE(gooseAvailable, "fixture declares gcbInd - gooseAvailable should be true");

    Orchestration_destroy(handle);
}

void
test_runFromLocalFile_neitherProtocolFixture_failsWithNoCapabilities(void) {
    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);
    config.ipcDispatcherConfig.port = NO_CAPABILITIES_TEST_WS_PORT;

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(&config, &createError);
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, createError);

    OrchestrationErrorDetail detail;
    OrchestrationError runError = Orchestration_runFromLocalFile(handle,
            "fixtures/served_files/reporter1_no_capabilities.cid", "127.0.0.1", NO_CAPABILITIES_TEST_PORT,
            IED_NAME, TEST_INTERFACE, IED_MODEL_ACCESS_REPORT_ONLY, &detail, NULL, NULL);

    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_ERR_NO_CAPABILITIES, runError,
            "a device declaring neither <ReportControl> nor <GSEControl> has nothing to monitor at all");
    TEST_ASSERT_EQUAL(ORCHESTRATION_STAGE_NO_CAPABILITIES, detail.stage);

    /* Not left half-started - Orchestration_stop must be a safe no-op. */
    Orchestration_stop(handle);
    Orchestration_destroy(handle);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_fullSequence_bootstrapModelReportAndGoose_endToEnd);
    RUN_TEST(test_onlineDiscoveryFallback_afterNoSclFileFound_endToEnd);
    RUN_TEST(test_authRequired_bootstrapSucceedsButReportClientRejected_deliversConnectionStatusPush);
    RUN_TEST(test_runFromLocalFile_mmsOnlyFixture_succeedsWithGooseUnavailable);
    RUN_TEST(test_runFromLocalFile_gooseOnlyFixture_succeedsWithMmsUnavailable);
    RUN_TEST(test_runFromLocalFile_neitherProtocolFixture_failsWithNoCapabilities);

    return UNITY_END();
}
