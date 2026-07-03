#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/goose_subscriber/service/goose_subscriber_api.h"
#include "features/ied_model/service/ied_model_api.h"
#include "hal_thread.h"
#include "sim_types.h"

/*
 * End-to-end test: runs a real "Reporter1" IED simulator (sim_types.h /
 * sim_server.c - see integration_tests/ied_simulator/, fully decoupled from
 * src/) in the SAME process, loads the matching real SCL fixture
 * (fixtures/reporter1.cid, a local copy of
 * integration_tests/mms_report_client/fixtures/reporter1.cid - see that
 * file's own header comment for the sim/fixture cross-reference convention)
 * through the real ied_model service API at IED_MODEL_ACCESS_REPORT_ONLY
 * (proving goose_subscriber needs nothing more than the lowest AccessMode
 * tier, per CLAUDE.md's stated intent), connects the real goose_subscriber
 * feature to it on "lo", waits (via the status callback - no arbitrary
 * sleep-and-hope timing) for the liveness thread to observe a VALID GOOSE
 * feed, then flips GGIO1.Ind1.stVal on the simulator and asserts a real
 * GOOSE-carried record arrives with the expected GoCB reference and value.
 *
 * REQUIRES CAP_NET_RAW (raw AF_PACKET socket) - run with sudo, same
 * requirement as the daemon itself per CLAUDE.md. Without it,
 * GooseSubscription_start returns GOOSE_SUBSCRIBER_ERR_RECEIVER_START_FAILED
 * and this test fails fast with a clear message rather than hanging.
 *
 * Whether GOOSE frames actually round-trip over "lo" through libiec61850's
 * specific hal_ethernet implementation was validated empirically via
 * tools/smoke_tests/goose_loopback_smoke_test.c before this test was written
 * (see that file's header comment) - Linux loopback is expected to echo raw
 * PF_PACKET traffic back to a local listener the same way it does for any
 * other AF_PACKET consumer.
 */

#define FIXTURE_PATH "fixtures/reporter1.cid"
#define TEST_INTERFACE "lo"
#define TEST_TCP_PORT 10204 /* SimServer_start always starts the MMS server too - unused here but must not collide with other E2E tests' ports */
#define EXPECTED_GOCB_REF "Reporter1LD1/LLN0$GO$gcbInd"
#define POLL_INTERVAL_MS 100
#define POLL_MAX_ATTEMPTS 100 /* 100 * 100ms = 10s bound on each wait */

static volatile bool sawValidStatus;
static volatile bool sawTrueValue;
static char lastGoCbRef[256];
static int lastEntryCount;

static void
onStatus(void* userParam, const char* goCbRef, GooseSubscriberStatus status, GooseParseError lastParseError) {
    (void) userParam;
    (void) goCbRef;
    (void) lastParseError;
    if (status == GOOSE_SUBSCRIBER_STATUS_VALID) sawValidStatus = true;
}

static void
onRecord(void* userParam, const GooseSubscriberRecord* record) {
    (void) userParam;

    strncpy(lastGoCbRef, record->goCbRef ? record->goCbRef : "", sizeof(lastGoCbRef) - 1);
    lastGoCbRef[sizeof(lastGoCbRef) - 1] = '\0';
    lastEntryCount = record->entryCount;

    if (record->entryCount > 0 && record->entries[0].value
            && MmsValue_getType(record->entries[0].value) == MMS_BOOLEAN
            && MmsValue_getBoolean(record->entries[0].value)) {
        sawTrueValue = true;
    }

    /* Ownership transferred to us per the API contract - free it. */
    GooseSubscription_destroyRecord((GooseSubscriberRecord*) record);
}

static bool
waitUntil(volatile bool* flag) {
    for (int i = 0; i < POLL_MAX_ATTEMPTS; i++) {
        if (*flag) return true;
        Thread_sleep(POLL_INTERVAL_MS);
    }
    return false;
}

void
setUp(void) {
    sawValidStatus = false;
    sawTrueValue = false;
    lastEntryCount = 0;
    lastGoCbRef[0] = '\0';
}

void
tearDown(void) {}

void
test_dataChangeOnServer_triggersGooseRecordWithNewValue(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_TCP_PORT);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_REPORT_ONLY, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    GooseSubscriberError createError;
    GooseSubscriberHandle handle = GooseSubscription_create(iedModel, TEST_INTERFACE, NULL, &createError);
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(GOOSE_SUBSCRIBER_OK, createError);

    GooseSubscription_setRecordCallback(handle, onRecord, NULL);
    GooseSubscription_setStatusCallback(handle, onStatus, NULL);

    GooseSubscriberError startError = GooseSubscription_start(handle);
    TEST_ASSERT_EQUAL_MESSAGE(GOOSE_SUBSCRIBER_OK, startError,
            "GooseSubscription_start failed - this test needs CAP_NET_RAW (run with sudo)");

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&sawValidStatus),
            "expected the frame adapter to observe a VALID GOOSE feed within the timeout");

    SimServer_setIndication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&sawTrueValue),
            "expected a GOOSE record carrying the new value after flipping GGIO1.Ind1.stVal");

    TEST_ASSERT_EQUAL_STRING(EXPECTED_GOCB_REF, lastGoCbRef);
    TEST_ASSERT_EQUAL_INT(1, lastEntryCount);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&sawValidStatus),
            "expected the liveness thread to observe a VALID GOOSE feed at some point during the run");

    GooseSubscription_destroy(handle);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_dataChangeOnServer_triggersGooseRecordWithNewValue);

    return UNITY_END();
}
