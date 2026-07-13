#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/mms_report_client/service/mms_report_client_api.h"
#include "features/ied_model/service/ied_model_api.h"
#include "hal_thread.h"
#include "sim_types.h"

/*
 * End-to-end test: runs a real "Reporter1" IED simulator (sim_types.h /
 * sim_server.c - see integration_tests/ied_simulator/, fully decoupled from
 * src/) in the SAME process, loads the matching real SCL fixture
 * (fixtures/reporter1.cid) through the real ied_model service API in
 * IED_MODEL_ACCESS_READ_AND_WRITE mode (proving mms_report_client works
 * regardless of AccessMode, not just at REPORT_ONLY), connects the real
 * mms_report_client feature to it, waits (via the RCB status callback - no
 * arbitrary sleep-and-hope timing) for the buffered RCB to be enabled, then
 * flips GGIO1.Ind1.stVal on the simulator and asserts a real report arrives
 * with the expected RCB reference, value, and reason-for-inclusion.
 *
 * Fixture/model shape (kept in sync by hand - see the cross-referencing
 * comment in sim_types.h): IED "Reporter1", LDevice "LD1", one buffered RCB
 * ("brcbMain", trgOps=dchg+qchg+gi) over dataset "ds1" containing
 * GGIO1.Ind1.stVal followed by GGIO1.Ind1.q.
 *
 * Also proves MmsReportClientConfig.acseAuthPassword end-to-end (see
 * mms_report_client_auth.h) against a real SimServer_requireAuthentication-
 * protected instance: a correct password connects and enables the RCB, a
 * wrong one never does - same SimServer_requireAuthentication mechanism
 * scl_bootstrap's own E2E test already exercises for the discovery side.
 */

#define FIXTURE_PATH "fixtures/reporter1.cid"
#define TEST_PORT 10203
#define TEST_PORT_AUTH_CORRECT 10206 /* distinct from every other E2E test's port - see
                                        integration_tests/orchestration's own registry comment */
#define TEST_PORT_AUTH_WRONG 10207
#define TEST_PORT_DYNAMIC_DATASET 10209
#define TEST_PORT_RECONNECT 10210
#define TEST_PORT_CROSS_RCB_DEDUP 10211
#define TEST_PASSWORD "secret123"
#define POLL_INTERVAL_MS 100
#define POLL_MAX_ATTEMPTS 100 /* 100 * 100ms = 10s bound on each wait */

static volatile bool rcbEnabled;
static volatile bool reportReceived;
static char lastRcbReference[256];
static int lastEntryCount;
static bool lastEntryValue;
static ReasonForInclusion lastReason;
static char lastEntry0Reference[256];
static char lastEntry1Reference[256];
static bool lastEntry0HasPreviousValue;
static bool lastEntry0PreviousValue;

/* The fixture now also declares "urcbDyn" (no datSet - see
 * test_dynamicDataset_createdOnEnable_andReportsRealChange below), enabled
 * alongside brcbMain on every test's connect. interestedRcbReference lets a
 * test that cares about one specific RCB's report content ignore the other
 * RCB's interleaved reports instead of racing on the shared globals above -
 * empty (the default, reset in setUp) means "match any", preserving every
 * pre-existing test's behavior untouched. dynamicRcbEnabled/dynamicRcbFailed
 * track urcbDyn's own enable outcome specifically, since rcbEnabled alone
 * would go true from brcbMain's success even if urcbDyn's dynamic dataset
 * creation silently failed. */
static char interestedRcbReference[256];
static volatile bool dynamicRcbEnabled;
static volatile bool dynamicRcbFailed;

/* Counts MMS_REPORT_CLIENT_CONNECTED transitions, for
 * test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsReseededPreviousValue
 * to observe that a real second connection (not just a re-enable on the same
 * association) actually happened. */
static volatile int connectedCount;

/* Counts successful brcbMain enable events specifically (RptEna set true),
 * for test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsReseededPreviousValue
 * to prove that one real reconnect produces exactly ONE enable cycle, not
 * two-or-more - see supervisorLoop's own comment on the reconnect "storm"
 * this guards against (mms_report_client_connection.c). */
static volatile int brcbMainEnableCount;

static void
onConnState(void* userParam, MmsReportClientConnState state) {
    (void) userParam;
    if (state == MMS_REPORT_CLIENT_CONNECTED) connectedCount++;
}

