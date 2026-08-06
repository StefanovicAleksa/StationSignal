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
#define TEST_PORT_AUTH_REJECTED_CALLBACK 10213
#define TEST_PORT_PULLED_DATASET 10214
#define TEST_PORT_CHUNKING 10215
#define TEST_PORT_BUDGET 10216
#define TEST_PORT_BUFFERED_DYNAMIC_DATASET 10217
#define TEST_PORT_ORPHAN_CLEANUP 10218
#define TEST_PORT_SIBLING_BUFFERED 10219
#define TEST_PORT_GI_ONLY 10220
#define TEST_PORT_DELETE_WHILE_ENABLED 10221
#define TEST_PASSWORD "secret123"
#define FIXTURE_PATH_SIBLING_BUFFERED "fixtures/reporter1_sibling_buffered.cid"
#define FIXTURE_PATH_GI_ONLY "fixtures/reporter1_gi_only.cid"
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

/* brcbDyn's own enable-outcome trackers - the buffered counterpart of
 * urcbDyn (see fixtures/reporter1.cid and sim_server.c's own comments on
 * that RCB), for test_dynamicDataset_bufferedRcb_createdOnEnable_andSurvivesReconnect
 * below to distinguish its outcome from urcbDyn's/brcbMain's own. */
static volatile bool dynamicBufferedRcbEnabled;
static volatile bool dynamicBufferedRcbFailed;

/* urcbDyn2's own enable-outcome trackers - the chunking/budget fixtures
 * (reporter1_chunking.cid / reporter1_budget.cid) declare a second Dyn RCB on
 * GGIO1 alongside urcbDyn, so buildChunkPlan has two spare targets to split
 * an oversized LN's leaf set across. No other fixture in this suite declares
 * urcbDyn2 - see sim_server.c's own comment on that RCB. */
static volatile bool dynamicRcb2Enabled;
static volatile bool dynamicRcb2Failed;

/* brcbDyn2's own enable-outcome trackers - a second BUFFERED Dyn RCB
 * alongside brcbDyn, declared only by fixtures/reporter1_sibling_buffered.cid,
 * for test_siblingBufferedDynRcbs_reconnectDoesNotCrossAdoptEachOthersLeftoverDataset
 * below - see sim_server.c's own comment on that RCB. */
static volatile bool dynamicBufferedRcb2Enabled;
static volatile bool dynamicBufferedRcb2Failed;

/* Per-RCB report captures for urcbDyn/urcbDyn2 specifically, populated
 * unconditionally in onReport (NOT gated by interestedRcbReference) - the
 * chunking test needs to observe both RCBs' own independent report content
 * at once, which interestedRcbReference's single-slot filter can't express. */
static volatile bool urcbDynGotReport;
static volatile bool urcbDyn2GotReport;
static int urcbDynEntryCount;
static int urcbDyn2EntryCount;
static char urcbDynEntry0Reference[256];
static char urcbDyn2Entry0Reference[256];

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

/* Set once onConnState observes MMS_REPORT_CLIENT_CONNECTION_REJECTED - see
 * test_authRequired_wrongPassword_firesConnectionRejectedCallback. */
static volatile bool sawConnectionRejected;

static void
onConnState(void* userParam, MmsReportClientConnState state) {
    (void) userParam;
    if (state == MMS_REPORT_CLIENT_CONNECTED) connectedCount++;
    if (state == MMS_REPORT_CLIENT_CONNECTION_REJECTED) sawConnectionRejected = true;
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

    if (rcbReference && strcmp(rcbReference, "Reporter1LD1/GGIO1.RP.urcbDyn2") == 0) {
        if (enabled) dynamicRcb2Enabled = true;
        else dynamicRcb2Failed = true;
    }

    if (rcbReference && strcmp(rcbReference, "Reporter1LD1/GGIO1.BR.brcbDyn") == 0) {
        if (enabled) dynamicBufferedRcbEnabled = true;
        else dynamicBufferedRcbFailed = true;
    }

    if (rcbReference && strcmp(rcbReference, "Reporter1LD1/GGIO1.BR.brcbDyn2") == 0) {
        if (enabled) dynamicBufferedRcb2Enabled = true;
        else dynamicBufferedRcb2Failed = true;
    }
}

