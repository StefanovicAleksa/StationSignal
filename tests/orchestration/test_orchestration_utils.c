#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "orchestration/utils/orchestration_utils.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- OrchestrationUtils_safeStringDup ---- */

void
test_safeStringDup_returnsNull_whenInputIsNull(void) {
    TEST_ASSERT_NULL(OrchestrationUtils_safeStringDup(NULL));
}

void
test_safeStringDup_returnsIndependentCopy(void) {
    char original[] = "Reporter1";
    char* copy = OrchestrationUtils_safeStringDup(original);

    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("Reporter1", copy);
    TEST_ASSERT_TRUE(copy != original);

    original[0] = 'X';
    TEST_ASSERT_EQUAL_STRING("Reporter1", copy);

    free(copy);
}

/* ---- OrchestrationUtils_errorToString ---- */

void
test_errorToString_returnsNonNull_forEveryKnownError(void) {
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_OK));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_ERR_INVALID_ARGUMENT));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_ERR_OUT_OF_MEMORY));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_ERR_IPC_DISPATCHER_FAILED));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_ERR_BOOTSTRAP_FAILED));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_ERR_STAGING_FAILED));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_ERR_MODEL_LOAD_FAILED));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_ERR_REPORT_CLIENT_FAILED));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString(ORCHESTRATION_ERR_GOOSE_SUBSCRIBER_FAILED));
}

void
test_errorToString_returnsNonNull_forUnknownValue(void) {
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_errorToString((OrchestrationError) 9999));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_safeStringDup_returnsNull_whenInputIsNull);
    RUN_TEST(test_safeStringDup_returnsIndependentCopy);

    RUN_TEST(test_errorToString_returnsNonNull_forEveryKnownError);
    RUN_TEST(test_errorToString_returnsNonNull_forUnknownValue);

    return UNITY_END();
}
