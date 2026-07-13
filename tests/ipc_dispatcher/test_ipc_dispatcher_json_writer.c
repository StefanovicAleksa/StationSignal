#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "cJSON.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_json_writer.h"

/*
 * Envelope shape asserted via cJSON_Parse + field inspection (not string
 * equality) - resilient to key ordering, per the plan's testing strategy.
 */

static IpcMessage* fixtureMessage;
static char* fixtureJson;
static cJSON* fixtureParsed;

void
setUp(void) {
    fixtureMessage = NULL;
    fixtureJson = NULL;
    fixtureParsed = NULL;
}

void
tearDown(void) {
    if (fixtureParsed) cJSON_Delete(fixtureParsed);
    free(fixtureJson);
    if (fixtureMessage) {
        free(fixtureMessage->sourceReference);
        for (int i = 0; i < fixtureMessage->dataPointCount; i++) {
            free(fixtureMessage->dataPoints[i].reference);
            if (fixtureMessage->dataPoints[i].value.type == IPC_SCALAR_STRING
                    || fixtureMessage->dataPoints[i].value.type == IPC_SCALAR_RAW) {
                free(fixtureMessage->dataPoints[i].value.value.str);
            }
            if (fixtureMessage->dataPoints[i].hasPreviousValue
                    && (fixtureMessage->dataPoints[i].previousValue.type == IPC_SCALAR_STRING
                            || fixtureMessage->dataPoints[i].previousValue.type == IPC_SCALAR_RAW)) {
                free(fixtureMessage->dataPoints[i].previousValue.value.str);
            }
        }
        free(fixtureMessage->dataPoints);
        free(fixtureMessage);
    }
}

static IpcMessage*
buildMessage(IpcSourceType sourceType, const char* sourceRef, bool hasBuffered, bool buffered,
        bool hasTimestamp, uint64_t timestampMs, int dataPointCount) {
    IpcMessage* message = calloc(1, sizeof(IpcMessage));
    message->sourceType = sourceType;
    message->sourceReference = sourceRef ? strdup(sourceRef) : NULL;
    message->hasBuffered = hasBuffered;
    message->buffered = buffered;
    message->hasTimestamp = hasTimestamp;
    message->timestampMs = timestampMs;
    message->dataPointCount = dataPointCount;
    message->dataPoints = dataPointCount > 0 ? calloc((size_t) dataPointCount, sizeof(IpcDataPoint)) : NULL;
    return message;
}

void
test_write_mmsReport_withQuality(void) {
    fixtureMessage = buildMessage(IPC_SOURCE_MMS_REPORT, "LD0/LLN0.BR.brcbMain", true, true, true, 1700000000000ULL, 1);
    fixtureMessage->dataPoints[0].reference = strdup("LD0/LLN0$ST$Ind1$stVal");
    fixtureMessage->dataPoints[0].value.type = IPC_SCALAR_BOOL;
    fixtureMessage->dataPoints[0].value.value.b = true;
    fixtureMessage->dataPoints[0].hasQuality = true;
    fixtureMessage->dataPoints[0].quality.validity = IPC_QUALITY_GOOD;
    fixtureMessage->dataPoints[0].quality.detailFlags = 0;

    fixtureJson = IpcDispatcherJsonWriter_write(fixtureMessage);
    TEST_ASSERT_NOT_NULL(fixtureJson);

    fixtureParsed = cJSON_Parse(fixtureJson);
    TEST_ASSERT_NOT_NULL(fixtureParsed);

    TEST_ASSERT_EQUAL_STRING("MMS_REPORT", cJSON_GetObjectItem(fixtureParsed, "type")->valuestring);

    cJSON* source = cJSON_GetObjectItem(fixtureParsed, "source");
    TEST_ASSERT_EQUAL_STRING("LD0/LLN0.BR.brcbMain", cJSON_GetObjectItem(source, "rcbReference")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(source, "buffered")));

    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(fixtureParsed, "hasTimestamp")));
    /* This vendored Unity build has double support excluded - plain C
     * comparison instead, exact here since this integer value round-trips
     * losslessly through cJSON's double-backed number type. */
    TEST_ASSERT_TRUE(cJSON_GetObjectItem(fixtureParsed, "timestampMs")->valuedouble == 1700000000000.0);

    cJSON* dataPoints = cJSON_GetObjectItem(fixtureParsed, "dataPoints");
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(dataPoints));

    cJSON* point = cJSON_GetArrayItem(dataPoints, 0);
    TEST_ASSERT_EQUAL_STRING("LD0/LLN0$ST$Ind1$stVal", cJSON_GetObjectItem(point, "reference")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(point, "value")));

    cJSON* quality = cJSON_GetObjectItem(point, "quality");
    TEST_ASSERT_FALSE(cJSON_IsNull(quality));
    TEST_ASSERT_EQUAL_STRING("GOOD", cJSON_GetObjectItem(quality, "validity")->valuestring);
}