static void
onReport(void* userParam, const MmsReportRecord* record) {
    (void) userParam;

    if (record->rcbReference && strcmp(record->rcbReference, "Reporter1LD1/GGIO1.RP.urcbDyn") == 0) {
        urcbDynEntryCount = record->entryCount;
        if (record->entryCount > 0) {
            strncpy(urcbDynEntry0Reference, record->entries[0].reference ? record->entries[0].reference : "",
                    sizeof(urcbDynEntry0Reference) - 1);
            urcbDynEntry0Reference[sizeof(urcbDynEntry0Reference) - 1] = '\0';
        }
        urcbDynGotReport = true;
    } else if (record->rcbReference && strcmp(record->rcbReference, "Reporter1LD1/GGIO1.RP.urcbDyn2") == 0) {
        urcbDyn2EntryCount = record->entryCount;
        if (record->entryCount > 0) {
            strncpy(urcbDyn2Entry0Reference, record->entries[0].reference ? record->entries[0].reference : "",
                    sizeof(urcbDyn2Entry0Reference) - 1);
            urcbDyn2Entry0Reference[sizeof(urcbDyn2Entry0Reference) - 1] = '\0';
        }
        urcbDyn2GotReport = true;
    }

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
    dynamicRcb2Enabled = false;
    dynamicRcb2Failed = false;
    dynamicBufferedRcbEnabled = false;
    dynamicBufferedRcbFailed = false;
    dynamicBufferedRcb2Enabled = false;
    dynamicBufferedRcb2Failed = false;
    urcbDynGotReport = false;
    urcbDyn2GotReport = false;
    urcbDynEntryCount = 0;
    urcbDyn2EntryCount = 0;
    urcbDynEntry0Reference[0] = '\0';
    urcbDyn2Entry0Reference[0] = '\0';
    connectedCount = 0;
    brcbMainEnableCount = 0;
    sawConnectionRejected = false;
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
 * Proves the MMS_REPORT_CLIENT_CONNECTION_REJECTED connState transition
 * (mms_report_client_connection.c's supervisorLoop) actually fires when the
 * supervisor thread's IedConnection_connect() comes back
 * IED_ERROR_CONNECTION_REJECTED - the mechanism the daemon's per-device
 * CONNECTION_STATUS push (ipc_dispatcher) and, in turn, the API/frontend's
 * "this device may need a password" UX are built on. Same wrong-password
 * harness as test_authRequired_wrongPassword_neverConnects, but observes the
 * connState callback instead of just the RCB status callback.
 */
void
test_authRequired_wrongPassword_firesConnectionRejectedCallback(void) {
    SimServer sim = SimServer_create();
    SimServer_requireAuthentication(sim, TEST_PASSWORD);
    SimServer_start(sim, TEST_PORT_AUTH_REJECTED_CALLBACK);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);
    config.acseAuthPassword = "wrong-password";

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_AUTH_REJECTED_CALLBACK,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    MmsReportClient_setConnectionStateCallback(client, onConnState, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&sawConnectionRejected),
            "expected a rejected connect attempt to fire MMS_REPORT_CLIENT_CONNECTION_REJECTED");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves mms_report_client's dynamic dataset creation
 * (IedConnection_createDataSet, data/mms_report_client_connection.c's
 * getOrCreateDynamicDataset) end-to-end against a real MMS association: the
 * fixture's Dyn RCBs (no datSet attribute at all - datSet="Dyn" in SCL terms)
 * would otherwise fail setRCBValues with IED_ERROR_OBJECT_VALUE_INVALID,
 * exactly as they do against a real device like E13_6MD. Instead,
 * mms_report_client's whole-device clustering (buildWholeDeviceClusterPlan)
 * covers every FC=ST/MX leaf across the ENTIRE device - not just each Dyn
 * RCB's own parent LN - and assigns clusters to Dyn slots in model-
 * declaration order. Since LLN0 is declared before GGIO1 in this fixture, and
 * "urcbDyn" (unbuffered, parented under GGIO1) is the first Dyn slot while
 * "brcbDyn" (buffered, also parented under GGIO1) is the second, the
 * assignment lands: urcbDyn <- LLN0's own cluster (Mod/Beh/Health, 9 leaves),
 * brcbDyn <- GGIO1's own cluster (Ind1.stVal/q/t, SPCSO1.stVal/q/t, 6 leaves)
 * - a real, deterministic consequence of a Dyn RCB's parent LN no longer
 * constraining what it reports on (see buildWholeDeviceClusterPlan's own doc
 * comment). This test exercises the GGIO1 cluster and its real data-change
 * flip via brcbDyn, matching this test's original intent (proving a
 * self-created dataset genuinely reports GGIO1's real content) even though
 * the specific RCB it lands on shifted with this feature.
 *
 * This client now deterministically requests GI on every enable, purely to
 * seed the value-diff cache - brcbDyn's GI-triggered snapshot seeds all 6
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

    strncpy(interestedRcbReference, "Reporter1LD1/GGIO1.BR.brcbDyn", sizeof(interestedRcbReference) - 1);
    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicBufferedRcbEnabled),
            "expected brcbDyn (no SCL-declared datSet) to get a dynamically-created dataset and enable successfully");
    TEST_ASSERT_FALSE_MESSAGE(dynamicBufferedRcbFailed,
            "brcbDyn must not fail after this feature exists - it used to fail with error 32 (OBJECT_VALUE_INVALID)");

    /* brcbDyn's GI-triggered snapshot (all 6 dataset members, live defaults)
     * seeds the cache and must never itself reach the callback. */
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "brcbDyn's GI snapshot must never reach the callback - it silently seeds the cache "
            "for all 6 dataset members instead");

    /* Because GI already seeded the cache, this single flip is itself a
     * genuine, immediately-forwarded change - no throwaway warm-up flip
     * needed. */
    SimServer_setIndication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report from brcbDyn's dynamically-created dataset after flipping Ind1.stVal");

    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1.BR.brcbDyn", lastRcbReference);
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
 * Proves the fix for the buffered-RCB dynamic-dataset bug confirmed against a
 * real SIPROTEC 6MD device and documented in GAP3_DYNAMIC_DATASET_NOTES.md:
 * the fixture's "brcbDyn" (parented under GGIO1, buffered="true", no datSet
 * attribute at all - datSet="Dyn" in SCL terms) used to fail setRCBValues
 * with IED_ERROR_OBJECT_VALUE_INVALID/32, exactly like urcbDyn once did,
 * because getOrCreateDynamicDataset generated an association-scoped
 * ("@"-prefixed) name regardless of buffered-ness. Both the vendored
 * reference server (mms_mapping/reporting.c's updateReportDataset) and a real
 * device reject assigning an association-scoped dataset to a buffered RCB
 * outright - that dataset is destroyed the instant this connection closes,
 * defeating the entire point of a buffered RCB surviving one.
 * getOrCreateDynamicDataset now builds a domain/VMD-scoped name for a
 * buffered target instead (buildDynamicDatasetName), which persists past
 * this connection.
 *
 * Also proves the resulting dataset is correctly reused across a reconnect:
 * since the name is domain-scoped (not auto-destroyed on disconnect like the
 * "@"-scoped ones) and deterministic, the SECOND connect's own
 * getOrCreateDynamicDataset call reaches IedConnection_createDataSet again
 * for the exact same name, which the server now answers with
 * IED_ERROR_OBJECT_EXISTS - createAndCacheDynamicDataset's own reuse branch
 * must treat that as success, not fail brcbDyn's re-enable.
 *
 * Deliberately does NOT assert a flipped value reaches the report callback
 * (unlike test_dynamicDataset_createdOnEnable_andReportsRealChange above):
 * brcbDyn and urcbDyn are both Dyn RCBs on the very same LN (GGIO1), so they
 * always cover the identical leaf set and always report byte-identical
 * content for the same underlying change - MmsReportClientUseCases_shouldForwardAcrossRcb's
 * cross-RCB dedup (proven separately in
 * test_crossRcbDuplicateContent_onlyOneOfTwoIdenticalRcbsReachesCallback)
 * correctly drops brcbDyn's own copy as a duplicate of urcbDyn's, since
 * urcbDyn's own GI-then-flip cycle always reaches the callback first. That's
 * correct, expected behavior, not something this test should fight - proving
 * per-report delivery for a Dyn dataset is already this suite's other test's
 * job; this test's job is proving the dataset itself can be created,
 * assigned, and reused for a BUFFERED target at all.
 */
void
test_dynamicDataset_bufferedRcb_createdOnEnable_andSurvivesReconnect(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_BUFFERED_DYNAMIC_DATASET);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1",
            TEST_PORT_BUFFERED_DYNAMIC_DATASET, &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    strncpy(interestedRcbReference, "Reporter1LD1/GGIO1.BR.brcbDyn", sizeof(interestedRcbReference) - 1);
    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);
    MmsReportClient_setConnectionStateCallback(client, onConnState, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicBufferedRcbEnabled),
            "expected brcbDyn (buffered, no SCL-declared datSet) to get a domain-scoped "
            "dynamically-created dataset and enable successfully");
    TEST_ASSERT_FALSE_MESSAGE(dynamicBufferedRcbFailed,
            "brcbDyn must not fail with error 32 (OBJECT_VALUE_INVALID) - an association-scoped "
            "dataset must never be attempted for a buffered RCB");

    /* brcbDyn's GI-triggered snapshot seeds the cache and must never itself
     * reach the callback - same bootstrap-suppression rule as every other
     * RCB in this suite. */
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "brcbDyn's GI snapshot must never reach the callback - it silently seeds the cache "
            "instead");

    dynamicBufferedRcbEnabled = false;

    /* Reconnect: the domain-scoped dataset created during the first connect
     * is still sitting on the server (unlike an "@"-scoped one, it was NOT
     * auto-destroyed when that first association closed - SimServer_stop
     * only stops listening, it doesn't destroy the underlying IedServer/model,
     * matching a real device across a client-side disconnect/reconnect). The
     * second connect's own getOrCreateDynamicDataset call must gracefully
     * reuse it (IED_ERROR_OBJECT_EXISTS) rather than failing. */
    SimServer_stop(sim);
    SimServer_start(sim, TEST_PORT_BUFFERED_DYNAMIC_DATASET);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicBufferedRcbEnabled),
            "expected the reconnect to re-enable brcbDyn by reusing its already-existing "
            "domain-scoped dataset, not fail trying to recreate it");
    TEST_ASSERT_FALSE_MESSAGE(dynamicBufferedRcbFailed,
            "brcbDyn's reconnect must not fail - IED_ERROR_OBJECT_EXISTS must be treated as a "
            "successful reuse, not an error");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves proactive orphan cleanup (cleanupOrphanedDatasets, called at the end
 * of every enableAllTargets): a domain-scoped dataset matching a real
 * buffered target's own deterministic name (buildDynamicDatasetName), but
 * left genuinely unclaimed this cycle, gets deleted to reclaim budget -
 * closing the real-world gap an ungracefully-terminated prior run leaves
 * behind (MmsReportClientConnection_stop's own cleanup-on-stop never runs if
 * the daemon is killed/crashes instead of going through STOP_REPORTING).
 *
 * IMPORTANT - why brcbDyn is given a LIVE-PREASSIGNED dataset here (this
 * setup step did not exist before adoptUnclaimedDataset gained a
 * preferred-own-name pass): a buffered target now checks for - and adopts -
 * a dataset matching ITS OWN exact deterministic name before adoptUnclaimedDataset's
 * general LD-wide scan even runs (see that function's own doc comment) -
 * correct, since a client restarting shouldn't blow away and recreate a
 * dataset that already exactly matches what it would create anyway. That
 * means brcbDyn's own exact-name leftover below can no longer be left
 * "genuinely unclaimed" via adoption - brcbDyn would just adopt it back
 * immediately. To still exercise a REAL orphan (a leftover matching this
 * target's naming convention that the daemon never even looks at), this test
 * instead pre-assigns a DIFFERENT, live dataset directly to brcbDyn's own RCB
 * DatSet attribute (mirroring test_pulledLiveDataset_preAssignedByAnotherClient_reusedInsteadOfSelfCreated's
 * own setup pattern) - tier 2 (pullLiveDataset) then satisfies brcbDyn
 * immediately, so adoptUnclaimedDataset is never even called for it, leaving
 * its own-exact-name leftover dataset genuinely untouched and orphaned.
 *
 * Pre-seeds five datasets via a side-channel connection. Which candidate
 * urcbDyn (unaffected by the buffered-only own-name preference above) adopts
 * is governed by discoverExistingServerDatasets'/adoptUnclaimedDataset's own
 * list order, which empirically follows the vendored reference server's own
 * IedConnection_getLogicalDeviceDataSets enumeration - observed (here and in
 * every other test in this suite exercising adoption) to return existing
 * datasets in ASCII-lexicographic order by full "LD/item" name, not creation
 * order.
 *   1/2. "AAA_decoy1"/"AAB_decoy2" - throwaway 1-member datasets, sorting
 *      before everything else under this LD (including the shared
 *      simulator's own pre-existing "ds1"/"ds2", both "LLN0$..." - 'G' <
 *      'L') - urcbDyn adopts decoy1 (first in list order); decoy2 is simply
 *      left unclaimed and untouched (it doesn't match any real target's own
 *      deterministic name, so cleanupOrphanedDatasets leaves it alone too -
 *      incidental extra coverage of "an unclaimed, non-matching leftover is
 *      never touched," alongside "foreign" below).
 *   3. "livepreassigned" - domain-scoped, directly assigned to brcbDyn's own
 *      RCB DatSet attribute below - satisfies brcbDyn via tier 2, so it never
 *      reaches adoptUnclaimedDataset at all.
 *   4. brcbDyn's own exact deterministic name -
 *      buildDynamicDatasetName("Reporter1LD1/GGIO1.BR.brcbDyn", true) =
 *      "Reporter1LD1/GGIO1$BR$brcbDyn$dyn" - matches OUR OWN naming
 *      convention exactly, standing in for a leftover from an earlier,
 *      ungracefully-terminated run of this very client. Genuinely unclaimed
 *      this cycle (brcbDyn itself never looks at it, satisfied by
 *      "livepreassigned" via tier 2 instead) - the case cleanupOrphanedDatasets
 *      exists to catch.
 *   5. "foreign" - a dataset that does NOT match any real target's own
 *      deterministic name at all - must be left completely untouched no
 *      matter what (cleanupOrphanedDatasets never deletes anything it can't
 *      prove is ours), whether or not it happens to get adopted too.
 *
 * Verified via a THIRD side-channel connection, opened only after the real
 * client has fully connected/enabled (cleanup already ran synchronously at
 * the end of that same enableAllTargets call, before either RCB's status
 * callback could even fire): brcbDyn's own exact-match dataset must be GONE
 * (IedConnection_getDataSetDirectory fails), while "foreign" must still
 * resolve successfully.
 */
void
test_orphanCleanup_ownUnclaimedDatasetDeleted_foreignDatasetLeftUntouched(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_ORPHAN_CLEANUP);

    IedConnection setupConn = IedConnection_create();
    IedClientError setupErr = IED_ERROR_OK;
    IedConnection_connect(setupConn, &setupErr, "127.0.0.1", TEST_PORT_ORPHAN_CLEANUP);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected the setup connection to associate");

    LinkedList decoy1Members = LinkedList_create();
    LinkedList_add(decoy1Members, (void*) "Reporter1LD1/GGIO1.SPCSO1.stVal[ST]");
    IedConnection_createDataSet(setupConn, &setupErr, "Reporter1LD1/GGIO1$AAA_decoy1", decoy1Members);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected decoy1 to be created");
    LinkedList_destroyStatic(decoy1Members);

    LinkedList decoy2Members = LinkedList_create();
    LinkedList_add(decoy2Members, (void*) "Reporter1LD1/GGIO1.SPCSO1.q[ST]");
    IedConnection_createDataSet(setupConn, &setupErr, "Reporter1LD1/GGIO1$AAB_decoy2", decoy2Members);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected decoy2 to be created");
    LinkedList_destroyStatic(decoy2Members);

    /* Live-preassign a DIFFERENT dataset directly to brcbDyn's own RCB DatSet
     * attribute, so tier 2 (pullLiveDataset) satisfies it immediately and it
     * never reaches adoptUnclaimedDataset at all - see this test's own doc
     * comment on why this step is required now that a buffered target
     * prefers adopting its own exact-name dataset first. */
    LinkedList livePreassignedMembers = LinkedList_create();
    LinkedList_add(livePreassignedMembers, (void*) "Reporter1LD1/GGIO1.SPCSO1.q[ST]");
    IedConnection_createDataSet(setupConn, &setupErr, "Reporter1LD1/GGIO1.livepreassigned", livePreassignedMembers);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected brcbDyn's live-preassigned dataset to be created");
    LinkedList_destroyStatic(livePreassignedMembers);

    ClientReportControlBlock brcbDynSetupRcb = IedConnection_getRCBValues(setupConn, &setupErr,
            "Reporter1LD1/GGIO1.BR.brcbDyn", NULL);
    TEST_ASSERT_NOT_NULL(brcbDynSetupRcb);
    ClientReportControlBlock_setDataSetReference(brcbDynSetupRcb, "Reporter1LD1/GGIO1$livepreassigned");
    IedConnection_setRCBValues(setupConn, &setupErr, brcbDynSetupRcb, RCB_ELEMENT_DATSET, true);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected assigning brcbDyn's DatSet to succeed");
    ClientReportControlBlock_destroy(brcbDynSetupRcb);

    LinkedList ownNameMembers = LinkedList_create();
    LinkedList_add(ownNameMembers, (void*) "Reporter1LD1/GGIO1.SPCSO1.q[ST]");
    IedConnection_createDataSet(setupConn, &setupErr, "Reporter1LD1/GGIO1$BR$brcbDyn$dyn", ownNameMembers);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected brcbDyn's own exact-name dataset to be created");
    LinkedList_destroyStatic(ownNameMembers);

    LinkedList foreignMembers = LinkedList_create();
    LinkedList_add(foreignMembers, (void*) "Reporter1LD1/GGIO1.Ind1.t[ST]");
    IedConnection_createDataSet(setupConn, &setupErr, "Reporter1LD1/GGIO1$foreign", foreignMembers);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected the foreign dataset to be created");
    LinkedList_destroyStatic(foreignMembers);

    IedConnection_close(setupConn);
    IedConnection_destroy(setupConn);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_ORPHAN_CLEANUP,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicBufferedRcbEnabled),
            "expected brcbDyn to enable successfully (via whichever tier - adoption or self-create)");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicRcbEnabled), "expected urcbDyn to enable successfully too");

    /* Settle margin only - cleanupOrphanedDatasets already ran synchronously
     * before either enable callback above could even fire; this just avoids
     * any race with this test's own verification connection opening before
     * the daemon's delete call has been fully acknowledged server-side. */
    Thread_sleep(300);

    IedConnection verifyConn = IedConnection_create();
    IedClientError verifyErr = IED_ERROR_OK;
    IedConnection_connect(verifyConn, &verifyErr, "127.0.0.1", TEST_PORT_ORPHAN_CLEANUP);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, verifyErr, "expected the verification connection to associate");

    bool ownNameIsDeletable = false;
    LinkedList ownNameDirectory = IedConnection_getDataSetDirectory(verifyConn, &verifyErr,
            "Reporter1LD1/GGIO1$BR$brcbDyn$dyn", &ownNameIsDeletable);
    TEST_ASSERT_NULL_MESSAGE(ownNameDirectory,
            "brcbDyn's own exact-name dataset was genuinely unclaimed this cycle (an earlier candidate "
            "satisfied brcbDyn's own adoption need first) - cleanupOrphanedDatasets must have deleted "
            "it to reclaim budget");
    if (ownNameDirectory) LinkedList_destroyDeep(ownNameDirectory, free);

    bool foreignIsDeletable = false;
    verifyErr = IED_ERROR_OK;
    LinkedList foreignDirectory = IedConnection_getDataSetDirectory(verifyConn, &verifyErr,
            "Reporter1LD1/GGIO1$foreign", &foreignIsDeletable);
    TEST_ASSERT_NOT_NULL_MESSAGE(foreignDirectory,
            "the foreign dataset (matching no real target's own deterministic name) must be left "
            "completely untouched, whether or not any target happened to adopt it");
    if (foreignDirectory) LinkedList_destroyDeep(foreignDirectory, free);

    IedConnection_close(verifyConn);
    IedConnection_destroy(verifyConn);

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Regression test for adoptUnclaimedDataset's preferred-own-name pass
 * (mms_report_client_connection.c) - confirmed by actually reverting that
 * fix (restoring adoptUnclaimedDataset's plain, unpreferenced LD-wide scan)
 * and observing this exact assertion fail, then restoring the fix and
 * observing it pass again. Pre-fix, tier 3 adopts the FIRST unclaimed
 * candidate under a target's own LD in server enumeration order (empirically,
 * ASCII-lexicographic by full dataset name for this vendored reference
 * server - see test_orphanCleanup_ownUnclaimedDatasetDeleted_foreignDatasetLeftUntouched's
 * own comment), with no preference for a candidate matching the target's OWN
 * deterministic name. So two buffered Dyn targets sharing one LD, each with a
 * pre-existing leftover dataset matching its OWN naming convention (standing
 * in for leftovers from an earlier, ungracefully-terminated run - the same
 * scenario that test models for cleanup), can end up cross-adopting EACH
 * OTHER's leftover instead of their own, whenever enumeration order and
 * target-processing order (SCL declaration order) disagree about which of
 * the two names comes "first." Each RCB then actively reports on the WRONG
 * dataset, covering the wrong attributes entirely - real value changes on
 * the attribute it's actually supposed to cover never reach the frontend,
 * which is exactly the reported symptom.
 *
 * Uses fixtures/reporter1_sibling_buffered.cid (brcbDyn + brcbDyn2, both
 * buffered Dyn RCBs on GGIO1 - the shared reporter1.cid only declares one).
 * brcbDyn2 is declared - and therefore processed - BEFORE brcbDyn there,
 * deliberately the REVERSE of their alphabetical dataset-name order
 * ("...$BR$brcbDyn$dyn" sorts before "...$BR$brcbDyn2$dyn" - '$' < '2'), so
 * enumeration order and processing order disagree on purpose. Sequence:
 *   1. Before the daemon ever connects, pre-seed (via a side-channel
 *      connection) brcbDyn2's own exact deterministic name (1 member: Ind1.t)
 *      and brcbDyn's own exact deterministic name (1 member: SPCSO1.stVal) -
 *      standing in for leftovers from an earlier run.
 *   2. Connect: brcbDyn2 is processed first. With the fix, its own-name-
 *      preference pass finds and adopts ITS OWN leftover (Ind1.t) directly,
 *      regardless of general scan order - brcbDyn then correctly gets its
 *      own leftover (SPCSO1.stVal) too. Pre-fix, brcbDyn2's plain general
 *      scan finds brcbDyn's own name first (sorts before its own
 *      alphabetically) and wrongly adopts it - brcbDyn is then left to adopt
 *      brcbDyn2's own leftover (Ind1.t) instead, having been cross-swapped.
 *   3. The actual end-user-visible assertion: a real SPCSO1 flip must reach
 *      the callback via brcbDyn - this only holds if brcbDyn is using its
 *      own leftover (SPCSO1.stVal). Under the bug, brcbDyn is using
 *      brcbDyn2's leftover (Ind1.t only), so this flip never reaches it at
 *      all.
 */