static void
onRcbStatus(void* userParam, const char* rcbReference, bool enabled, IedClientError lastError) {
    (void) userParam;
    (void) lastError;
    if (enabled) rcbEnabled = true;

    if (rcbReference && strcmp(rcbReference, "Reporter1LD1/LLN0.BR.brcbMain") == 0 && enabled) {
        brcbMainEnableCount++;
    }

    if (rcbReference && strcmp(rcbReference, "Reporter1LD1/GGIO1.RP.urcbDyn") == 0) {
        if (enabled) dynamicRcbEnabled = true;
        else dynamicRcbFailed = true;
    }
}

static void
onReport(void* userParam, const MmsReportRecord* record) {
    (void) userParam;

    if (interestedRcbReference[0] != '\0'
            && (!record->rcbReference || strcmp(record->rcbReference, interestedRcbReference) != 0)) {
        MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
        return;
    }

    strncpy(lastRcbReference, record->rcbReference ? record->rcbReference : "", sizeof(lastRcbReference) - 1);
    lastRcbReference[sizeof(lastRcbReference) - 1] = '\0';
    lastEntryCount = record->entryCount;
    if (record->entryCount > 0 && record->entries[0].value) {
        lastEntryValue = MmsValue_getBoolean(record->entries[0].value);
        lastReason = record->entries[0].reason;
        lastEntry0HasPreviousValue = record->entries[0].previousValue != NULL;
        lastEntry0PreviousValue = lastEntry0HasPreviousValue
                && MmsValue_getBoolean(record->entries[0].previousValue);
    }
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
    reportReceived = true;

    /* Ownership transferred to us per the API contract - free it. */
    MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
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
 * (e.g. a GI-triggered snapshot must never reach the report callback).
 * Deliberately much shorter than POLL_MAX_ATTEMPTS's 10s: a report that
 * WOULD arrive does so within a loopback round-trip (order of ms), so 2s is
 * a generous safety margin without slowing the suite down waiting out a
 * full 10s for something that's correctly never going to happen. */
#define NEGATIVE_POLL_MAX_ATTEMPTS 20

static bool
waitBriefly(volatile bool* flag) {
    return waitUntilOrTimeout(flag, NEGATIVE_POLL_MAX_ATTEMPTS);
}

static bool
waitUntilAtLeast(volatile int* counter, int threshold) {
    for (int i = 0; i < POLL_MAX_ATTEMPTS; i++) {
        if (*counter >= threshold) return true;
        Thread_sleep(POLL_INTERVAL_MS);
    }
    return false;
}

void
setUp(void) {
    rcbEnabled = false;
    reportReceived = false;
    lastEntryCount = 0;
    lastRcbReference[0] = '\0';
    lastEntry0Reference[0] = '\0';
    lastEntry1Reference[0] = '\0';
    lastEntry0HasPreviousValue = false;
    lastEntry0PreviousValue = false;
    interestedRcbReference[0] = '\0';
    dynamicRcbEnabled = false;
    dynamicRcbFailed = false;
    connectedCount = 0;
    brcbMainEnableCount = 0;
}

void
tearDown(void) {}

void
test_dataChangeOnServer_triggersReportWithNewValue(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT, &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    strncpy(interestedRcbReference, "Reporter1LD1/LLN0.BR.brcbMain", sizeof(interestedRcbReference) - 1);
    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbEnabled),
            "expected the reconnect supervisor thread to connect and enable brcbMain within the timeout");

    /* This client never requests GI on enable (see enableOneTarget's own
     * comment - unreliable on real hardware, matches goose_subscriber's
     * GI-less design exactly), so nothing has seeded the value-diff cache
     * yet - the FIRST report this client ever receives for a position has
     * cached==NULL and is therefore itself treated as a bootstrap event
     * (silently seeds the cache, never forwarded). This is deliberate, not a
     * gap: shouldForwardAndUpdateCache cannot safely special-case "first
     * observation with a real-change reason" without reopening the exact bug
     * real-hardware testing found (a reconnect also resets the cache to
     * NULL, and this same simulator tags a reconnect's redelivered-but-
     * unchanged report as DATA_CHANGE too - trusting the reason bit on a
     * NULL cache would forward that right back). So this test flips twice:
     * the first flip is the throwaway seed, the second is the one that
     * actually reaches the callback. */
    SimServer_setIndication(sim, true);
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "the first-ever observation for this position must be bootstrap-suppressed, "
            "since nothing has ever seeded the cache and this client never requests GI");

    SimServer_setIndication(sim, false);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report after the second flip of GGIO1.Ind1.stVal");

    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/LLN0.BR.brcbMain", lastRcbReference);
    TEST_ASSERT_EQUAL_INT(2, lastEntryCount);
    TEST_ASSERT_FALSE(lastEntryValue);
    TEST_ASSERT_TRUE_MESSAGE((lastReason & IEC61850_REASON_DATA_CHANGE) != 0,
            "expected the report's reason-for-inclusion to include data-change");
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0HasPreviousValue,
            "the throwaway seed flip's value must surface as this entry's previousValue");
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0PreviousValue,
            "previousValue must be the seed flip's value (true), not the simulator's original default");

    /* brcbMain's OptFlds has no DataRef (see sim_server.c) - these are the
     * locally-resolved fallback references, proving the new
     * IedModel_getDataSetMemberReferences-backed path end-to-end against a
     * real IedModelHandle/SCL/MmsReportClient_start. */
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$Ind1$stVal", lastEntry0Reference);
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$Ind1$q", lastEntry1Reference);

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves MmsReportClientAuth_configurePasswordAuth end-to-end against a
 * real password-protected simulator IED: the reconnect supervisor thread
 * must apply ACSE auth on its very first connect attempt (not just after a
 * failed unauthenticated one - see mms_report_client_auth.h for why no
 * retry dance is needed here) and successfully enable the RCB.
 */
