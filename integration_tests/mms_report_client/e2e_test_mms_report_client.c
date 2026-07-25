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
 *
 * Known gap, documented rather than faked: this fixture's own
 * sim_server.c only ever assigns strictly-increasing EntryIDs (confirmed
 * against the vendored server's own EntryID-assignment logic,
 * third_party_src/libiec61850/src/iec61850/server/mms_mapping/reporting.c),
 * and its buffered-RCB resume is EXCLUSIVE of the EntryID a client resumes
 * from - so a real, non-monotonic/duplicate EntryID redelivery (the bug
 * MmsReportClientUseCases_isEntryIdStale in mms_report_client_report_adapter.c
 * guards against, confirmed only via real-hardware debug logs, never
 * reproducible against this compliant simulator) cannot be fault-injected
 * here without patching the vendored third_party_src server, which would
 * violate this repo's "Don't touch third_party/" Hard Rule. This suite
 * instead proves the guard's pure comparison logic in
 * tests/mms_report_client/test_mms_report_client_usecases.c, and proves
 * HERE (test_entryIdStaleGuard_doesNotSuppressLegitimateMultiEntryBacklog)
 * that the guard is a true no-op against this simulator's own legitimate,
 * strictly-increasing backlog delivery - i.e. it never has a false positive
 * against real, compliant behavior, even though its true-positive path
 * against actual non-monotonic redelivery is only exercised on real hardware.
 */

#define FIXTURE_PATH "fixtures/reporter1.cid"
#define TEST_PORT 10203
#define TEST_PORT_AUTH_CORRECT 10206 /* distinct from every other E2E test's port - see
                                        integration_tests/orchestration's own registry comment */
#define TEST_PORT_AUTH_WRONG 10207
#define TEST_PORT_DYNAMIC_DATASET 10209
#define TEST_PORT_RECONNECT 10210
#define TEST_PORT_CROSS_RCB_DEDUP 10211
#define TEST_PORT_ENTRY_ID_RESUME 10212
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
 * test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsPreservedPreviousValue
 * to observe that a real second connection (not just a re-enable on the same
 * association) actually happened. */
static volatile int connectedCount;

