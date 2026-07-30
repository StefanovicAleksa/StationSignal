#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "cJSON.h"
#include "features/control_dispatcher/data/control_dispatcher_json_writer.h"

/* Envelope shape asserted via cJSON_Parse + field inspection (not string
 * equality) - resilient to key ordering, same convention as
 * test_ipc_dispatcher_json_writer.c/test_scan_dispatcher_json_writer.c. */

static char* fixtureJson;
static cJSON* fixtureParsed;

void
setUp(void) {
    fixtureJson = NULL;
    fixtureParsed = NULL;
}

void
tearDown(void) {
    if (fixtureParsed) cJSON_Delete(fixtureParsed);
    free(fixtureJson);
}

void
test_writeStartSuccess_hasExpectedShape(void) {
    fixtureJson = ControlDispatcherJsonWriter_writeStartSuccess("req-1", 3, 9001, true, false);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("req-1", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "requestId")->valuestring);
    TEST_ASSERT_EQUAL_STRING("START_REPORTING", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error")));

    cJSON* result = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "result");
    TEST_ASSERT_NOT_NULL(result);
    /* Unity's vendored build has double-precision assertions disabled (see
     * CLAUDE.md) - plain C == comparison instead, same pattern
     * test_ipc_dispatcher_value_codec.c already uses. */
    TEST_ASSERT_TRUE(3 == cJSON_GetObjectItemCaseSensitive(result, "deviceId")->valuedouble);
    TEST_ASSERT_TRUE(9001 == cJSON_GetObjectItemCaseSensitive(result, "wsPort")->valuedouble);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "mmsAvailable")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "gooseAvailable")));
}

void
test_writeStopSuccess_hasExpectedShape(void) {
    fixtureJson = ControlDispatcherJsonWriter_writeStopSuccess("req-2", 7);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("STOP_REPORTING", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));

    cJSON* result = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(7 == cJSON_GetObjectItemCaseSensitive(result, "deviceId")->valuedouble);
}

void
test_writeError_hasExpectedShape_withStageAndDetail(void) {
    fixtureJson = ControlDispatcherJsonWriter_writeError("req-3", "START_REPORTING", "ORCHESTRATION_FAILED",
            "orchestration failed", "SCL bootstrap", "no MMS server");
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "result")));

    cJSON* error = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_EQUAL_STRING("ORCHESTRATION_FAILED", cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring);
    TEST_ASSERT_EQUAL_STRING("orchestration failed", cJSON_GetObjectItemCaseSensitive(error, "message")->valuestring);
    TEST_ASSERT_EQUAL_STRING("SCL bootstrap", cJSON_GetObjectItemCaseSensitive(error, "stage")->valuestring);
    TEST_ASSERT_EQUAL_STRING("no MMS server", cJSON_GetObjectItemCaseSensitive(error, "detail")->valuestring);
}

void
test_writeError_omitsStageAndDetail_whenNull(void) {
    fixtureJson = ControlDispatcherJsonWriter_writeError("req-4", "STOP_REPORTING", "DEVICE_NOT_FOUND",
            "unknown deviceId", NULL, NULL);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    cJSON* error = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error");
    TEST_ASSERT_NOT_NULL(error);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(error, "stage"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(error, "detail"));
}

void
test_writeError_nullRequestIdAndAction_writeJsonNull(void) {
    fixtureJson = ControlDispatcherJsonWriter_writeError(NULL, NULL, "MALFORMED_REQUEST", "invalid JSON", NULL, NULL);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "requestId")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")));
}

void
test_writeScanStartSuccess_hasExpectedShape(void) {
    fixtureJson = ControlDispatcherJsonWriter_writeScanStartSuccess("req-5", 42);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("req-5", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "requestId")->valuestring);
    TEST_ASSERT_EQUAL_STRING("START_SCAN", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "error")));

    cJSON* result = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(42 == cJSON_GetObjectItemCaseSensitive(result, "scanId")->valuedouble);
}

void
test_writeScanStopSuccess_hasExpectedShape(void) {
    fixtureJson = ControlDispatcherJsonWriter_writeScanStopSuccess("req-6", 42);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("STOP_SCAN", cJSON_GetObjectItemCaseSensitive(fixtureParsed, "action")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fixtureParsed, "success")));

    cJSON* result = cJSON_GetObjectItemCaseSensitive(fixtureParsed, "result");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(42 == cJSON_GetObjectItemCaseSensitive(result, "scanId")->valuedouble);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_writeStartSuccess_hasExpectedShape);
    RUN_TEST(test_writeStopSuccess_hasExpectedShape);
    RUN_TEST(test_writeError_hasExpectedShape_withStageAndDetail);
    RUN_TEST(test_writeError_omitsStageAndDetail_whenNull);
    RUN_TEST(test_writeError_nullRequestIdAndAction_writeJsonNull);
    RUN_TEST(test_writeScanStartSuccess_hasExpectedShape);
    RUN_TEST(test_writeScanStopSuccess_hasExpectedShape);

    return UNITY_END();
}