void
test_authRequired_correctPassword_connectsAndEnablesRcb(void) {
    SimServer sim = SimServer_create();
    SimServer_requireAuthentication(sim, TEST_PASSWORD);
    SimServer_start(sim, TEST_PORT_AUTH_CORRECT);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);
    config.acseAuthPassword = TEST_PASSWORD;

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_AUTH_CORRECT,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbEnabled),
            "expected the correct ACSE password to let the supervisor thread connect and enable brcbMain");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Negative case: a password-protected IED with the wrong password configured
 * must never successfully connect - the reconnect supervisor thread keeps
 * retrying with exponential backoff forever, but brcbMain never gets
 * enabled. Proves the auth failure doesn't silently fall back to an
 * unauthenticated connection.
 */
void
test_authRequired_wrongPassword_neverConnects(void) {
    SimServer sim = SimServer_create();
    SimServer_requireAuthentication(sim, TEST_PASSWORD);
    SimServer_start(sim, TEST_PORT_AUTH_WRONG);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);
    config.acseAuthPassword = "wrong-password";

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_AUTH_WRONG,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_FALSE_MESSAGE(waitUntil(&rcbEnabled),
            "expected the wrong ACSE password to never let the supervisor thread connect");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves mms_report_client's dynamic dataset creation
 * (IedConnection_createDataSet, data/mms_report_client_connection.c's
 * getOrCreateDynamicDataset) end-to-end against a real MMS association: the
 * fixture's "urcbDyn" (parented under GGIO1, no datSet attribute at all -
 * datSet="Dyn" in SCL terms) would otherwise fail setRCBValues with
 * IED_ERROR_OBJECT_VALUE_INVALID, exactly as it does against a real device
 * like E13_6MD. Instead, mms_report_client synthesizes an association-scoped
 * dataset covering every FC=ST leaf under GGIO1 (Ind1.stVal/q/t,
 * SPCSO1.stVal/q/t - GGIO1 has no FC=MX attributes in this fixture), enables
 * the RCB against it, and a real data-change report arrives after flipping
 * GGIO1.Ind1.stVal.
 *
 * This client never requests GI on enable, so nothing ever seeds the
 * value-diff cache automatically - every entry in urcbDyn's first-ever
 * report has cached == NULL and is therefore bootstrap-suppressed regardless
 * of its own reason (see shouldForwardAndUpdateCache's own doc comment for
 * why a real-change reason is never trusted to bypass this, even on a first
 * observation - the same real-hardware finding that fixed the flooding bug
 * also rules out narrowly trusting reason bits here). This test flips twice:
 * the first flip is a throwaway seed for all 6 dataset members, the second
 * is what actually reaches the callback. On that second flip, only Ind1's
 * own group (stVal + its q/t siblings, dragged along by group-extension)
 * survives - SPCSO1's unrelated group stays suppressed (its own cache was
 * seeded by the first flip too, and SPCSO1's value never itself changes, so
 * nothing in that group individually qualifies and nothing drags it in).
 * Expected surviving entries: Ind1.stVal, Ind1.q, Ind1.t (3, not all 6
 * dataset members).
 */
