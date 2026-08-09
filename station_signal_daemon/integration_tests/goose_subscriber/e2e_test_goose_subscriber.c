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
static volatile bool anyRecordReceived;
static char lastGoCbRef[256];
static int lastEntryCount;
static char lastEntry0Reference[256];
static char lastEntry1Reference[256];

static void
onStatus(void* userParam, const char* goCbRef, GooseSubscriberStatus status, GooseParseError lastParseError) {
    (void) userParam;
    (void) lastParseError;
    /* This fixture also declares "gcbDup" (same ds1 dataset as gcbInd,
     * reproducing a real network's redundant-publisher pattern) - these two
     * tests are specifically about gcbInd's own bootstrap-suppression/
     * data-change behavior, so gcbDup's own independent status transitions
     * must not be observed here. Cross-target duplicate content is no longer
     * suppressed at this layer at all (see
     * test_crossTargetDuplicateContent_bothIdenticalGoCbsReachCallback_dedupMovedDownstream
     * below) - both gcbInd and gcbDup now genuinely reach a real callback,
     * so a test that only cares about one of them must filter for it
     * itself. */
    if (goCbRef && strcmp(goCbRef, EXPECTED_GOCB_REF) != 0) return;
    if (status == GOOSE_SUBSCRIBER_STATUS_VALID) sawValidStatus = true;
}

static void
onRecord(void* userParam, const GooseSubscriberRecord* record) {
    (void) userParam;

    /* Same gcbInd-only scoping as onStatus above - gcbDup's own independent
     * reports (real, no longer suppressed) must not be observed by these two
     * gcbInd-specific tests. */
    if (!record->goCbRef || strcmp(record->goCbRef, EXPECTED_GOCB_REF) != 0) {
        GooseSubscription_destroyRecord((GooseSubscriberRecord*) record);
        return;
    }

    anyRecordReceived = true;

    strncpy(lastGoCbRef, record->goCbRef ? record->goCbRef : "", sizeof(lastGoCbRef) - 1);
    lastGoCbRef[sizeof(lastGoCbRef) - 1] = '\0';
    lastEntryCount = record->entryCount;

    if (record->entryCount > 0) {
        strncpy(lastEntry0Reference, record->entries[0].reference ? record->entries[0].reference : "",
                sizeof(lastEntry0Reference) - 1);
        lastEntry0Reference[sizeof(lastEntry0Reference) - 1] = '\0';
    }
    if (record->entryCount > 1) {
        strncpy(lastEntry1Reference, record->entries[1].reference ? record->entries[1].reference : "",
                sizeof(lastEntry1Reference) - 1);
        lastEntry1Reference[sizeof(lastEntry1Reference) - 1] = '\0';
    }

    if (record->entryCount > 0 && record->entries[0].value
            && MmsValue_getType(record->entries[0].value) == MMS_BOOLEAN
            && MmsValue_getBoolean(record->entries[0].value)) {
        sawTrueValue = true;
    }

    /* Ownership transferred to us per the API contract - free it. */
    GooseSubscription_destroyRecord((GooseSubscriberRecord*) record);
}

static bool
waitUntilOrTimeout(volatile bool* flag, int maxAttempts) {
    for (int i = 0; i < maxAttempts; i++) {
        if (*flag) return true;
        Thread_sleep(POLL_INTERVAL_MS);
    }
    return false;
}

static bool
waitUntil(volatile bool* flag) {
    return waitUntilOrTimeout(flag, POLL_MAX_ATTEMPTS);
}

/* Bounded NEGATIVE wait - used only to assert something does NOT arrive
 * (e.g. the very first frame for a target must never reach the record
 * callback). Deliberately much shorter than POLL_MAX_ATTEMPTS's 10s - see
 * the identical helper/rationale in e2e_test_mms_report_client.c. */
#define NEGATIVE_POLL_MAX_ATTEMPTS 20

static bool
waitBriefly(volatile bool* flag) {
    return waitUntilOrTimeout(flag, NEGATIVE_POLL_MAX_ATTEMPTS);
}

void
setUp(void) {
    sawValidStatus = false;
    sawTrueValue = false;
    anyRecordReceived = false;
    lastEntryCount = 0;
    lastGoCbRef[0] = '\0';
    lastEntry0Reference[0] = '\0';
    lastEntry1Reference[0] = '\0';
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
    TEST_ASSERT_EQUAL_INT(2, lastEntryCount);

    /* GOOSE never carries a server-supplied reference - these are always
     * resolved via the new IedModel_getDataSetMemberReferences-backed path,
     * the primary end-to-end proof point for goose_subscriber's always-on
     * reference labeling. */
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$Ind1$stVal", lastEntry0Reference);
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$Ind1$q", lastEntry1Reference);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&sawValidStatus),
            "expected the liveness thread to observe a VALID GOOSE feed at some point during the run");

    GooseSubscription_destroy(handle);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves the bootstrap-suppression mechanism end-to-end for GOOSE: the very
 * first frame this subscriber ever receives for gcbInd - which the publisher
 * sends immediately/repeatedly once SimServer_start runs, well before
 * GGIO1.Ind1.stVal is ever flipped - must never reach the record callback at
 * all (cache-seed only, GOOSE's equivalent of MMS's GI suppression, since
 * GOOSE has no GI concept of its own). Only the genuine post-flip change
 * (proven separately by test_dataChangeOnServer_triggersGooseRecordWithNewValue)
 * ever reaches the callback.
 */
void
test_firstFrameEverPerTarget_neverReachesCallback(void) {
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

    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&anyRecordReceived),
            "the very first frame for this target must never reach the record callback - it's "
            "cache-seed only, even though the publisher has been retransmitting since SimServer_start");

    GooseSubscription_destroy(handle);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