/* Counts successful brcbMain enable events specifically (RptEna set true),
 * for test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsPreservedPreviousValue
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

    /* This client now deterministically requests GI on every enable (see
     * enableOneTarget's own comment) purely to seed the value-diff cache -
     * the GI-triggered snapshot (current live value, false - see
     * sim_server.c) lands on shouldForwardAndUpdateCache's cached==NULL
     * branch just like any other first observation, and must never itself
     * reach the callback. */
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "the GI snapshot requested on enable must never reach the callback - it silently "
            "seeds the cache instead");

    /* Because GI already seeded the cache with the live default (false), this
     * single flip is itself a genuine, immediately-forwarded change - no
     * throwaway warm-up flip needed anymore. */
    SimServer_setIndication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report after flipping GGIO1.Ind1.stVal");

    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/LLN0.BR.brcbMain", lastRcbReference);
    TEST_ASSERT_EQUAL_INT(2, lastEntryCount);
    TEST_ASSERT_TRUE(lastEntryValue);
    TEST_ASSERT_TRUE_MESSAGE((lastReason & IEC61850_REASON_DATA_CHANGE) != 0,
            "expected the report's reason-for-inclusion to include data-change");
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0HasPreviousValue,
            "the GI snapshot's value must surface as this entry's previousValue");
    TEST_ASSERT_FALSE_MESSAGE(lastEntry0PreviousValue,
            "previousValue must be the GI-seeded live default (false), not the flipped-to value");

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
 * This client now deterministically requests GI on every enable, purely to
 * seed the value-diff cache - urcbDyn's GI-triggered snapshot seeds all 6
 * dataset members (cached == NULL for each) and is itself bootstrap-suppressed
 * regardless of its own reason (see shouldForwardAndUpdateCache's own doc
 * comment for why a real-change reason is never trusted to bypass this, even
 * on a first observation - the same real-hardware finding that fixed the
 * flooding bug also rules out narrowly trusting reason bits here). Because GI
 * already seeded the cache with every member's live default, one single flip
 * afterward is itself a genuine, immediately-forwarded change - no throwaway
 * warm-up flip needed. On that flip, only Ind1's own group (stVal + its q/t
 * siblings, dragged along by group-extension) survives - SPCSO1's unrelated
 * group stays suppressed (its own cache was seeded by GI too, and SPCSO1's
 * value never itself changes, so nothing in that group individually qualifies
 * and nothing drags it in). Expected surviving entries: Ind1.stVal, Ind1.q,
 * Ind1.t (3, not all 6 dataset members).
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

    /* urcbDyn's GI-triggered snapshot (all 6 dataset members, live defaults)
     * seeds the cache and must never itself reach the callback. */
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "urcbDyn's GI snapshot must never reach the callback - it silently seeds the cache "
            "for all 6 dataset members instead");

    /* Because GI already seeded the cache, this single flip is itself a
     * genuine, immediately-forwarded change - no throwaway warm-up flip
     * needed. */
    SimServer_setIndication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report from urcbDyn's dynamically-created dataset after flipping Ind1.stVal");

    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1.RP.urcbDyn", lastRcbReference);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, lastEntryCount,
            "only Ind1's own group (stVal + its q/t siblings, dragged along by group-extension) "
            "should survive - SPCSO1's unrelated group stays suppressed (unchanged since the GI seed)");
    TEST_ASSERT_TRUE(lastEntryValue);
    TEST_ASSERT_TRUE_MESSAGE((lastReason & IEC61850_REASON_DATA_CHANGE) != 0,
            "expected the report's reason-for-inclusion to include data-change");
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$Ind1$stVal", lastEntry0Reference);
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0HasPreviousValue,
            "the GI snapshot's value must surface as this entry's previousValue");
    TEST_ASSERT_FALSE_MESSAGE(lastEntry0PreviousValue,
            "previousValue must be the GI-seeded live default (false), not the flipped-to value");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves the value-diff cache's populate-once/preserve-forever design
 * end-to-end: MmsReportClientMemberRefCacheEntry's lastForwardedValues cache
 * is built once at MmsReportClient_start and is never rebuilt OR reset
 * across reconnects (enableOneTarget no longer resets anything on
 * (re-)enable - the reset mechanism this test used to exercise,
 * MmsReportClientUseCases_resetValueDiffCache, has been deleted entirely -
 * see MmsReportClientMemberRefCacheEntry's own doc comment for the full
 * redesign). So a reconnect's own fresh GI snapshot, and this simulator's
 * buffered redelivery of brcbMain's pre-disconnect content, both diff
 * against the REAL, preserved pre-disconnect value - not a wiped-clean one.
 *
 * SimServer_stop/_start operate on the same SimServer (same IedModel/IedServer
 * object - only the TCP listener toggles, see SimServer_stop/_start's own
 * comments), so GGIO1.Ind1.stVal survives the restart unchanged (true, set by
 * the pre-disconnect flip below). This client deterministically requests GI
 * on every enable, including the reconnect's own re-enable. Both the
 * reconnect's GI-triggered snapshot and this simulator's buffered
 * redelivery of brcbMain's pre-disconnect content (tagged DATA_CHANGE even
 * though the value never actually changed) carry the SAME live value (true)
 * that's already sitting in the preserved cache from before the disconnect
 * - so both are ordinary duplicates of a real cached value, suppressed by
 * the everyday value-diff check, regardless of arrival order.
 * shouldForwardAndUpdateCache no longer trusts a DATA_CHANGE reason bit
 * unconditionally (see its own doc comment for the real-hardware finding
 * that proved the old unconditional-trust design unsafe), which is what
 * makes this order-independent guarantee hold. This test flips the value
 * once before disconnecting (a real, GI-seeded-then-forwarded change, since
 * the first connect's own GI snapshot already seeded the pre-disconnect
 * cache with the live default - so the pre-disconnect cache ends up
 * non-default), reconnects, asserts neither the reconnect's GI snapshot nor
 * its buffered redelivery ever reaches the callback (both are duplicates of
 * the preserved value), then flips the value AGAIN after reconnecting and
 * asserts the resulting report's previousValue reflects the REAL preserved
 * pre-disconnect/post-reconnect state (true, correctly carried across the
 * reconnect) rather than being lost to a cache reset - proving the cache
 * was genuinely preserved, not just that nothing crashed.
 */
