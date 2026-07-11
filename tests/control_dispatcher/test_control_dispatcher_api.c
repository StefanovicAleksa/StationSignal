#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/control_dispatcher/service/control_dispatcher_api.h"
#include "device_manager/service/device_manager_api.h"

/*
 * Real bind on loopback for real - no external IED dependency to avoid here,
 * same reasoning as test_ipc_dispatcher_api.c/test_scan_dispatcher_api.c.
 * Uses a dedicated high port range to avoid clashing with a real daemon
 * instance that might also be running on the default 8767 during test runs.
 * Never sends a real websocket frame at this layer (that's
 * integration_tests/control_dispatcher/'s job) - just create/start/stop/
 * destroy lifecycle plus argument validation.
 */
#define TEST_PORT 18767

static ControlDispatcherHandle fixtureHandle;
static DeviceManagerHandle fixtureDeviceManager;

void
setUp(void) {
    fixtureHandle = NULL;
    fixtureDeviceManager = DeviceManager_create(NULL, NULL);
}

void
tearDown(void) {
    if (fixtureHandle) {
        ControlDispatcher_destroy(fixtureHandle);
        fixtureHandle = NULL;
    }
    if (fixtureDeviceManager) {
        DeviceManager_destroy(fixtureDeviceManager);
        fixtureDeviceManager = NULL;
    }
}

/* ---- ControlDispatcherConfig_defaults ---- */

void
test_configDefaults_matchDocumentedValues(void) {
    ControlDispatcherConfig config;
    ControlDispatcherConfig_defaults(&config);

    TEST_ASSERT_EQUAL_UINT16(8767, config.port);
    TEST_ASSERT_EQUAL_INT(256, config.ringBufferCapacity);
    TEST_ASSERT_EQUAL_INT(16, config.maxConnections);
    TEST_ASSERT_EQUAL_INT(64, config.requestQueueCapacity);
}

void
test_configDefaults_doesNotCrash_onNull(void) {
    ControlDispatcherConfig_defaults(NULL);
}

/* ---- create ---- */

void
test_create_appliesDefaults_whenConfigIsNull(void) {
    ControlDispatcherError error;
    fixtureHandle = ControlDispatcher_create(NULL, fixtureDeviceManager, &error);

    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_OK, error);
}

void
test_create_rejectsNullDeviceManager(void) {
    ControlDispatcherError error;
    ControlDispatcherHandle handle = ControlDispatcher_create(NULL, NULL, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT, error);
}

void
test_create_rejectsInvalidCapacities(void) {
    ControlDispatcherConfig config;
    ControlDispatcherConfig_defaults(&config);
    config.ringBufferCapacity = 0;

    ControlDispatcherError error;
    ControlDispatcherHandle handle = ControlDispatcher_create(&config, fixtureDeviceManager, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT, error);
}

/* ---- start / stop, real bind on loopback ---- */

void
test_stop_beforeStart_isNoOp(void) {
    ControlDispatcherConfig config;
    ControlDispatcherConfig_defaults(&config);
    config.port = TEST_PORT;

    fixtureHandle = ControlDispatcher_create(&config, fixtureDeviceManager, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    ControlDispatcher_stop(fixtureHandle); /* never started - must not crash */
}

void
test_start_thenDoubleStart_returnsAlreadyRunning(void) {
    ControlDispatcherConfig config;
    ControlDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 1;

    fixtureHandle = ControlDispatcher_create(&config, fixtureDeviceManager, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_OK, ControlDispatcher_start(fixtureHandle));
    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_ERR_ALREADY_RUNNING, ControlDispatcher_start(fixtureHandle));

    ControlDispatcher_stop(fixtureHandle);
}

void
test_startStopStart_reusesPortCleanly(void) {
    ControlDispatcherConfig config;
    ControlDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 2;

    fixtureHandle = ControlDispatcher_create(&config, fixtureDeviceManager, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_OK, ControlDispatcher_start(fixtureHandle));
    ControlDispatcher_stop(fixtureHandle); /* fully tears down + releases the port */

    /* Same handle, same port - must bind cleanly again, not EADDRINUSE. */
    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_OK, ControlDispatcher_start(fixtureHandle));

    ControlDispatcher_stop(fixtureHandle);
}

/* ---- NULL-safety ---- */

void
test_destroy_and_stop_doNotCrash_onNullHandle(void) {
    ControlDispatcher_stop(NULL);
    ControlDispatcher_destroy(NULL);
}

void
test_start_returnsInvalidArgument_onNullHandle(void) {
    TEST_ASSERT_EQUAL(CONTROL_DISPATCHER_ERR_INVALID_ARGUMENT, ControlDispatcher_start(NULL));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_configDefaults_matchDocumentedValues);
    RUN_TEST(test_configDefaults_doesNotCrash_onNull);

    RUN_TEST(test_create_appliesDefaults_whenConfigIsNull);
    RUN_TEST(test_create_rejectsNullDeviceManager);
    RUN_TEST(test_create_rejectsInvalidCapacities);

    RUN_TEST(test_stop_beforeStart_isNoOp);
    RUN_TEST(test_start_thenDoubleStart_returnsAlreadyRunning);
    RUN_TEST(test_startStopStart_reusesPortCleanly);

    RUN_TEST(test_destroy_and_stop_doNotCrash_onNullHandle);
    RUN_TEST(test_start_returnsInvalidArgument_onNullHandle);

    return UNITY_END();
}