void
test_siblingBufferedDynRcbs_reconnectDoesNotCrossAdoptEachOthersLeftoverDataset(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_SIBLING_BUFFERED);

    IedConnection sideConn = IedConnection_create();
    IedClientError sideErr = IED_ERROR_OK;
    IedConnection_connect(sideConn, &sideErr, "127.0.0.1", TEST_PORT_SIBLING_BUFFERED);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, sideErr, "expected the side-channel connection to associate");

    /* brcbDyn2's own leftover, created FIRST - deliberately does NOT cover
     * SPCSO1, so a wrongful cross-adoption by brcbDyn is observable. */
    LinkedList brcbDyn2LeftoverMembers = LinkedList_create();
    LinkedList_add(brcbDyn2LeftoverMembers, (void*) "Reporter1LD1/GGIO1.Ind1.t[ST]");
    IedConnection_createDataSet(sideConn, &sideErr, "Reporter1LD1/GGIO1$BR$brcbDyn2$dyn", brcbDyn2LeftoverMembers);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, sideErr, "expected brcbDyn2's leftover dataset to be created");
    LinkedList_destroyStatic(brcbDyn2LeftoverMembers);

    /* brcbDyn's own leftover, created SECOND - covers SPCSO1.stVal, the
     * attribute this test flips to prove brcbDyn ends up using it. */
    LinkedList brcbDynLeftoverMembers = LinkedList_create();
    LinkedList_add(brcbDynLeftoverMembers, (void*) "Reporter1LD1/GGIO1.SPCSO1.stVal[ST]");
    IedConnection_createDataSet(sideConn, &sideErr, "Reporter1LD1/GGIO1$BR$brcbDyn$dyn", brcbDynLeftoverMembers);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, sideErr, "expected brcbDyn's leftover dataset to be created");
    LinkedList_destroyStatic(brcbDynLeftoverMembers);

    IedConnection_close(sideConn);
    IedConnection_destroy(sideConn);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH_SIBLING_BUFFERED, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1_sibling_buffered.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1",
            TEST_PORT_SIBLING_BUFFERED, &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    strncpy(interestedRcbReference, "Reporter1LD1/GGIO1.BR.brcbDyn", sizeof(interestedRcbReference) - 1);
    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicBufferedRcbEnabled), "expected brcbDyn to enable successfully");
    TEST_ASSERT_FALSE_MESSAGE(dynamicBufferedRcbFailed, "brcbDyn must not fail to enable");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicBufferedRcb2Enabled), "expected brcbDyn2 to enable successfully too");
    TEST_ASSERT_FALSE_MESSAGE(dynamicBufferedRcb2Failed, "brcbDyn2 must not fail to enable");

    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "brcbDyn's GI snapshot must never reach the callback - it silently seeds the cache instead");

    /* The actual end-user-visible assertion: only reaches brcbDyn's callback
     * if brcbDyn is genuinely using its OWN leftover (SPCSO1.stVal), not
     * brcbDyn2's (Ind1.t only). */
    SimServer_setSpcso1Indication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report from brcbDyn after flipping SPCSO1.stVal - if brcbDyn wrongly cross-adopted "
            "brcbDyn2's own leftover dataset instead of its own (the reported bug), SPCSO1 would no longer "
            "even be part of brcbDyn's dataset, so no report would ever arrive - exactly like every report "
            "in the originally reported log capture");
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1.BR.brcbDyn", lastRcbReference);
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$SPCSO1$stVal", lastEntry0Reference);

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Regression test for mms_report_client's proactive TrgOps.dchg/qchg/gi fix
 * (mms_report_client_connection.c's enableOneTarget) - confirmed by actually
 * reverting the fix (restoring the plain getRCBValues-only enable, no
 * TrgOps write) and observing this exact assertion fail (report never
 * arrives at all), then restoring the fix and observing it pass again.
 *
 * urcbGiOnly's live TrgOps (set server-side in sim_server.c's own
 * ReportControlBlock_create call, NOT derivable from SCL) is deliberately
 * just TRG_OPT_GI - reproducing the real-device finding (via OMICRON IED
 * Scout's "Simulate IED" feature) that motivated this fix: an RCB configured
 * this way will send its one-time GI snapshot on enable (correctly
 * bootstrap-suppressed, same as every other RCB in this suite) and then
 * NEVER send anything else, no matter what changes - not a client-side
 * filtering problem, the server genuinely never generates the report at
 * all, so previously there was nothing for this feature's own reporting/
 * logging to even see.
 *
 * Pre-fix, enableOneTarget only ever read the RCB's own current TrgOps to
 * decide nothing - it never wrote TrgOps back, so urcbGiOnly's server-side
 * dchg/qchg would stay off forever, and a flip after enable would never
 * produce a report. Post-fix, enableOneTarget ORs TRG_OPT_DATA_CHANGED |
 * TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI into whatever TrgOps the device
 * already has (never clearing anything already on) and writes it back if
 * any bit was missing - so this same RCB now genuinely reports the flip.
 */
void
test_dynamicDataset_giOnlyRcb_reportsRealChangeAfterTrgOpsFix(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_GI_ONLY);

    /* urcbGiOnly's datSet="ds3" deliberately doesn't exist on a fresh
     * SimServer instance (see sim_server.c's own comment on why ds3 isn't
     * created in buildModel itself) - create it here, scoped to this test's
     * own SimServer instance only, before the daemon's own client ever
     * connects. */
    IedConnection sideConn = IedConnection_create();
    IedClientError sideErr = IED_ERROR_OK;
    IedConnection_connect(sideConn, &sideErr, "127.0.0.1", TEST_PORT_GI_ONLY);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, sideErr, "expected the side-channel connection to associate");

    LinkedList ds3Members = LinkedList_create();
    LinkedList_add(ds3Members, (void*) "Reporter1LD1/GGIO1.SPCSO1.stVal[ST]");
    IedConnection_createDataSet(sideConn, &sideErr, "Reporter1LD1/LLN0$ds3", ds3Members);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, sideErr, "expected ds3 to be created");
    LinkedList_destroyStatic(ds3Members);

    IedConnection_close(sideConn);
    IedConnection_destroy(sideConn);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH_GI_ONLY, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1_gi_only.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_GI_ONLY,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&rcbEnabled), "expected urcbGiOnly to enable successfully");

    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "urcbGiOnly's GI snapshot must never reach the callback - it silently seeds the cache instead");

    /* The actual end-user-visible symptom this fix addresses: a real value
     * change must reach the callback even though the server's own TrgOps
     * was configured with only GI - if the daemon never proactively OR'd in
     * dchg/qchg, this flip would never even be sent by the server, let alone
     * reach this callback. */
    SimServer_setSpcso1Indication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report after flipping SPCSO1.stVal on a GI-only-configured RCB - if the daemon "
            "never proactively enabled TrgOps.dchg/qchg (the fixed bug), the server would never generate "
            "this report at all, exactly like the originally reported symptom");
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/LLN0.RP.urcbGiOnly", lastRcbReference);
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$SPCSO1$stVal", lastEntry0Reference);
    TEST_ASSERT_TRUE_MESSAGE(lastEntry0HasPreviousValue, "expected a real previousValue on this ordinary change");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves tier 2 of the three-tier static -> pull live -> self-create dataset
 * resolution order (mms_report_client_connection.c's enableOneTarget/
 * pullLiveDataset): a dataset already assigned to urcbDyn's DatSet attribute
 * by a DIFFERENT, already-disconnected client - standing in for a real
 * commissioning/engineering tool (e.g. Siemens DIGSI) that assigns a dataset
 * out-of-band during substation engineering, independent of whatever client
 * connects later - is reused as-is, instead of the daemon synthesizing its
 * own association-scoped one via getOrCreateDynamicDataset.
 *
 * A second, short-lived IedConnection opens directly against the same
 * simulator, creates a PERSISTENT (domain-scoped, NOT "@"-prefixed - so it
 * deliberately outlives that connection, unlike getOrCreateDynamicDataset's
 * own association-scoped datasets) dataset covering GGIO1.SPCSO1.stVal and
 * GGIO1.SPCSO1.q - a small, deliberately different subset than
 * getOrCreateDynamicDataset's own LN-wide fallback, which would cover all 6
 * FC=ST leaves under GGIO1 across both Ind1 AND SPCSO1 (see
 * test_dynamicDataset_createdOnEnable_andReportsRealChange's own comment).
 * Assigns it to urcbDyn's DatSet, then closes - proving persistence isn't
 * tied to the creator staying connected. Only THEN does the daemon's own
 * MmsReportClientHandle connect.
 *
 * Deliberately uses SPCSO1 (flipped via SimServer_setSpcso1Indication), NOT
 * Ind1 (SimServer_setIndication): brcbMain/brcbDup (always enabled alongside
 * urcbDyn in this fixture) already report Ind1.stVal+Ind1.q via their own
 * "ds1" dataset, and MmsReportClientUseCases_shouldForwardAcrossRcb's
 * cross-RCB duplicate-content suppression (proven in
 * test_crossRcbDuplicateContent_onlyOneOfTwoIdenticalRcbsReachesCallback)
 * would otherwise legitimately treat urcbDyn's own Ind1 report as an
 * exact-content duplicate of what brcbMain/brcbDup just forwarded a moment
 * earlier - not a bug in the tier-2 pull logic this test targets, but a
 * confound worth avoiding by construction. SPCSO1 is not a member of any
 * other RCB's dataset in this fixture, so its content can never collide.
 *
 * The load-bearing assertion is the survivor count on the post-GI flip: 2
 * (stVal + its quality sibling q, dragged along by group-extension - the
 * same mechanism proven in test_dynamicDataset_createdOnEnable_andReportsRealChange),
 * not urcbDyn's usual self-created-dataset survivor count of 3 (stVal+q+t) -
 * proving the daemon decoded against the PULLED dataset's real (smaller,
 * t-less) shape, not the LN-wide fallback shape it would have used had it
 * created its own.
 */
