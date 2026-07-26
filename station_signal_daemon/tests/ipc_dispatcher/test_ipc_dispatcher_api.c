#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ipc_dispatcher/service/ipc_dispatcher_api.h"

/*
 * Unlike goose_subscriber_api/mms_report_client_api's own unit tests (which
 * deliberately avoid a real connection since that needs an external IED),
 * ipc_dispatcher has no external dependency to avoid - _start()/_stop() can
 * genuinely bind+listen+unbind on loopback for real in a fast unit test.
 * Uses a dedicated high port range to avoid clashing with a real daemon
 * instance that might also be running on the default 8765 during test runs.
 */
#define TEST_PORT 18765

static IpcDispatcherHandle fixtureHandle;

void
setUp(void) {
    fixtureHandle = NULL;
}

void
tearDown(void) {
    if (fixtureHandle) {
        IpcDispatcher_destroy(fixtureHandle);
        fixtureHandle = NULL;
    }
}

/* ---- IpcDispatcherConfig_defaults ---- */

void
test_configDefaults_matchDocumentedValues(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);

    TEST_ASSERT_EQUAL_UINT16(8765, config.port);
    TEST_ASSERT_EQUAL_INT(256, config.ringBufferCapacity);
    TEST_ASSERT_EQUAL_INT(16, config.maxConnections);
}

void
test_configDefaults_doesNotCrash_onNull(void) {
    IpcDispatcherConfig_defaults(NULL);
}

/* ---- create ---- */

void
test_create_appliesDefaults_whenConfigIsNull(void) {
    IpcDispatcherError error;
    fixtureHandle = IpcDispatcher_create(NULL, &error);

    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_OK, error);
}

void
test_create_rejectsInvalidCapacities(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);
    config.ringBufferCapacity = 0;

    IpcDispatcherError error;
    IpcDispatcherHandle handle = IpcDispatcher_create(&config, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_ERR_INVALID_ARGUMENT, error);
}

/* ---- start / stop, real bind on loopback ---- */

void
test_stop_beforeStart_isNoOp(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);
    config.port = TEST_PORT;

    fixtureHandle = IpcDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    IpcDispatcher_stop(fixtureHandle); /* never started - must not crash */
}

void
test_start_thenDoubleStart_returnsAlreadyRunning(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 1;

    fixtureHandle = IpcDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    TEST_ASSERT_EQUAL(IPC_DISPATCHER_OK, IpcDispatcher_start(fixtureHandle));
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_ERR_ALREADY_RUNNING, IpcDispatcher_start(fixtureHandle));

    IpcDispatcher_stop(fixtureHandle);
}

void
test_startStopStart_reusesPortCleanly(void) {
    IpcDispatcherConfig config;
    IpcDispatcherConfig_defaults(&config);
    config.port = TEST_PORT + 2;

    fixtureHandle = IpcDispatcher_create(&config, NULL);
    TEST_ASSERT_NOT_NULL(fixtureHandle);

    TEST_ASSERT_EQUAL(IPC_DISPATCHER_OK, IpcDispatcher_start(fixtureHandle));
    IpcDispatcher_stop(fixtureHandle); /* fully tears down + releases the port, see IpcDispatcher_stop's doc comment */

    /* Same handle, same port - must bind cleanly again, not EADDRINUSE. */
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_OK, IpcDispatcher_start(fixtureHandle));

    IpcDispatcher_stop(fixtureHandle);
}

/* ---- NULL-safety ---- */

void
test_destroy_and_stop_doNotCrash_onNullHandle(void) {
    IpcDispatcher_stop(NULL);
    IpcDispatcher_destroy(NULL);
}

void
test_start_returnsInvalidArgument_onNullHandle(void) {
    TEST_ASSERT_EQUAL(IPC_DISPATCHER_ERR_INVALID_ARGUMENT, IpcDispatcher_start(NULL));
}

/* ---- callback adapters, NULL-safety ---- */

void
test_onMmsReport_doesNotCrash_onNullRecord(void) {
    IpcDispatcher_onMmsReport(NULL, NULL);
}

void
test_onGooseRecord_doesNotCrash_onNullRecord(void) {
    IpcDispatcher_onGooseRecord(NULL, NULL);
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

    RUN_TEST(test_onMmsReport_doesNotCrash_onNullRecord);
    RUN_TEST(test_onGooseRecord_doesNotCrash_onNullRecord);

    return UNITY_END();
}