void
test_dynamicDataset_createdOnEnable_andReportsRealChange(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_DYNAMIC_DATASET);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_DYNAMIC_DATASET,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    strncpy(interestedRcbReference, "Reporter1LD1/GGIO1.RP.urcbDyn", sizeof(interestedRcbReference) - 1);
    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicRcbEnabled),
            "expected urcbDyn (no SCL-declared datSet) to get a dynamically-created dataset and enable successfully");
    TEST_ASSERT_FALSE_MESSAGE(dynamicRcbFailed,
            "urcbDyn must not fail after this feature exists - it used to fail with error 32 (OBJECT_VALUE_INVALID)");

    /* Throwaway seed flip: this is urcbDyn's first-ever report - every one of
     * its 6 members has cached == NULL and is bootstrap-suppressed (see this
     * test's own doc comment above). */
    SimServer_setIndication(sim, true);
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "urcbDyn's first-ever report must be entirely bootstrap-suppressed, since nothing has "
            "seeded the cache and this client never requests GI");

    SimServer_setIndication(sim, false);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report from urcbDyn's dynamically-created dataset after the second flip");

    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1.RP.urcbDyn", lastRcbReference);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, lastEntryCount,
            "only Ind1's own group (stVal + its q/t siblings, dragged along by group-extension) "
            "should survive - SPCSO1's unrelated group stays suppressed (unchanged since the seed)");
    TEST_ASSERT_FALSE(lastEntryValue);
    TEST_ASSERT_TRUE_MESSAGE((lastReason & IEC61850_REASON_DATA_CHANGE) != 0,
            "expected the report's reason-for-inclusion to include data-change");
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$Ind1$stVal", lastEntry0Reference);
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0HasPreviousValue,
            "the throwaway seed flip's value must surface as this entry's previousValue");
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0PreviousValue,
            "previousValue must be the seed flip's value (true), not the simulator's original default");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves the reconnect cache-reseeding mechanism end-to-end:
 * MmsReportClientMemberRefCacheEntry's lastForwardedValues cache is built
 * once at MmsReportClient_start and is never rebuilt across reconnects, so
 * without explicitly resetting it on every (re-)enable
 * (mms_report_client_connection.c's enableOneTarget, via
 * MmsReportClientUseCases_resetValueDiffCache), a reconnect's redelivered
 * report would be diff-checked against the STALE pre-disconnect cache
 * instead of being treated as a fresh bootstrap event.
 *
 * SimServer_stop/_start operate on the same SimServer (same IedModel/IedServer
 * object - only the TCP listener toggles, see SimServer_stop/_start's own
 * comments), so GGIO1.Ind1.stVal survives the restart unchanged (true, set by
 * the pre-disconnect flip below). This simulator has been confirmed to
 * redeliver brcbMain's buffered content on re-enable and tag it DATA_CHANGE
 * even though the value never actually changed relative to the pre-disconnect
 * state - shouldForwardAndUpdateCache no longer trusts that bit
 * unconditionally (see its own doc comment for the real-hardware finding that
 * proved the old unconditional-trust design unsafe), so the redelivery must
 * still be suppressed here exactly like this client's own first-ever
 * observation for a position (this client never requests GI - see
 * enableOneTarget's own comment). This test flips the value once before
 * disconnecting (so the pre-disconnect cache is non-default), reconnects, asserts the reconnect's
 * own redelivery never reaches the callback, then flips the value AGAIN
 * after reconnecting and asserts the resulting report's previousValue
 * reflects the reconnect's own re-seeded state (true, matching the
 * post-restart live value) rather than stale pre-disconnect state - proving
 * the reset+reseed happened correctly, not just that nothing crashed.
 */