void
test_pulledLiveDataset_preAssignedByAnotherClient_reusedInsteadOfSelfCreated(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_PULLED_DATASET);

    /* ---- Pre-assign a persistent dataset to urcbDyn via a second,
     * short-lived client - standing in for an out-of-band commissioning tool
     * (e.g. DIGSI) - closed BEFORE the daemon's own client ever connects. */
    IedConnection setupConn = IedConnection_create();
    IedClientError setupErr = IED_ERROR_OK;
    IedConnection_connect(setupConn, &setupErr, "127.0.0.1", TEST_PORT_PULLED_DATASET);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected the setup connection to associate");

    LinkedList members = LinkedList_create();
    LinkedList_add(members, (void*) "Reporter1LD1/GGIO1.SPCSO1.stVal[ST]");
    LinkedList_add(members, (void*) "Reporter1LD1/GGIO1.SPCSO1.q[ST]");
    /* Domain-scoped (permanent) form per IedConnection_createDataSet's own
     * doc comment: "LDName/LNodeName.dataSetName" - deliberately NOT the
     * "@"-prefixed association-scoped form getOrCreateDynamicDataset uses. */
    IedConnection_createDataSet(setupConn, &setupErr, "Reporter1LD1/GGIO1.preassigned1", members);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected the pre-assigned dataset to be created");
    LinkedList_destroyStatic(members); /* elements are string literals, not owned copies */

    ClientReportControlBlock setupRcb = IedConnection_getRCBValues(setupConn, &setupErr,
            "Reporter1LD1/GGIO1.RP.urcbDyn", NULL);
    TEST_ASSERT_NOT_NULL(setupRcb);
    /* RCB DatSet attribute form per ClientReportControlBlock_setDataSetReference's
     * own doc comment: "LDName/LNName$DataSetName" - this codebase's own
     * standard "$"-joined convention, NOT the dot-joined form createDataSet
     * itself just took above. */
    ClientReportControlBlock_setDataSetReference(setupRcb, "Reporter1LD1/GGIO1$preassigned1");
    IedConnection_setRCBValues(setupConn, &setupErr, setupRcb, RCB_ELEMENT_DATSET, true);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, setupErr, "expected assigning urcbDyn's DatSet to succeed");
    ClientReportControlBlock_destroy(setupRcb);

    IedConnection_close(setupConn);
    IedConnection_destroy(setupConn);

    /* ---- Now the daemon's own client connects and must find + reuse the
     * dataset the setup connection left behind. ---- */
    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile(FIXTURE_PATH, "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_PULLED_DATASET,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    strncpy(interestedRcbReference, "Reporter1LD1/GGIO1.RP.urcbDyn", sizeof(interestedRcbReference) - 1);
    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicRcbEnabled),
            "expected urcbDyn to enable successfully against the pre-assigned pulled dataset");
    TEST_ASSERT_FALSE_MESSAGE(dynamicRcbFailed, "urcbDyn must not fail when a live dataset is already assigned");

    /* GI-triggered snapshot (2 members - the pulled dataset's own shape, not
     * the LN-wide fallback's 6) seeds the cache and must never itself reach
     * the callback. */
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&reportReceived),
            "urcbDyn's GI snapshot must never reach the callback - it silently seeds the cache instead");

    SimServer_setSpcso1Indication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&reportReceived),
            "expected a report from the pulled dataset after flipping SPCSO1.stVal");

    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1.RP.urcbDyn", lastRcbReference);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, lastEntryCount,
            "the pulled dataset only has stVal+q (no t) - a count of 3 here would mean the daemon "
            "self-created its own LN-wide dataset instead of reusing the pulled one");
    TEST_ASSERT_TRUE(lastEntryValue);
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$SPCSO1$stVal", lastEntry0Reference);
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$SPCSO1$q", lastEntry1Reference);

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

