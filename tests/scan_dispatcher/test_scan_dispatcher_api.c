#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/scan_dispatcher/service/scan_dispatcher_api.h"

/*
 * Real bind on loopback for real - no external IED dependency to avoid here,
 * same reasoning as test_ipc_dispatcher_api.c. Uses a dedicated high port
 * range to avoid clashing with a real daemon instance that might also be
 * running on the default 8766 during test runs.
 */
#define TEST_PORT 18766

static ScanDispatcherHandle fixtureHandle;

void
setUp(void) {
    fixtureHandle = NULL;
}

void
tearDown(void) {
    if (fixtureHandle) {
        ScanDispatcher_destroy(fixtureHandle);
        fixtureHandle = NULL;
    }
}

/* ---- ScanDispatcherConfig_defaults ---- */

void
test_configDefaults_matchDocumentedValues(void) {
    ScanDispatcherConfig config;
    ScanDispatcherConfig_defaults(&config);

    TEST_ASSERT_EQUAL_UINT16(8766, config.port);
    TEST_ASSERT_EQUAL_INT(256, config.ringBufferCapacity);
    TEST_ASSERT_EQUAL_INT(16, config.maxConnections);
}

void
test_configDefaults_doesNotCrash_onNull(void) {
    ScanDispatcherConfig_defaults(NULL);
}

/* ---- create ---- */

void
test_create_appliesDefaults_whenConfigIsNull(void) {
    ScanDispatcherError error;
    fixtureHandle = ScanDispatcher_create(NULL, &error);

    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_OK, error);
}

void
test_create_rejectsInvalidCapacities(void) {
    ScanDispatcherConfig config;
    ScanDispatcherConfig_defaults(&config);
    config.ringBufferCapacity = 0;

    ScanDispatcherError error;
    ScanDispatcherHandle handle = ScanDispatcher_create(&config, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_ERR_INVALID_ARGUMENT, error);
}

/* ---- start / stop, real bind on loopback ---- */

void
test_stop_beforeStart_isNoOp(void) {
    ScanDispatcherConfig config;
    ScanDispatcherConfig_defaults(&config);
    config.port = TEST_PORT;

    fixtureHandle = ScanDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    ScanDispatcher_stop(fixtureHandle); /* never started - must not crash */
}

void
test_start_thenDoubleStart_returnsAlreadyRunning(void) {
    ScanDispatcherConfig config;
    ScanDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 1;

    fixtureHandle = ScanDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_OK, ScanDispatcher_start(fixtureHandle));
    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_ERR_ALREADY_RUNNING, ScanDispatcher_start(fixtureHandle));

    ScanDispatcher_stop(fixtureHandle);
}

void
test_startStopStart_reusesPortCleanly(void) {
    ScanDispatcherConfig config;
    ScanDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 2;

    fixtureHandle = ScanDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_OK, ScanDispatcher_start(fixtureHandle));
    ScanDispatcher_stop(fixtureHandle); /* fully tears down + releases the port */

    /* Same handle, same port - must bind cleanly again, not EADDRINUSE. */
    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_OK, ScanDispatcher_start(fixtureHandle));

    ScanDispatcher_stop(fixtureHandle);
}

/* ---- NULL-safety ---- */

void
test_destroy_and_stop_doNotCrash_onNullHandle(void) {
    ScanDispatcher_stop(NULL);
    ScanDispatcher_destroy(NULL);
}

void
test_start_returnsInvalidArgument_onNullHandle(void) {
    TEST_ASSERT_EQUAL(SCAN_DISPATCHER_ERR_INVALID_ARGUMENT, ScanDispatcher_start(NULL));
}

void
test_publishDeviceFound_doesNotCrash_onNullHandle(void) {
    ScanDispatcher_publishDeviceFound(NULL, 1, "127.0.0.1", 102);
}

void
test_publishDeviceFound_isNoOp_whenNotRunning(void) {
    ScanDispatcherConfig config;
    ScanDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 3;

    fixtureHandle = ScanDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    /* Not started - must not crash even though this reaches into the ring
     * buffer/ws-server fields. */
    ScanDispatcher_publishDeviceFound(fixtureHandle, 1, "127.0.0.1", 102);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_configDefaults_matchDocumentedValues);
    RUN_TEST(test_configDefaults_doesNotCrash_onNull);

    RUN_TEST(test_create_appliesDefaults_whenConfigIsNull);
    RUN_TEST(test_create_rejectsInvalidCapacities);

    RUN_TEST(test_stop_beforeStart_isNoOp);
    RUN_TEST(test_start_thenDoubleStart_returnsAlreadyRunning);
    RUN_TEST(test_startStopStart_reusesPortCleanly);

    RUN_TEST(test_destroy_and_stop_doNotCrash_onNullHandle);
    RUN_TEST(test_start_returnsInvalidArgument_onNullHandle);

    RUN_TEST(test_publishDeviceFound_doesNotCrash_onNullHandle);
    RUN_TEST(test_publishDeviceFound_isNoOp_whenNotRunning);

    return UNITY_END();
}