void
test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsReseededPreviousValue(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_RECONNECT);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_RECONNECT,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    strncpy(interestedRcbReference, "Reporter1LD1/LLN0.BR.brcbMain", sizeof(interestedRcbReference) - 1);
    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);
    MmsReportClient_setConnectionStateCallback(client, onConnState, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbEnabled),
            "expected the first connect to enable brcbMain within the timeout");
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "the first connect must not produce a report - this client never requests GI, and "
            "nothing has changed yet");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, connectedCount, "expected exactly one connection so far");

    /* Generous settle window for a hypothetical extra, redundant enable cycle
     * (the reconnect "storm" bug this guards against - see supervisorLoop's
     * own comment) to also land, before asserting the count is exactly 1. */
    Thread_sleep(500);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, brcbMainEnableCount,
            "expected exactly one enable cycle for brcbMain from the first connect, not a "
            "redundant re-enable storm");

    /* This client never requests GI, so nothing has seeded the value-diff
     * cache yet - this flip is simultaneously this test's only pre-disconnect
     * transition (deliberately just one, matching the buffered RCB's own
     * un-acked backlog staying minimal by disconnect time) AND the
     * bootstrap-suppressed first-ever observation for this position: it's
     * silently seeded (cache becomes true, non-default) but never forwarded
     * itself, exactly like a foreign GI would have been had one been
     * requested. */
    SimServer_setIndication(sim, true);
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "the first-ever observation for this position must be bootstrap-suppressed, since "
            "this client never requests GI - it still silently seeds the cache with true (making "
            "it non-default), so a stale-cache bug (using the ORIGINAL false instead of the "
            "reconnect's own reseeded true as previousValue below) would be distinguishable from "
            "a correct reseed");
    rcbEnabled = false;

    SimServer_stop(sim);

    /* No explicit wait for the disconnect to be observed - the reconnect
     * supervisor's own backoff/retry loop will keep failing against the
     * closed port until SimServer_start reopens it below; waitUntil(&rcbEnabled)
     * afterwards is what actually bounds this test. */
    SimServer_start(sim, TEST_PORT_RECONNECT);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbEnabled),
            "expected the reconnect supervisor to reconnect and re-enable brcbMain within the timeout");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, connectedCount,
            "expected a second, genuinely new connection after the server restart");

    /* Proves the reconnect "storm" bug (supervisorLoop treating any wake as
     * "go reconnect", re-running enableAllTargets multiple times for one real
     * connect - see its own comment in mms_report_client_connection.c) is
     * fixed: a single reconnect must produce exactly ONE additional enable
     * cycle, not two-or-more racing each other. */
    TEST_ASSERT_TRUE_MESSAGE(waitUntilAtLeast(&brcbMainEnableCount, 2),
            "expected the reconnect to enable brcbMain a second time");
    Thread_sleep(500);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, brcbMainEnableCount,
            "expected exactly one enable cycle for the reconnect (total 2), not a redundant "
            "re-enable storm racing report delivery");

    /* This simulator redelivers brcbMain's buffered content on re-enable and
     * tags it DATA_CHANGE even though the value never actually changed -
     * shouldForwardAndUpdateCache no longer trusts that bit unconditionally
     * (see its own doc comment), so this redelivery must be suppressed
     * exactly like this client's own first-ever observation for a position,
     * silently reseeding the cache instead. Also proves the reset-before-
     * enable ordering fix (see enableOneTarget's own comment): the cache is
     * guaranteed clear before this redelivered report can possibly be
     * dispatched, however quickly it arrives relative to the enable call
     * returning. */
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "the reconnect's own redelivered report must never reach the callback either, even "
            "though this simulator tags it DATA_CHANGE - the value never actually changed, and "
            "a reason bit is no longer trusted to bypass the value-diff check");

    /* Flip again, now FROM the reconnect-reseeded true back to false. */
    SimServer_setIndication(sim, false);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report after flipping GGIO1.Ind1.stVal post-reconnect");
    TEST_ASSERT_FALSE(lastEntryValue);
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0HasPreviousValue,
            "the reconnect's own redelivered value must surface as this entry's previousValue");
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0PreviousValue,
            "previousValue must reflect the reconnect's own report (true, the post-restart live "
            "value), not stale pre-disconnect state");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Isolated callback state for test_crossRcbDuplicateContent_onlyOneOfTwoIdenticalRcbsReachesCallback
 * below - deliberately NOT the shared globals every other test in this file
 * uses, since this test needs to distinguish three concurrently-enabling
 * RCBs (brcbMain/brcbDup/urcbDyn) and count reports rather than just track
 * the latest one.
 */
typedef struct {
    volatile bool brcbMainEnabled;
    volatile bool brcbDupEnabled;
    volatile bool urcbDynEnabled;
    volatile int brcbMainOrDupReportCount;
} CrossRcbDedupTestState;

