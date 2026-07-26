#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "cJSON.h"
#include "features/scan_dispatcher/data/scan_dispatcher_json_writer.h"

/*
 * Envelope shape asserted via cJSON_Parse + field inspection (not string
 * equality) - resilient to key ordering, same convention as
 * test_ipc_dispatcher_json_writer.c.
 */

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
test_write_roundTripsAllFields(void) {
    ScanDeviceFoundEvent event = { 7, (char*) "192.168.1.50", 102, 1700000000000ULL, false };

    fixtureJson = ScanDispatcherJsonWriter_write(&event);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    /* This vendored Unity build has double support excluded - plain C
     * comparison instead, exact here since these integer values round-trip
     * losslessly through cJSON's double-backed number type. */
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(fixtureParsed, "schemaVersion")->valuedouble == 1.0);
    TEST_ASSERT_EQUAL_STRING("SCAN_RESULT", cJSON_GetObjectItem(fixtureParsed, "type")->valuestring);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(fixtureParsed, "scanId")->valuedouble == 7.0);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", cJSON_GetObjectItem(fixtureParsed, "host")->valuestring);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(fixtureParsed, "mmsPort")->valuedouble == 102.0);
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(fixtureParsed, "discoveredAtMs")->valuedouble == 1700000000000.0);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(fixtureParsed, "authRequired")));
}

void
test_write_authRequiredTrue_roundTrips(void) {
    ScanDeviceFoundEvent event = { 7, (char*) "192.168.1.50", 102, 1700000000000ULL, true };

    fixtureJson = ScanDispatcherJsonWriter_write(&event);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(fixtureParsed, "authRequired")));
}

void
test_write_returnsNull_onNullEvent(void) {
    TEST_ASSERT_NULL(ScanDispatcherJsonWriter_write(NULL));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_write_roundTripsAllFields);
    RUN_TEST(test_write_authRequiredTrue_roundTrips);
    RUN_TEST(test_write_returnsNull_onNullEvent);

    return UNITY_END();
}