/*
 * Proves DO-grouped chunking (buildWholeDeviceClusterPlan/getOrCreateDynamicDataset,
 * mms_report_client_connection.c): fixtures/reporter1_chunking.cid declares
 * <Services><DynDataSet max="10" maxAttributes="3"/></Services>, so the
 * WHOLE device's 15-leaf set (LLN0's Mod/Beh/Health, 9 leaves, walked before
 * GGIO1's Ind1/SPCSO1, 6 leaves, per LD/LN declaration order) is packed into
 * five 3-member DO-atomic chunks - Mod, Beh, Health, Ind1, SPCSO1, in that
 * order, and buildWholeDeviceClusterPlan assigns cluster i to Dyn slot i
 * (urcbDyn, urcbDyn2, both unbuffered, in that declaration order) - so urcbDyn
 * is PLANNED to get LLN0's Mod chunk and urcbDyn2 LLN0's Beh chunk.
 *
 * But tier "adopt" runs BEFORE tier "self-create" for every target (see
 * enableOneTarget's own doc comment - "primarily try to use existing/foreign
 * datasets and create our own only via necessity"), and the shared simulator
 * always has a pre-existing, SCL-unclaimed domain-scoped dataset ("ds2",
 * GGIO1.Ind1.stVal only) sitting on it regardless of which fixture the
 * client loads. urcbDyn (processed first) adopts THAT instead of ever
 * reaching its own planned Mod chunk - a real, correct consequence of
 * adoption taking priority, not a bug; the Mod chunk simply goes unused this
 * cycle. urcbDyn2 (processed second, after ds2 is already claimed and ds1 is
 * excluded as brcbMain's own SCL-known dataset) finds no adoption candidate
 * left and falls through to its own planned Beh chunk exactly as designed.
 * This test exercises both paths: urcbDyn proves adoption pre-empting a
 * planned cluster, urcbDyn2 proves the chunking/assignment mechanism itself.
 * The simulator's own device-side caps (SimServer_createWithDatasetLimits)
 * are configured generously (would happily accept every chunk as one large
 * dataset too) so any failure here is provably ours, not the device's.
 */