static void
onRcbStatusForCrossRcbDedupTest(void* userParam, const char* rcbReference, bool enabled, IedClientError lastError) {
    (void) lastError;
    if (!enabled || !rcbReference) return;

    CrossRcbDedupTestState* state = (CrossRcbDedupTestState*) userParam;
    if (strcmp(rcbReference, "Reporter1LD1/LLN0.BR.brcbMain") == 0) state->brcbMainEnabled = true;
    else if (strcmp(rcbReference, "Reporter1LD1/LLN0.BR.brcbDup") == 0) state->brcbDupEnabled = true;
    else if (strcmp(rcbReference, "Reporter1LD1/GGIO1.RP.urcbDyn") == 0) state->urcbDynEnabled = true;
}

static void
onReportForCrossRcbDedupTest(void* userParam, const MmsReportRecord* record) {
    CrossRcbDedupTestState* state = (CrossRcbDedupTestState*) userParam;

    if (record->rcbReference && (strcmp(record->rcbReference, "Reporter1LD1/LLN0.BR.brcbMain") == 0
            || strcmp(record->rcbReference, "Reporter1LD1/LLN0.BR.brcbDup") == 0)) {
        state->brcbMainOrDupReportCount++;
    }

    MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
}

/*
 * Proves MmsReportClientUseCases_shouldForwardAcrossRcb end-to-end: brcbMain
 * and brcbDup (fixtures/reporter1.cid) are two independently-enabled RCBs
 * over the IDENTICAL ds1 dataset/TrgOps/OptFields - reproducing a real
 * device's redundant/reserved RCB pattern (e.g. "urcbA01"/"urcbB01"). Both
 * get RptEna on connect (this client never requests GI), but each one's
 * first-ever observation is cache-seed-only and never reaches the callback
 * at all (bootstrap suppression) - so cross-RCB dedup can't be proven
 * against that first observation. Instead, this test flips GGIO1.Ind1.stVal
 * AFTER both RCBs are enabled: each independently produces a real,
 * byte-identical DATA_CHANGE report at nearly the same moment, and only one
 * of the two must ever reach the report callback. urcbDyn (also declared in
 * this fixture) reports a different, larger dataset, so it's unaffected and
 * used here only to confirm the client is otherwise fully connected/enabled.
 */
void
test_crossRcbDuplicateContent_onlyOneOfTwoIdenticalRcbsReachesCallback(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_CROSS_RCB_DEDUP);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_CROSS_RCB_DEDUP,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    CrossRcbDedupTestState state = { 0 };
    MmsReportClient_setReportCallback(client, onReportForCrossRcbDedupTest, &state);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatusForCrossRcbDedupTest, &state);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.brcbMainEnabled), "expected brcbMain to enable");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.brcbDupEnabled), "expected brcbDup to enable");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.urcbDynEnabled), "expected urcbDyn to enable");

    /* Both RCBs' first-ever observations are cache-seed-only and never reach
     * the callback (bootstrap suppression) - this client never requests GI,
     * so this throwaway flip is what seeds both RCBs' independent caches;
     * the real, forwarded change below is what actually produces real,
     * byte-identical DATA_CHANGE reports from each independently-enabled
     * RCB. */
    SimServer_setIndication(sim, true);
    Thread_sleep(200);
    SimServer_setIndication(sim, false);

    TEST_ASSERT_TRUE_MESSAGE(waitUntilAtLeast(&state.brcbMainOrDupReportCount, 1),
            "expected at least one of brcbMain/brcbDup's identical data-change reports to reach the callback");

    /* Generous settle window for a hypothetical duplicate (the bug this test
     * guards against) to also arrive - loopback round-trips are on the order
     * of a few ms, so this is a large safety margin, not a tight race. */
    Thread_sleep(500);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, state.brcbMainOrDupReportCount,
            "brcbMain and brcbDup report byte-identical content (same ds1 dataset) - only one must "
            "reach the callback, the other must be suppressed as a cross-RCB duplicate");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_dataChangeOnServer_triggersReportWithNewValue);
    RUN_TEST(test_authRequired_correctPassword_connectsAndEnablesRcb);
    RUN_TEST(test_authRequired_wrongPassword_neverConnects);
    RUN_TEST(test_dynamicDataset_createdOnEnable_andReportsRealChange);
    RUN_TEST(test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsReseededPreviousValue);
    RUN_TEST(test_crossRcbDuplicateContent_onlyOneOfTwoIdenticalRcbsReachesCallback);

    return UNITY_END();
}