void
test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsPreservedPreviousValue(void) {
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
            "the first connect's GI-triggered snapshot must never reach the callback - it "
            "silently seeds the cache instead");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, connectedCount, "expected exactly one connection so far");

    /* Generous settle window for a hypothetical extra, redundant enable cycle
     * (the reconnect "storm" bug this guards against - see supervisorLoop's
     * own comment) to also land, before asserting the count is exactly 1. */
    Thread_sleep(500);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, brcbMainEnableCount,
            "expected exactly one enable cycle for brcbMain from the first connect, not a "
            "redundant re-enable storm");

    /* The first connect's own GI snapshot already seeded the cache with the
     * live default (false), so this is this test's only pre-disconnect
     * transition (deliberately just one, matching the buffered RCB's own
     * un-acked backlog staying minimal by disconnect time) AND a genuine,
     * immediately-forwarded change (cache becomes true, non-default) - unlike
     * before GI was re-added, this is no longer a throwaway/suppressed flip. */
    SimServer_setIndication(sim, true);
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a real, GI-seeded-then-forwarded report after this pre-disconnect flip");
    TEST_ASSERT_TRUE(lastEntryValue);
    reportReceived = false;
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

    /* The reconnect's re-enable requests GI (deterministic snapshot) AND this
     * simulator redelivers brcbMain's buffered content (tagged DATA_CHANGE
     * even though the value never actually changed) - both carry the same
     * live value (true), which ALSO matches the value already sitting in the
     * cache from before the disconnect (the cache is populated once and
     * PRESERVED forever now - never reset on reconnect, see
     * MmsReportClientMemberRefCacheEntry's own doc comment). So both the GI
     * snapshot and the buffered redelivery are ordinary duplicates of the
     * real, preserved pre-disconnect value - neither one is a "bootstrap"
     * event and neither ever reaches the callback, regardless of arrival
     * order (shouldForwardAndUpdateCache no longer trusts a reason bit
     * unconditionally, see its own doc comment, and there is no reset-timing
     * race left to worry about since nothing is ever reset). */
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "neither the reconnect's GI snapshot nor its buffered redelivery must ever reach the "
            "callback - the value never actually changed across the reconnect, diffed against the "
            "real preserved pre-disconnect value");

    /* Flip again, now FROM the real preserved pre-disconnect value (true) back to false. */
    SimServer_setIndication(sim, false);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report after flipping GGIO1.Ind1.stVal post-reconnect");
    TEST_ASSERT_FALSE(lastEntryValue);
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0HasPreviousValue,
            "the real, preserved pre-disconnect value must surface as this entry's previousValue - "
            "this is the whole point of never resetting the cache on reconnect");
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0PreviousValue,
            "previousValue must reflect the true pre-disconnect/post-reconnect live value (true), "
            "correctly preserved across the reconnect rather than lost to a cache reset");

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
 * get RptEna+GI on connect (this client now deterministically requests GI on
 * every enable), but each one's GI-triggered snapshot is cache-seed-only and
 * never reaches the callback at all (bootstrap suppression) - so cross-RCB
 * dedup can't be proven against that snapshot. Instead, this test flips
 * GGIO1.Ind1.stVal AFTER both RCBs are enabled: because GI already seeded
 * both RCBs' independent caches with the live default, this single flip is
 * itself a genuine, immediately-forwarded change from each - each
 * independently produces a real, byte-identical DATA_CHANGE report at nearly
 * the same moment, and only one of the two must ever reach the report
 * callback. urcbDyn (also declared in this fixture) reports a different,
 * larger dataset, so it's unaffected and used here only to confirm the
 * client is otherwise fully connected/enabled.
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

    /* Both RCBs' GI-triggered snapshots are cache-seed-only and never reach
     * the callback (bootstrap suppression) - because GI already seeded both
     * RCBs' independent caches with the live default, this single flip is
     * itself what produces real, byte-identical DATA_CHANGE reports from each
     * independently-enabled RCB (no throwaway warm-up flip needed). */
    SimServer_setIndication(sim, true);

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

/*
 * Isolated callback state for test_secondReconnectWithNoNewChanges_doesNotRedeliverBacklog
 * below - counts every brcbMain report received (not just the latest), since
 * this test's whole point is proving a COUNT stays flat across a reconnect
 * with nothing new to report.
 */
typedef struct {
    volatile bool enabled;
    volatile int reportCount;
    volatile bool lastValue;
} EntryIdResumeTestState;

static void
onRcbStatusForEntryIdResumeTest(void* userParam, const char* rcbReference, bool enabled, IedClientError lastError) {
    (void) lastError;
    if (!enabled || !rcbReference) return;
    if (strcmp(rcbReference, "Reporter1LD1/LLN0.BR.brcbMain") != 0) return;

    EntryIdResumeTestState* state = (EntryIdResumeTestState*) userParam;
    state->enabled = true;
}

static void
onReportForEntryIdResumeTest(void* userParam, const MmsReportRecord* record) {
    EntryIdResumeTestState* state = (EntryIdResumeTestState*) userParam;

    if (record->rcbReference && strcmp(record->rcbReference, "Reporter1LD1/LLN0.BR.brcbMain") == 0) {
        state->reportCount++;
        if (record->entryCount > 0 && record->entries[0].value) {
            state->lastValue = MmsValue_getBoolean(record->entries[0].value);
        }
    }

    MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
}