void
test_write_goose_noBufferedField(void) {
    fixtureMessage = buildMessage(IPC_SOURCE_GOOSE, "Breaker1CB1/LLN0$GO$gcbStatus", false, false, true, 42, 0);

    fixtureJson = IpcDispatcherJsonWriter_write(fixtureMessage);
    fixtureParsed = cJSON_Parse(fixtureJson);

    TEST_ASSERT_EQUAL_STRING("GOOSE", cJSON_GetObjectItem(fixtureParsed, "type")->valuestring);

    cJSON* source = cJSON_GetObjectItem(fixtureParsed, "source");
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/LLN0$GO$gcbStatus", cJSON_GetObjectItem(source, "goCbRef")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(source, "buffered"));

    cJSON* dataPoints = cJSON_GetObjectItem(fixtureParsed, "dataPoints");
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(dataPoints));
}

void
test_write_hasTimestampFalse_omitsTimestampMsField(void) {
    fixtureMessage = buildMessage(IPC_SOURCE_MMS_REPORT, "rcb", true, false, false, 0, 0);

    fixtureJson = IpcDispatcherJsonWriter_write(fixtureMessage);
    fixtureParsed = cJSON_Parse(fixtureJson);

    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(fixtureParsed, "hasTimestamp")));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(fixtureParsed, "timestampMs"));
}

void
test_write_hasQualityFalse_emitsJsonNull(void) {
    fixtureMessage = buildMessage(IPC_SOURCE_MMS_REPORT, "rcb", true, false, false, 0, 1);
    fixtureMessage->dataPoints[0].reference = strdup("ref");
    fixtureMessage->dataPoints[0].value.type = IPC_SCALAR_INT64;
    fixtureMessage->dataPoints[0].value.value.i64 = 7;
    fixtureMessage->dataPoints[0].hasQuality = false;

    fixtureJson = IpcDispatcherJsonWriter_write(fixtureMessage);
    fixtureParsed = cJSON_Parse(fixtureJson);

    cJSON* dataPoints = cJSON_GetObjectItem(fixtureParsed, "dataPoints");
    cJSON* point = cJSON_GetArrayItem(dataPoints, 0);
    cJSON* quality = cJSON_GetObjectItem(point, "quality");

    TEST_ASSERT_NOT_NULL(quality);
    TEST_ASSERT_TRUE(cJSON_IsNull(quality));
}

