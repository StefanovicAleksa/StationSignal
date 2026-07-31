#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "cJSON.h"
#include "features/control_dispatcher/domain/control_dispatcher_usecases.h"
#include "device_manager/service/device_manager_api.h"
#include "scan_orchestration/service/scan_orchestration_api.h"

/*
 * Dispatch/error-mapping tests only, driven against a REAL DeviceManagerHandle
 * and a REAL ScanOrchestrationHandle in deliberately-failing, no-network
 * cases (empty host, unknown deviceId, empty interfaceId, unknown scanId) -
 * mirrors tests/orchestration/test_orchestration_api.c's own
 * "argument-validation only, no real I/O" scope. The success-path JSON
 * mapping (a real StartReporting/StartScan succeeding) is proven only by
 * integration_tests/control_dispatcher/'s E2E test, which has a real IED/
 * network interface to talk to.
 */

static DeviceManagerHandle fixtureDeviceManager;
static ScanOrchestrationHandle fixtureScanOrchestration;
static char* fixtureJson;
static cJSON* fixtureParsed;

void
setUp(void) {
    fixtureDeviceManager = DeviceManager_create(NULL, NULL);
    fixtureScanOrchestration = ScanOrchestration_create(NULL, NULL);
    fixtureJson = NULL;
    fixtureParsed = NULL;
}

void
tearDown(void) {
    if (fixtureParsed) cJSON_Delete(fixtureParsed);
    free(fixtureJson);
    if (fixtureDeviceManager) {
        DeviceManager_destroy(fixtureDeviceManager);
        fixtureDeviceManager = NULL;
    }
    if (fixtureScanOrchestration) {
        ScanOrchestration_destroy(fixtureScanOrchestration);
        fixtureScanOrchestration = NULL;
    }
}

static ControlRequest*
makeStartRequest(void) {
    ControlRequest* request = calloc(1, sizeof(ControlRequest));
    request->requestId = strdup("req-1");
    request->type = CONTROL_REQ_START_REPORTING;
    request->host = NULL; /* deliberately invalid - empty host, no real network I/O attempted */
    request->mmsPort = 102;
    request->interfaceId = strdup("lo");
    request->accessMode = IED_MODEL_ACCESS_REPORT_ONLY;
    return request;
}

static void
freeRequest(ControlRequest* request) {
    if (!request) return;
    free(request->requestId);
    free(request->host);
    free(request->iedName);
    free(request->interfaceId);
    free(request->sclFilePath);
    free(request->acseAuthPassword);
    free(request);
}

void
test_processRequest_returnsNull_onNullRequest(void) {
    TEST_ASSERT_NULL(ControlDispatcherUseCases_processRequest(NULL, fixtureDeviceManager, fixtureScanOrchestration));
}

void
test_startReporting_invalidArgument_mapsToErrorEnvelope(void) {
    ControlRequest* request = makeStartRequest();

    fixtureJson = ControlDispatcherUseCases_processRequest(request, fixtureDeviceManager, fixtureScanOrchestration);
    freeRequest(request);

    TEST_ASSERT_NOT_NULL(fixtureJson);
    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("req-1", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "requestId")->valuestring);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));

    cJSON* error = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("INVALID_ARGUMENT", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);
}

void
test_stopReporting_unknownDeviceId_mapsToErrorEnvelope(void) {
    ControlRequest* request = calloc(1, sizeof(ControlRequest));
    request->requestId = strdup("req-2");
    request->type = CONTROL_REQ_STOP_REPORTING;
    request->deviceId = 999;

    fixtureJson = ControlDispatcherUseCases_processRequest(request, fixtureDeviceManager, fixtureScanOrchestration);
    freeRequest(request);

    TEST_ASSERT_NOT_NULL(fixtureJson);
    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("STOP_REPORTING", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")->valuestring);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));

    cJSON* error = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("DEVICE_NOT_FOUND", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);
}

void
test_stopReportingByAddress_nothingRegistered_mapsToErrorEnvelope(void) {
    /* host != NULL is the discriminator control_dispatcher_usecases.c relies
     * on to take the address-based branch - see ControlRequest's own doc
     * comment (control_dispatcher_types.h). */
    ControlRequest* request = calloc(1, sizeof(ControlRequest));
    request->requestId = strdup("req-5");
    request->type = CONTROL_REQ_STOP_REPORTING;
    request->host = strdup("10.0.0.1");
    request->mmsPort = 102;

    fixtureJson = ControlDispatcherUseCases_processRequest(request, fixtureDeviceManager, fixtureScanOrchestration);
    freeRequest(request);

    TEST_ASSERT_NOT_NULL(fixtureJson);
    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("req-5", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "requestId")->valuestring);
    TEST_ASSERT_EQUAL_STRING("STOP_REPORTING", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")->valuestring);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));

    cJSON* error = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("DEVICE_NOT_FOUND", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);
}

void
test_startScan_invalidArgument_mapsToErrorEnvelope(void) {
    /* interfaceId NULL - ScanOrchestrationWorker_create's own validation
     * rejects this before anything touches a real network interface. */
    ControlRequest* request = calloc(1, sizeof(ControlRequest));
    request->requestId = strdup("req-3");
    request->type = CONTROL_REQ_START_SCAN;
    request->interfaceId = NULL;
    request->mmsPort = 102;

    fixtureJson = ControlDispatcherUseCases_processRequest(request, fixtureDeviceManager, fixtureScanOrchestration);
    freeRequest(request);

    TEST_ASSERT_NOT_NULL(fixtureJson);
    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("req-3", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "requestId")->valuestring);
    TEST_ASSERT_EQUAL_STRING("START_SCAN", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")->valuestring);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));

    cJSON* error = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("INVALID_ARGUMENT", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);
}

void
test_stopScan_unknownScanId_mapsToErrorEnvelope(void) {
    ControlRequest* request = calloc(1, sizeof(ControlRequest));
    request->requestId = strdup("req-4");
    request->type = CONTROL_REQ_STOP_SCAN;
    request->scanId = 999;

    fixtureJson = ControlDispatcherUseCases_processRequest(request, fixtureDeviceManager, fixtureScanOrchestration);
    freeRequest(request);

    TEST_ASSERT_NOT_NULL(fixtureJson);
    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("STOP_SCAN", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")->valuestring);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));

    cJSON* error = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("SCAN_NOT_FOUND", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_processRequest_returnsNull_onNullRequest);
    RUN_TEST(test_startReporting_invalidArgument_mapsToErrorEnvelope);
    RUN_TEST(test_stopReporting_unknownDeviceId_mapsToErrorEnvelope);
    RUN_TEST(test_stopReportingByAddress_nothingRegistered_mapsToErrorEnvelope);
    RUN_TEST(test_startScan_invalidArgument_mapsToErrorEnvelope);
    RUN_TEST(test_stopScan_unknownScanId_mapsToErrorEnvelope);

    return UNITY_END();
}
