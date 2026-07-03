#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "orchestration/service/orchestration_api.h"
#include "hal_thread.h"
#include "sim_types.h"

/*
 * End-to-end test of the full orchestration sequence: scl_bootstrap ->
 * ied_model -> mms_report_client -> goose_subscriber, against a single real
 * "Reporter1" IED simulator (sim_types.h/sim_server.c, see
 * integration_tests/ied_simulator/, fully decoupled from src/) running in
 * this same process, over loopback.
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
#define TEST_INTERFACE "lo"
#define IED_NAME "Reporter1"
#define EXPECTED_RCB_REF "Reporter1LD1/LLN0.BR.brcbMain"
#define EXPECTED_GOCB_REF "Reporter1LD1/LLN0$GO$gcbInd"
#define POLL_INTERVAL_MS 100
#define POLL_MAX_ATTEMPTS 100 /* 100 * 100ms = 10s bound on each wait */

static volatile bool rcbEnabled;
static volatile bool reportReceived;
static volatile bool gooseValid;
static volatile bool gooseRecordReceived;
static char lastRcbReference[256];
static char lastGoCbRef[256];

static void
onReport(void* userParam, const MmsReportRecord* record) {
    (void) userParam;
    strncpy(lastRcbReference, record->rcbReference ? record->rcbReference : "", sizeof(lastRcbReference) - 1);
    lastRcbReference[sizeof(lastRcbReference) - 1] = '\0';
    reportReceived = true;
    MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
}

static void
onRcbStatus(void* userParam, const char* rcbReference, bool enabled, IedClientError lastError) {
    (void) userParam;
    (void) rcbReference;
    (void) lastError;
    if (enabled) rcbEnabled = true;
}

static void
onGooseRecord(void* userParam, const GooseSubscriberRecord* record) {
    (void) userParam;
    strncpy(lastGoCbRef, record->goCbRef ? record->goCbRef : "", sizeof(lastGoCbRef) - 1);
    lastGoCbRef[sizeof(lastGoCbRef) - 1] = '\0';
    gooseRecordReceived = true;
    GooseSubscription_destroyRecord((GooseSubscriberRecord*) record);
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

void
setUp(void) {
    rcbEnabled = false;
    reportReceived = false;
    gooseValid = false;
    gooseRecordReceived = false;
    lastRcbReference[0] = '\0';
    lastGoCbRef[0] = '\0';
}

void
tearDown(void) {}

void
test_fullSequence_bootstrapModelReportAndGoose_endToEnd(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/served_files/");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    OrchestrationError createError;
    OrchestrationHandle handle = Orchestration_create(NULL, &createError);
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, createError);

    Orchestration_setReportCallback(handle, onReport, NULL);
    Orchestration_setRcbStatusCallback(handle, onRcbStatus, NULL);
    Orchestration_setGooseRecordCallback(handle, onGooseRecord, NULL);
    Orchestration_setGooseStatusCallback(handle, onGooseStatus, NULL);

    LinkedList hosts = makeHostList("127.0.0.1");

    OrchestrationErrorDetail detail;
    OrchestrationError runError = Orchestration_run(handle, hosts, TEST_PORT, IED_NAME, TEST_INTERFACE,
            IED_MODEL_ACCESS_REPORT_ONLY, &detail);
    TEST_ASSERT_EQUAL_MESSAGE(ORCHESTRATION_OK, runError,
            "Orchestration_run failed - if stage==GOOSE_SUBSCRIBER_START, this test needs CAP_NET_RAW (run with sudo)");

    LinkedList_destroyStatic(hosts);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbEnabled),
            "expected the reconnect supervisor thread to connect and enable brcbMain within the timeout");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&gooseValid),
            "expected the frame adapter to observe a VALID GOOSE feed within the timeout");

    SimServer_setIndication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report after flipping GGIO1.Ind1.stVal");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&gooseRecordReceived),
            "expected a GOOSE record after flipping GGIO1.Ind1.stVal");

    TEST_ASSERT_EQUAL_STRING(EXPECTED_RCB_REF, lastRcbReference);
    TEST_ASSERT_EQUAL_STRING(EXPECTED_GOCB_REF, lastGoCbRef);

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