void
test_write_previousValueQualityLabel_allPopulated(void) {
    fixtureMessage = buildMessage(IPC_SOURCE_MMS_REPORT, "rcb", true, false, false, 0, 1);
    fixtureMessage->dataPoints[0].reference = strdup("LD0/LLN0$ST$Pos$stVal");
    fixtureMessage->dataPoints[0].value.type = IPC_SCALAR_UINT64;
    fixtureMessage->dataPoints[0].value.value.u64 = 2;
    fixtureMessage->dataPoints[0].hasQuality = true;
    fixtureMessage->dataPoints[0].quality.validity = IPC_QUALITY_GOOD;

    fixtureMessage->dataPoints[0].hasPreviousValue = true;
    fixtureMessage->dataPoints[0].previousValue.type = IPC_SCALAR_UINT64;
    fixtureMessage->dataPoints[0].previousValue.value.u64 = 1;
    fixtureMessage->dataPoints[0].hasPreviousQuality = true;
    fixtureMessage->dataPoints[0].previousQuality.validity = IPC_QUALITY_INVALID;
    fixtureMessage->dataPoints[0].hasLabel = true;
    fixtureMessage->dataPoints[0].label = "on";
    fixtureMessage->dataPoints[0].hasPreviousLabel = true;
    fixtureMessage->dataPoints[0].previousLabel = "off";

    fixtureJson = IpcDispatcherJsonWriter_write(fixtureMessage);
    fixtureParsed = cJSON_Parse(fixtureJson);

    cJSON* dataPoints = cJSON_GetObjectItem(fixtureParsed, "dataPoints");
    cJSON* point = cJSON_GetArrayItem(dataPoints, 0);

    cJSON* previousValue = cJSON_GetObjectItem(point, "previousValue");
    TEST_ASSERT_FALSE(cJSON_IsNull(previousValue));
    TEST_ASSERT_TRUE(previousValue->valuedouble == 1.0);

    cJSON* previousQuality = cJSON_GetObjectItem(point, "previousQuality");
    TEST_ASSERT_FALSE(cJSON_IsNull(previousQuality));
    TEST_ASSERT_EQUAL_STRING("INVALID", cJSON_GetObjectItem(previousQuality, "validity")->valuestring);

    TEST_ASSERT_EQUAL_STRING("on", cJSON_GetObjectItem(point, "label")->valuestring);
    TEST_ASSERT_EQUAL_STRING("off", cJSON_GetObjectItem(point, "previousLabel")->valuestring);
}

void
test_write_previousValueQualityLabel_allAbsent_emitJsonNull(void) {
    fixtureMessage = buildMessage(IPC_SOURCE_MMS_REPORT, "rcb", true, false, false, 0, 1);
    fixtureMessage->dataPoints[0].reference = strdup("ref");
    fixtureMessage->dataPoints[0].value.type = IPC_SCALAR_BOOL;
    fixtureMessage->dataPoints[0].value.value.b = true;
    /* hasPreviousValue/hasPreviousQuality/hasLabel/hasPreviousLabel all
     * default false via buildMessage's calloc. */

    fixtureJson = IpcDispatcherJsonWriter_write(fixtureMessage);
    fixtureParsed = cJSON_Parse(fixtureJson);

    cJSON* dataPoints = cJSON_GetObjectItem(fixtureParsed, "dataPoints");
    cJSON* point = cJSON_GetArrayItem(dataPoints, 0);

    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(point, "previousValue")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(point, "previousQuality")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(point, "label")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(point, "previousLabel")));
}

void
test_write_nullSourceReference_emitsJsonNull(void) {
    fixtureMessage = buildMessage(IPC_SOURCE_GOOSE, NULL, false, false, false, 0, 0);

    fixtureJson = IpcDispatcherJsonWriter_write(fixtureMessage);
    fixtureParsed = cJSON_Parse(fixtureJson);

    cJSON* source = cJSON_GetObjectItem(fixtureParsed, "source");
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(source, "goCbRef")));
}

void
test_write_returnsNull_onNullMessage(void) {
    TEST_ASSERT_NULL(IpcDispatcherJsonWriter_write(NULL));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_write_mmsReport_withQuality);
    RUN_TEST(test_write_goose_noBufferedField);
    RUN_TEST(test_write_hasTimestampFalse_omitsTimestampMsField);
    RUN_TEST(test_write_hasQualityFalse_emitsJsonNull);
    RUN_TEST(test_write_previousValueQualityLabel_allPopulated);
    RUN_TEST(test_write_previousValueQualityLabel_allAbsent_emitJsonNull);
    RUN_TEST(test_write_nullSourceReference_emitsJsonNull);
    RUN_TEST(test_write_returnsNull_onNullMessage);

    return UNITY_END();
}