#define EXPECTED_GOCB_REF_DUP "Reporter1LD1/LLN0$GO$gcbDup"

/*
 * Isolated callback state for
 * test_crossTargetDuplicateContent_onlyOneOfTwoIdenticalGoCbsReachesCallback
 * below - deliberately NOT the shared globals the test above uses, since
 * this test needs to distinguish two independently-VALID targets
 * (gcbInd/gcbDup) and count records rather than just track the latest one.
 */
typedef struct {
    volatile bool gcbIndValid;
    volatile bool gcbDupValid;
    volatile int gcbIndOrDupRecordCount;
} CrossTargetDedupTestState;

static void
onStatusForCrossTargetDedupTest(void* userParam, const char* goCbRef, GooseSubscriberStatus status,
        GooseParseError lastParseError) {
    (void) lastParseError;
    if (status != GOOSE_SUBSCRIBER_STATUS_VALID || !goCbRef) return;

    CrossTargetDedupTestState* state = (CrossTargetDedupTestState*) userParam;
    if (strcmp(goCbRef, EXPECTED_GOCB_REF) == 0) state->gcbIndValid = true;
    else if (strcmp(goCbRef, EXPECTED_GOCB_REF_DUP) == 0) state->gcbDupValid = true;
}

static void
onRecordForCrossTargetDedupTest(void* userParam, const GooseSubscriberRecord* record) {
    CrossTargetDedupTestState* state = (CrossTargetDedupTestState*) userParam;

    if (record->goCbRef && (strcmp(record->goCbRef, EXPECTED_GOCB_REF) == 0
            || strcmp(record->goCbRef, EXPECTED_GOCB_REF_DUP) == 0)) {
        /* Only count a genuine value-change delivery (the initial state is
         * "false" and both publishers start retransmitting it immediately
         * on SimServer_start, before this subscriber even attaches - the
         * per-position bootstrap-suppression mechanism now drops that
         * entire startup burst before it ever reaches this callback at all,
         * not just collapses it to one snapshot per target - see
         * test_firstFrameEverPerTarget_neverReachesCallback below for the
         * dedicated proof of that). */
        if (record->entryCount > 0 && record->entries[0].value
                && MmsValue_getType(record->entries[0].value) == MMS_BOOLEAN
                && MmsValue_getBoolean(record->entries[0].value)) {
            state->gcbIndOrDupRecordCount++;
        }
    }

    GooseSubscription_destroyRecord((GooseSubscriberRecord*) record);
}

static bool
waitUntilAtLeast(volatile int* counter, int threshold) {
    for (int i = 0; i < POLL_MAX_ATTEMPTS; i++) {
        if (*counter >= threshold) return true;
        Thread_sleep(POLL_INTERVAL_MS);
    }
    return false;
}

/*
 * Proves goose_subscriber's own recordCallback is now deliberately NOT the
 * layer that suppresses cross-GoCB duplicate content - that job moved to
 * ipc_dispatcher's own per-protocol dedup cache
 * (IpcDispatcherUseCases_shouldForwardWithinProtocol, proven in
 * integration_tests/ipc_dispatcher/ and unit-tested in
 * tests/ipc_dispatcher/test_ipc_dispatcher_usecases.c), the one place both
 * this feature's GoCBs AND mms_report_client's RCBs actually converge.
 *
 * gcbInd and gcbDup (fixtures/reporter1.cid) are two independently-subscribed
 * GoCBs publishing the IDENTICAL ds1 dataset - reproducing a real network's
 * redundant-publisher pattern. Flipping GGIO1.Ind1.stVal changes both
 * publishers' state at nearly the same moment; BOTH resulting
 * identical-content records must now reach the record callback - this
 * feature no longer decides which of them is "the" real one.
 */
void
test_crossTargetDuplicateContent_bothIdenticalGoCbsReachCallback_dedupMovedDownstream(void) {
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

    CrossTargetDedupTestState state = { 0 };
    GooseSubscription_setRecordCallback(handle, onRecordForCrossTargetDedupTest, &state);
    GooseSubscription_setStatusCallback(handle, onStatusForCrossTargetDedupTest, &state);

    GooseSubscriberError startError = GooseSubscription_start(handle);
    TEST_ASSERT_EQUAL_MESSAGE(GOOSE_SUBSCRIBER_OK, startError,
            "GooseSubscription_start failed - this test needs CAP_NET_RAW (run with sudo)");

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.gcbIndValid), "expected gcbInd to become VALID");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.gcbDupValid), "expected gcbDup to become VALID");

    SimServer_setIndication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntilAtLeast(&state.gcbIndOrDupRecordCount, 2),
            "expected BOTH gcbInd's and gcbDup's identical new-value frames to reach the callback - "
            "goose_subscriber itself no longer suppresses cross-GoCB duplicates, ipc_dispatcher's "
            "own dedup cache does that downstream instead");

    /* Generous settle window to make sure no MORE than 2 ever arrive (a
     * regression the other direction - e.g. a redelivery bug). */
    Thread_sleep(500);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, state.gcbIndOrDupRecordCount,
            "gcbInd and gcbDup publish byte-identical content (same ds1 dataset) - both must reach "
            "this feature's own callback now; suppression is entirely ipc_dispatcher's concern");

    GooseSubscription_destroy(handle);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_dataChangeOnServer_triggersGooseRecordWithNewValue);
    RUN_TEST(test_firstFrameEverPerTarget_neverReachesCallback);
    RUN_TEST(test_crossTargetDuplicateContent_bothIdenticalGoCbsReachCallback_dedupMovedDownstream);

    return UNITY_END();
}