void
test_dynamicDataset_maxAttributesExceeded_chunksOntoSpareRcbInstances(void) {
    SimServer sim = SimServer_createWithDatasetLimits(100, 10);
    SimServer_start(sim, TEST_PORT_CHUNKING);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile("fixtures/reporter1_chunking.cid", "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1_chunking.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_CHUNKING,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    MmsReportClient_setReportCallback(client, onReport, NULL);
    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicRcbEnabled),
            "expected urcbDyn to get its own chunked dataset and enable successfully");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicRcb2Enabled),
            "expected urcbDyn2 to get its own chunked dataset and enable successfully");
    TEST_ASSERT_FALSE_MESSAGE(dynamicRcbFailed, "urcbDyn must not fail when a small maxAttributes forces chunking");
    TEST_ASSERT_FALSE_MESSAGE(dynamicRcb2Failed, "urcbDyn2 must not fail when a small maxAttributes forces chunking");

    /* Each RCB's own GI-triggered snapshot seeds its own value-diff cache and
     * must never itself reach the callback - mirrors
     * test_dynamicDataset_createdOnEnable_andReportsRealChange's own
     * reasoning, just for two independent RCBs instead of one. */
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&urcbDynGotReport), "urcbDyn's GI snapshot must not reach the callback");
    TEST_ASSERT_FALSE_MESSAGE(waitBriefly(&urcbDyn2GotReport), "urcbDyn2's GI snapshot must not reach the callback");

    SimServer_setIndication(sim, true);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&urcbDynGotReport),
            "expected urcbDyn (adopted the existing 'ds2' dataset instead of its own planned Mod "
            "chunk - the shared simulator's own SCL-unclaimed 'ds2' pre-exists on every test's "
            "server, and tier-3 adoption correctly prefers it over self-creating) to report the "
            "Ind1.stVal flip");
    TEST_ASSERT_FALSE_MESSAGE(urcbDyn2GotReport,
            "urcbDyn2's own chunk (Beh) must not report an Ind1 change it has no member for");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, urcbDynEntryCount,
            "urcbDyn adopted 'ds2', which contains only Ind1.stVal (no q/t siblings) - so exactly 1 "
            "entry, not a 3-member DO group");
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$Ind1$stVal", urcbDynEntry0Reference);

    urcbDynGotReport = false;
    SimServer_setBehStVal(sim, 1);

    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&urcbDyn2GotReport),
            "expected urcbDyn2 (assigned LLN0's Beh chunk) to report the Beh.stVal flip");
    TEST_ASSERT_FALSE_MESSAGE(urcbDynGotReport,
            "urcbDyn's own chunk (Mod) must not report a Beh change it has no member for");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, urcbDyn2EntryCount,
            "urcbDyn2's own chunk is exactly Beh's DO group (stVal + its q/t siblings)");
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/LLN0$ST$Beh$stVal", urcbDyn2Entry0Reference);

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves the per-connect-cycle dataset-count budgets (DynamicDatasetSession,
 * mms_report_client_connection.c) genuinely gate self-creation, on both
 * pools, once exhausted: fixtures/reporter1_budget.cid declares
 * <Services><DynDataSet max="0" maxAttributes="3"/><ConfDataSet max="0"
 * maxAttributes="3"/></Services> - the same maxAttributes=3 as
 * reporter1_chunking.cid (forcing the same two-chunk split), both pools
 * declared already exhausted.
 *
 * urcbDyn (the LN's first spare Dyn target) still succeeds - the base
 * simulator model always has a pre-existing "ds2" dataset available under
 * this LD, so urcbDyn is satisfied via the ADOPT tier (adoptUnclaimedDataset)
 * before it ever reaches tier-4 self-create at all, and adoption is free
 * against either budget (see DynamicDatasetSession's own doc comment on why
 * cache hits/adoptions never decrement anything). urcbDyn2 has no such
 * candidate available, so it's the one real proof here: its own
 * association-specific attempt is gated by the already-zero Dyn budget with
 * no wire call even made, its domain-scoped fallback attempt
 * (createAndCacheDynamicDataset) is gated the same way by the already-zero
 * Conf budget - genuine, total exhaustion with nowhere left to fall back to,
 * not just "the first attempt's own pool is capped." No DATSET gets set,
 * setRCBValues never even attempted, rcbStatusCallback fires false - while
 * an unrelated SCL-static RCB (brcbMain, entirely untouched by dynamic-
 * dataset bookkeeping) keeps enabling normally, proving graceful degradation
 * rather than a cascade failure. The simulator's own device-side count cap
 * (SimServer_createWithDatasetLimits) is configured generously (10) so the
 * failure here is provably from OUR OWN SCL-declared budgets, not an
 * incidental device-side rejection.
 */
void
test_dynamicDataset_countBudgetExhausted_secondChunkFailsCleanly(void) {
    SimServer sim = SimServer_createWithDatasetLimits(100, 10);
    SimServer_start(sim, TEST_PORT_BUDGET);

    IedModelLoadError modelError;
    IedModelHandle iedModel = IedModel_loadFromFile("fixtures/reporter1_budget.cid", "Reporter1",
            IED_MODEL_ACCESS_READ_AND_WRITE, &modelError);
    TEST_ASSERT_NOT_NULL_MESSAGE(iedModel, "expected reporter1_budget.cid to load successfully");

    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    MmsReportClientError clientError;
    MmsReportClientHandle client = MmsReportClient_create(iedModel, "127.0.0.1", TEST_PORT_BUDGET,
            &config, &clientError);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, clientError);

    MmsReportClient_setRcbStatusCallback(client, onRcbStatus, NULL);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, startError);

    TEST_ASSERT_TRUE_MESSAGE(waitUntilAtLeast(&brcbMainEnableCount, 1),
            "expected brcbMain (SCL-static dataset, unrelated to dynamic-dataset budget bookkeeping) "
            "to enable normally regardless of urcbDyn2's budget failure");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicRcbEnabled),
            "expected urcbDyn to succeed via adopting the pre-existing 'ds2' dataset - unrelated to "
            "either dynamic-dataset budget, both of which are declared already exhausted");
    TEST_ASSERT_FALSE_MESSAGE(dynamicRcbFailed, "urcbDyn adopts an existing dataset - must succeed");
    TEST_ASSERT_TRUE_MESSAGE(waitUntil(&dynamicRcb2Failed),
            "expected urcbDyn2 (no adoption candidate available) to fail cleanly once both the Dyn and "
            "Conf budgets are exhausted, not silently hang or crash the daemon");
    TEST_ASSERT_FALSE_MESSAGE(dynamicRcb2Enabled, "urcbDyn2 must not enable once both budgets are exhausted");

    MmsReportClient_destroy(client);
    IedModel_release(iedModel);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

