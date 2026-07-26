#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "scan_orchestration/service/scan_orchestration_api.h"

/*
 * Argument-validation-only wiring tests, plus a full start/stop lifecycle
 * driven against a deliberately nonexistent interface ("nonexistent0") -
 * every sweep fails fast with IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND and is
 * tolerated gracefully by the worker's own sweepLoop (see
 * scan_orchestration_worker.c), which proves scanId monotonicity and
 * refcounted dispatcher start/stop without needing any real reachable
 * network - same reasoning test_ied_discovery_api.c's own wiring tests use.
 * Real end-to-end behavior (a real device actually being found and
 * streamed) is covered by integration_tests/scan_dispatcher/ and
 * integration_tests/scan_orchestration/.
 *
 * Uses a dedicated high port range to avoid clashing with a real daemon
 * instance that might also be running on the default 8766 during test runs.
 */
#define TEST_SCAN_PORT 18866

static ScanOrchestrationHandle fixtureHandle;

void
setUp(void) {
    fixtureHandle = NULL;
}

void
tearDown(void) {
    if (fixtureHandle) {
        ScanOrchestration_destroy(fixtureHandle);
        fixtureHandle = NULL;
    }
}

static ScanOrchestrationHandle
createHandleOnTestPort(void) {
    ScanOrchestrationConfig config;
    ScanOrchestrationConfig_defaults(&config);
    config.scanDispatcherConfig.port = TEST_SCAN_PORT;
    config.defaultSweepIntervalMs = 50; /* fast sweeps, keeps the test quick */
    return ScanOrchestration_create(&config, NULL);
}

/* ---- ScanOrchestrationConfig_defaults ---- */

void
test_configDefaults_matchDocumentedValues(void) {
    ScanOrchestrationConfig config;
    ScanOrchestrationConfig_defaults(&config);

    TEST_ASSERT_EQUAL_UINT16(8766, config.scanDispatcherConfig.port);
    TEST_ASSERT_EQUAL_UINT32(10000, config.defaultSweepIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(500, config.discoveryConfigTemplate.tcpProbeTimeoutMs);
    TEST_ASSERT_EQUAL_INT(64, config.discoveryConfigTemplate.maxConcurrentTcpProbes);
}

void
test_configDefaults_doesNotCrash_onNull(void) {
    ScanOrchestrationConfig_defaults(NULL);
}

/* ---- create/destroy ---- */

void
test_create_succeeds_withNullConfig(void) {
    ScanOrchestrationError err;
    fixtureHandle = ScanOrchestration_create(NULL, &err);

    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, err);
}

void
test_destroy_doesNotCrash_onNull(void) {
    ScanOrchestration_destroy(NULL);
}

/* ---- startScan argument validation ---- */

void
test_startScan_rejectsNullHandle(void) {
    ScanRequest request = { .interfaceId = "lo", .mmsPort = 102 };
    uint64_t scanId;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT,
            ScanOrchestration_startScan(NULL, &request, &scanId));
}

void
test_startScan_rejectsNullRequest(void) {
    fixtureHandle = createHandleOnTestPort();
    uint64_t scanId;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT,
            ScanOrchestration_startScan(fixtureHandle, NULL, &scanId));
}

void
test_startScan_rejectsEmptyInterfaceId(void) {
    fixtureHandle = createHandleOnTestPort();
    ScanRequest request = { .interfaceId = "", .mmsPort = 102 };
    uint64_t scanId;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_ERR_INVALID_ARGUMENT,
            ScanOrchestration_startScan(fixtureHandle, &request, &scanId));
}

/* ---- stopScan / snapshot on unknown scanId ---- */

void
test_stopScan_returnsScanNotFound_onUnknownId(void) {
    fixtureHandle = createHandleOnTestPort();
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND, ScanOrchestration_stopScan(fixtureHandle, 999));
}

void
test_snapshotDiscoveredHosts_returnsScanNotFound_onUnknownId(void) {
    fixtureHandle = createHandleOnTestPort();
    char** hosts;
    int count;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND,
            ScanOrchestration_snapshotDiscoveredHosts(fixtureHandle, 999, &hosts, &count));
}

/* ---- full lifecycle against a nonexistent interface ---- */

void
test_startScan_thenStopScan_scanIdMonotonic(void) {
    fixtureHandle = createHandleOnTestPort();
    ScanRequest request = { .interfaceId = "nonexistent0", .mmsPort = 102 };

    uint64_t firstId, secondId;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &request, &firstId));
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, firstId));

    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &request, &secondId));
    TEST_ASSERT_TRUE(secondId > firstId);
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, secondId));
}

void
test_twoConcurrentScans_shareDispatcher_independentStop(void) {
    fixtureHandle = createHandleOnTestPort();
    ScanRequest requestA = { .interfaceId = "nonexistent0", .mmsPort = 102 };
    ScanRequest requestB = { .interfaceId = "nonexistent0", .mmsPort = 103 };

    uint64_t scanIdA, scanIdB;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &requestA, &scanIdA));
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &requestB, &scanIdB));
    TEST_ASSERT_TRUE(scanIdA != scanIdB);

    /* Stopping the first must not affect the second still being findable
     * (the shared dispatcher must stay up - refcount 2->1, not 2->0). */
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, scanIdA));

    char** hosts;
    int count;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK,
            ScanOrchestration_snapshotDiscoveredHosts(fixtureHandle, scanIdB, &hosts, &count));
    ScanOrchestration_freeDiscoveredHostsSnapshot(hosts, count);

    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, scanIdB));
}

void
test_stopScan_twice_secondReturnsScanNotFound(void) {
    fixtureHandle = createHandleOnTestPort();
    ScanRequest request = { .interfaceId = "nonexistent0", .mmsPort = 102 };

    uint64_t scanId;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(fixtureHandle, &request, &scanId));
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_stopScan(fixtureHandle, scanId));
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_ERR_SCAN_NOT_FOUND, ScanOrchestration_stopScan(fixtureHandle, scanId));
}

void
test_destroy_drainsStillActiveScans(void) {
    ScanOrchestrationHandle handle = createHandleOnTestPort();
    ScanRequest request = { .interfaceId = "nonexistent0", .mmsPort = 102 };

    uint64_t scanId;
    TEST_ASSERT_EQUAL(SCAN_ORCHESTRATION_OK, ScanOrchestration_startScan(handle, &request, &scanId));

    /* Never stopped explicitly - destroy must drain it without hanging or crashing. */
    ScanOrchestration_destroy(handle);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_configDefaults_matchDocumentedValues);
    RUN_TEST(test_configDefaults_doesNotCrash_onNull);

    RUN_TEST(test_create_succeeds_withNullConfig);
    RUN_TEST(test_destroy_doesNotCrash_onNull);

    RUN_TEST(test_startScan_rejectsNullHandle);
    RUN_TEST(test_startScan_rejectsNullRequest);
    RUN_TEST(test_startScan_rejectsEmptyInterfaceId);

    RUN_TEST(test_stopScan_returnsScanNotFound_onUnknownId);
    RUN_TEST(test_snapshotDiscoveredHosts_returnsScanNotFound_onUnknownId);

    RUN_TEST(test_startScan_thenStopScan_scanIdMonotonic);
    RUN_TEST(test_twoConcurrentScans_shareDispatcher_independentStop);
    RUN_TEST(test_stopScan_twice_secondReturnsScanNotFound);
    RUN_TEST(test_destroy_drainsStillActiveScans);

    return UNITY_END();
}
