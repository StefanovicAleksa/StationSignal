#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "device_manager/service/device_manager_api.h"

/*
 * Argument-validation-only wiring tests, mirrors
 * tests/orchestration/test_orchestration_api.c's own scope - never drives a
 * real Orchestration_run* against a reachable host, no real connection/socket
 * opened. Real end-to-end multi-device behavior (including the
 * HOST_ALREADY_RUNNING dedupe race and the two-phase lock's
 * non-serialization property, both of which need genuine concurrency to
 * observe) is covered by integration_tests/device_manager/.
 */

static DeviceManagerHandle fixtureHandle;

void
setUp(void) {
    fixtureHandle = NULL;
}

void
tearDown(void) {
    if (fixtureHandle) {
        DeviceManager_destroy(fixtureHandle);
        fixtureHandle = NULL;
    }
}

/* ---- DeviceManagerConfig_defaults ---- */

void
test_configDefaults_matchDocumentedValues(void) {
    DeviceManagerConfig config;
    DeviceManagerConfig_defaults(&config);

    TEST_ASSERT_EQUAL_UINT16(9000, config.wsPortRangeStart);
    TEST_ASSERT_EQUAL_UINT16(9999, config.wsPortRangeEnd);
}

void
test_configDefaults_doesNotCrash_onNull(void) {
    DeviceManagerConfig_defaults(NULL);
}

/* ---- DeviceManager_create ---- */

void
test_create_appliesDefaults_whenConfigIsNull(void) {
    DeviceManagerError err;
    fixtureHandle = DeviceManager_create(NULL, &err);

    TEST_ASSERT_NOT_NULL(fixtureHandle);
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK, err);
}

void
test_create_rejectsInvertedPortRange(void) {
    DeviceManagerConfig config;
    config.wsPortRangeStart = 9010;
    config.wsPortRangeEnd = 9000;

    DeviceManagerError err;
    DeviceManagerHandle handle = DeviceManager_create(&config, &err);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT, err);
}

/* ---- DeviceManager_startReporting argument validation ---- */

void
test_startReporting_rejectsNullHandle(void) {
    uint64_t deviceId;
    uint16_t wsPort;
    DeviceManagerError err = DeviceManager_startReporting(NULL, "10.0.0.1", 102, "Reporter1", "lo",
            NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, &deviceId, &wsPort, NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT, err);
}

void
test_startReporting_rejectsEmptyHost(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);

    uint64_t deviceId;
    uint16_t wsPort;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_startReporting(fixtureHandle, "", 102, "Reporter1", "lo",
                    NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, &deviceId, &wsPort, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_startReporting(fixtureHandle, NULL, 102, "Reporter1", "lo",
                    NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, &deviceId, &wsPort, NULL, NULL, NULL));
}

void
test_startReporting_rejectsEmptyInterfaceId(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);

    uint64_t deviceId;
    uint16_t wsPort;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_startReporting(fixtureHandle, "10.0.0.1", 102, "Reporter1", "",
                    NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, &deviceId, &wsPort, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_startReporting(fixtureHandle, "10.0.0.1", 102, "Reporter1", NULL,
                    NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, &deviceId, &wsPort, NULL, NULL, NULL));
}

void
test_startReporting_rejectsSclFilePath_withoutIedName(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);

    uint64_t deviceId;
    uint16_t wsPort;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_startReporting(fixtureHandle, "10.0.0.1", 102, NULL, "lo",
                    "/tmp/some.icd", NULL, IED_MODEL_ACCESS_REPORT_ONLY, &deviceId, &wsPort, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_startReporting(fixtureHandle, "10.0.0.1", 102, "", "lo",
                    "/tmp/some.icd", NULL, IED_MODEL_ACCESS_REPORT_ONLY, &deviceId, &wsPort, NULL, NULL, NULL));
}

void
test_startReporting_rejectsNullOutParams(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);

    uint16_t wsPort;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_startReporting(fixtureHandle, "10.0.0.1", 102, "Reporter1", "lo",
                    NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, NULL, &wsPort, NULL, NULL, NULL));

    uint64_t deviceId;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_startReporting(fixtureHandle, "10.0.0.1", 102, "Reporter1", "lo",
                    NULL, NULL, IED_MODEL_ACCESS_REPORT_ONLY, &deviceId, NULL, NULL, NULL, NULL));
}

/* ---- DeviceManager_stopReporting ---- */

void
test_stopReporting_rejectsNullHandle(void) {
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT, DeviceManager_stopReporting(NULL, 1, NULL));
}

void
test_stopReporting_returnsDeviceNotFound_forUnknownId(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND,
            DeviceManager_stopReporting(fixtureHandle, 999, NULL));
}

/* ---- DeviceManager_stopReportingByAddress ---- */

void
test_stopReportingByAddress_rejectsNullHandle(void) {
    uint64_t outDeviceId;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_stopReportingByAddress(NULL, "10.0.0.1", 102, &outDeviceId, NULL));
}

void
test_stopReportingByAddress_rejectsEmptyHost(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);

    uint64_t outDeviceId;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_stopReportingByAddress(fixtureHandle, "", 102, &outDeviceId, NULL));
}

void
test_stopReportingByAddress_rejectsNullOutDeviceId(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManager_stopReportingByAddress(fixtureHandle, "10.0.0.1", 102, NULL, NULL));
}

void
test_stopReportingByAddress_returnsDeviceNotFound_whenNothingRegisteredThere(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);

    uint64_t outDeviceId = 0;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND,
            DeviceManager_stopReportingByAddress(fixtureHandle, "10.0.0.1", 102, &outDeviceId, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, outDeviceId); /* untouched on failure */
}

/* ---- NULL-safety ---- */

void
test_destroy_doesNotCrash_onNullHandle(void) {
    DeviceManager_destroy(NULL);
}

void
test_destroy_onFreshHandle_withNoDevices_doesNotCrash(void) {
    fixtureHandle = DeviceManager_create(NULL, NULL);
    DeviceManager_destroy(fixtureHandle);
    fixtureHandle = NULL; /* already destroyed - tearDown must not double-free */
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_configDefaults_matchDocumentedValues);
    RUN_TEST(test_configDefaults_doesNotCrash_onNull);

    RUN_TEST(test_create_appliesDefaults_whenConfigIsNull);
    RUN_TEST(test_create_rejectsInvertedPortRange);

    RUN_TEST(test_startReporting_rejectsNullHandle);
    RUN_TEST(test_startReporting_rejectsEmptyHost);
    RUN_TEST(test_startReporting_rejectsEmptyInterfaceId);
    RUN_TEST(test_startReporting_rejectsSclFilePath_withoutIedName);
    RUN_TEST(test_startReporting_rejectsNullOutParams);

    RUN_TEST(test_stopReporting_rejectsNullHandle);
    RUN_TEST(test_stopReporting_returnsDeviceNotFound_forUnknownId);

    RUN_TEST(test_stopReportingByAddress_rejectsNullHandle);
    RUN_TEST(test_stopReportingByAddress_rejectsEmptyHost);
    RUN_TEST(test_stopReportingByAddress_rejectsNullOutDeviceId);
    RUN_TEST(test_stopReportingByAddress_returnsDeviceNotFound_whenNothingRegisteredThere);

    RUN_TEST(test_destroy_doesNotCrash_onNullHandle);
    RUN_TEST(test_destroy_onFreshHandle_withNoDevices_doesNotCrash);

    return UNITY_END();
}