/*
 * Proves (and refines) the hypothesis MmsReportClientConnection_stop's
 * disable-before-delete step is built on (found via manual IEDScout testing
 * against a real device): a dataset still referenced by an ENABLED RCB's
 * DatSet is refused for deletion, independent of any quota/pollution
 * question - purely because it's still in use. Deliberately bypasses
 * mms_report_client entirely (raw IedConnection calls only) - this is a
 * question about the SERVER's own behavior, not about this client's logic.
 *
 * The vendored reference server DOES enforce this - confirmed empirically
 * here. But disabling RptEna alone was NOT sufficient against it (still
 * refused with IED_ERROR_OBJECT_CONSTRAINT_CONFLICT/35) - the RCB's own
 * DatSet attribute continuing to point at the dataset is itself the
 * constraint. Clearing DatSet (empty string) alongside disabling RptEna is
 * what actually releases it, confirmed by the same delete then succeeding.
 * MmsReportClientConnection_stop's cleanup step does both, matching this
 * exact sequence.
 */
void
test_deleteDataSet_refusedWhileRcbEnabled_succeedsAfterDisable(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT_DELETE_WHILE_ENABLED);

    IedConnection conn = IedConnection_create();
    IedClientError err = IED_ERROR_OK;
    IedConnection_connect(conn, &err, "127.0.0.1", TEST_PORT_DELETE_WHILE_ENABLED);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, err, "expected the connection to associate");

    LinkedList members = LinkedList_create();
    LinkedList_add(members, (void*) "Reporter1LD1/GGIO1.SPCSO1.stVal[ST]");
    IedConnection_createDataSet(conn, &err, "Reporter1LD1/GGIO1$deleteTestDs", members);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, err, "expected the test dataset to be created");
    LinkedList_destroyStatic(members);

    ClientReportControlBlock rcb = IedConnection_getRCBValues(conn, &err, "Reporter1LD1/GGIO1.RP.urcbDyn", NULL);
    TEST_ASSERT_NOT_NULL(rcb);
    ClientReportControlBlock_setDataSetReference(rcb, "Reporter1LD1/GGIO1$deleteTestDs");
    ClientReportControlBlock_setRptEna(rcb, true);
    IedConnection_setRCBValues(conn, &err, rcb, RCB_ELEMENT_DATSET | RCB_ELEMENT_RPT_ENA, true);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, err, "expected enabling urcbDyn against the test dataset to succeed");

    IedClientError deleteErr = IED_ERROR_OK;
    bool deletedWhileEnabled = IedConnection_deleteDataSet(conn, &deleteErr, "Reporter1LD1/GGIO1$deleteTestDs");
    TEST_ASSERT_FALSE_MESSAGE(deletedWhileEnabled,
            "expected the reference server to refuse deleting a dataset still bound to an enabled RCB - if "
            "this fails, the reference server does NOT enforce the same rule the real device apparently "
            "does, and MmsReportClientConnection_stop's disable-and-unbind step doesn't change anything "
            "against a device with that behavior either");

    /* Disabling RptEna alone was tried first and was NOT enough - the
     * reference server still refused with IED_ERROR_OBJECT_CONSTRAINT_CONFLICT
     * (35). The RCB's own DatSet attribute still pointing at this dataset is
     * itself the constraint - clearing it (empty string) alongside disabling
     * is what actually releases it. This is the real mechanism
     * MmsReportClientConnection_stop's cleanup step needs to replicate. */
    ClientReportControlBlock_setRptEna(rcb, false);
    ClientReportControlBlock_setDataSetReference(rcb, "");
    IedConnection_setRCBValues(conn, &err, rcb, RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_DATSET, true);
    TEST_ASSERT_EQUAL_MESSAGE(IED_ERROR_OK, err, "expected disabling and unbinding urcbDyn's DatSet to succeed");
    ClientReportControlBlock_destroy(rcb);

    deleteErr = IED_ERROR_OK;
    bool deletedAfterDisable = IedConnection_deleteDataSet(conn, &deleteErr, "Reporter1LD1/GGIO1$deleteTestDs");
    TEST_ASSERT_TRUE_MESSAGE(deletedAfterDisable,
            "expected the delete to succeed once the RCB referencing it was disabled AND its DatSet cleared");

    IedConnection_close(conn);
    IedConnection_destroy(conn);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_dataChangeOnServer_triggersReportWithNewValue);
    RUN_TEST(test_authRequired_correctPassword_connectsAndEnablesRcb);
    RUN_TEST(test_authRequired_wrongPassword_neverConnects);
    RUN_TEST(test_authRequired_wrongPassword_firesConnectionRejectedCallback);
    RUN_TEST(test_dynamicDataset_createdOnEnable_andReportsRealChange);
    RUN_TEST(test_dynamicDataset_bufferedRcb_createdOnEnable_andSurvivesReconnect);
    RUN_TEST(test_orphanCleanup_ownUnclaimedDatasetDeleted_foreignDatasetLeftUntouched);
    RUN_TEST(test_siblingBufferedDynRcbs_reconnectDoesNotCrossAdoptEachOthersLeftoverDataset);
    RUN_TEST(test_dynamicDataset_giOnlyRcb_reportsRealChangeAfterTrgOpsFix);
    RUN_TEST(test_pulledLiveDataset_preAssignedByAnotherClient_reusedInsteadOfSelfCreated);
    RUN_TEST(test_reconnect_afterServerRestart_redeliverySuppressed_thenChangeReportsPreservedPreviousValue);
    RUN_TEST(test_crossRcbDuplicateContent_onlyOneOfTwoIdenticalRcbsReachesCallback);
    RUN_TEST(test_secondReconnectWithNoNewChanges_doesNotRedeliverBacklog);
    RUN_TEST(test_entryIdStaleGuard_doesNotSuppressLegitimateMultiEntryBacklog);
    RUN_TEST(test_dynamicDataset_maxAttributesExceeded_chunksOntoSpareRcbInstances);
    RUN_TEST(test_dynamicDataset_countBudgetExhausted_secondChunkFailsCleanly);
    RUN_TEST(test_deleteDataSet_refusedWhileRcbEnabled_succeedsAfterDisable);

    return UNITY_END();
}