/*
 * Direct regression test for the missing-EntryID-resumption bug (see
 * mms_report_client_connection.c's enableOneTarget - RCB_ELEMENT_ENTRY_ID /
 * ClientReportControlBlock_setEntryId): without it, a buffered RCB's server
 * has no way to know what this client already received, so it redelivers its
 * ENTIRE unacknowledged backlog on every RptEna transition. Confirmed against
 * real hardware: a buffered RCB kept resending the same alternating
 * true/false backlog on every reconnect, defeating the value-diff cache
 * (which only remembers the single last forwarded value - replaying the same
 * multi-entry sequence again looks like fresh changes each time).
 *
 * Reproduces the shape directly: accumulate a multi-entry alternating
 * backlog (true/false/true) WHILE disconnected, reconnect once (the backlog
 * must be delivered - this is real, wanted data), then force a SECOND
 * reconnect with ZERO new changes in between. Without EntryID resumption,
 * the second reconnect would redeliver the SAME already-consumed backlog,
 * producing spurious reports exactly like the real-hardware symptom. With
 * it, the server has nothing new buffered past this client's last
 * acknowledged EntryID, so nothing is redelivered.
 */
void
test_secondReconnectWithNoNewChanges_doesNotRedeliverBacklog(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_ENTRY_ID_RESUME);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_ENTRY_ID_RESUME,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    EntryIdResumeTestState state = { 0 };
    MmsReportClient_setReportCallback(client, onReportForEntryIdResumeTest, &state);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatusForEntryIdResumeTest, &state);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.enabled),
            "expected the first connect to enable brcbMain within the timeout");
    Thread_sleep(300); /* let the GI-seeded bootstrap snapshot settle - never forwarded */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, state.reportCount,
            "the first connect's GI snapshot must never reach the callback");

    SimServer_stop(sim);

    /* Accumulate a multi-entry buffered backlog WHILE disconnected - the same
     * alternating true/false/true shape that exposed the real-hardware bug. */
    SimServer_setIndication(sim, true);
    SimServer_setIndication(sim, false);
    SimServer_setIndication(sim, true);

    state.enabled = false;
    SimServer_start(sim, TEST_PORT_ENTRY_ID_RESUME);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.enabled),
            "expected the reconnect supervisor to reconnect and re-enable brcbMain");

    /* The backlog must actually be delivered here - this is real, wanted
     * data, not something the fix should suppress. */
    TEST_ASSERT_TRUE_MESSAGE(waitUntilAtLeast(&state.reportCount, 1),
            "expected the buffered backlog accumulated while disconnected to be delivered");
    Thread_sleep(500); /* let every backlog entry settle */
    int reportCountAfterFirstReconnect = state.reportCount;
    TEST_ASSERT_TRUE_MESSAGE(state.lastValue, "expected the backlog's final live value to be true");

    /* Second reconnect, with ZERO new changes since the first - the direct
     * regression check. */
    state.enabled = false;
    SimServer_stop(sim);
    SimServer_start(sim, TEST_PORT_ENTRY_ID_RESUME);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.enabled),
            "expected the second reconnect to also re-enable brcbMain");

    /* Generous settle window for a hypothetical spurious redelivery to land. */
    Thread_sleep(1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(reportCountAfterFirstReconnect, state.reportCount,
            "the second reconnect, with no new changes since the first, must not redeliver the "
            "already-consumed backlog - proves RCB_ELEMENT_ENTRY_ID resumption is actually applied "
            "on enable, not just relying on the pre-existing value-diff filter (which alone cannot "
            "catch a full alternating-sequence resend)");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Isolated callback state for
 * test_entryIdStaleGuard_doesNotSuppressLegitimateMultiEntryBacklog below -
 * records every brcbMain report's value IN ORDER (not just a count), since
 * this test's whole point is proving the new
 * MmsReportClientUseCases_isEntryIdStale guard (mms_report_client_report_adapter.c)
 * doesn't drop any entry of a genuinely-increasing-EntryID backlog.
 */
#define ORDERED_VALUES_CAPACITY 16
typedef struct {
    volatile bool enabled;
    volatile int reportCount;
    bool orderedValues[ORDERED_VALUES_CAPACITY];
} EntryIdGuardRegressionTestState;

static void
onRcbStatusForEntryIdGuardRegressionTest(void* userParam, const char* rcbReference, bool enabled,
        IedClientError lastError) {
    (void) lastError;
    if (!enabled || !rcbReference) return;
    if (strcmp(rcbReference, "Reporter1LD1/LLN0.BR.brcbMain") != 0) return;

    EntryIdGuardRegressionTestState* state = (EntryIdGuardRegressionTestState*) userParam;
    state->enabled = true;
}

static void
onReportForEntryIdGuardRegressionTest(void* userParam, const MmsReportRecord* record) {
    EntryIdGuardRegressionTestState* state = (EntryIdGuardRegressionTestState*) userParam;

    if (record->rcbReference && strcmp(record->rcbReference, "Reporter1LD1/LLN0.BR.brcbMain") == 0
            && record->entryCount > 0 && record->entries[0].value) {
        if (state->reportCount < ORDERED_VALUES_CAPACITY) {
            state->orderedValues[state->reportCount] = MmsValue_getBoolean(record->entries[0].value);
        }
        state->reportCount++;
    }

    MmsReportClient_destroyReportRecord((MmsReportRecord*) record);
}

/*
 * Non-regression test for the new EntryID-staleness guard
 * (MmsReportClientUseCases_isEntryIdStale, wired into
 * MmsReportClientReportAdapter_onReport in mms_report_client_report_adapter.c):
 * a real, spec-compliant server only ever assigns strictly-increasing
 * EntryIDs (confirmed by reading its own EntryID-assignment logic), so every
 * entry of a genuinely accumulated backlog must still pass the guard and
 * reach the callback, in order - the guard must only ever reject a
 * non-monotonic/repeated EntryID, which this fixture's compliant simulator
 * structurally cannot produce (see this test file's own header comment on
 * why a true fault-injection test isn't achievable here). This directly
 * complements test_secondReconnectWithNoNewChanges_doesNotRedeliverBacklog
 * above (which proves an EMPTY backlog isn't redelivered) by proving a
 * NON-EMPTY one isn't partially dropped.
 */
void
test_entryIdStaleGuard_doesNotSuppressLegitimateMultiEntryBacklog(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_ENTRY_ID_RESUME);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_ENTRY_ID_RESUME,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    EntryIdGuardRegressionTestState state = { 0 };
    MmsReportClient_setReportCallback(client, onReportForEntryIdGuardRegressionTest, &state);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatusForEntryIdGuardRegressionTest, &state);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.enabled),
            "expected the first connect to enable brcbMain within the timeout");
    Thread_sleep(300); /* let the GI-seeded bootstrap snapshot settle - never forwarded */

    SimServer_stop(sim);

    /* Accumulate a multi-entry buffered backlog WHILE disconnected - each of
     * these three is a genuinely increasing EntryID on this compliant
     * simulator, so all three must survive the new guard. */
    SimServer_setIndication(sim, true);
    SimServer_setIndication(sim, false);
    SimServer_setIndication(sim, true);

    state.enabled = false;
    SimServer_start(sim, TEST_PORT_ENTRY_ID_RESUME);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&state.enabled),
            "expected the reconnect supervisor to reconnect and re-enable brcbMain");

    TEST_ASSERT_TRUE_MESSAGE(waitUntilAtLeast(&state.reportCount, 3),
            "expected all three genuinely accumulated backlog entries to reach the callback - "
            "the new EntryID-staleness guard must not drop any of a real, strictly-increasing sequence");
    Thread_sleep(500); /* let anything further (there should be nothing) settle */

    TEST_ASSERT_EQUAL_INT_MESSAGE(3, state.reportCount,
            "exactly the three genuine backlog entries must be delivered - no fewer (guard "
            "over-rejecting) and no more (guard failing to catch an unrelated duplicate)");
    TEST_ASSERT_TRUE_MESSAGE(state.orderedValues[0], "backlog entry 1 must be true, in order");
    TEST_ASSERT_FALSE_MESSAGE(state.orderedValues[1], "backlog entry 2 must be false, in order");
    TEST_ASSERT_TRUE_MESSAGE(state.orderedValues[2], "backlog entry 3 must be true, in order");

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
    RUN_TEST(test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsPreservedPreviousValue);
    RUN_TEST(test_crossRcbDuplicateContent_onlyOneOfTwoIdenticalRcbsReachesCallback);
    RUN_TEST(test_secondReconnectWithNoNewChanges_doesNotRedeliverBacklog);
    RUN_TEST(test_entryIdStaleGuard_doesNotSuppressLegitimateMultiEntryBacklog);

    return UNITY_END();
}
