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

/* ---- OrchestrationUtils_stageToString ---- */

void
test_stageToString_returnsNonNull_forEveryKnownStage(void) {
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_NONE));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_IPC_DISPATCHER_START));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_BOOTSTRAP));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_STAGING));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_IED_NAME_RESOLUTION));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_MODEL_LOAD));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_ONLINE_DISCOVERY));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_REPORT_CLIENT_START));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString(ORCHESTRATION_STAGE_GOOSE_SUBSCRIBER_START));
}

void
test_stageToString_returnsNonNull_forUnknownValue(void) {
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_stageToString((OrchestrationStage) 9999));
}

/* ---- OrchestrationUtils_candidateStatusToString ---- */

void
test_candidateStatusToString_returnsNonNull_forEveryKnownStatus(void) {
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_candidateStatusToString(SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_candidateStatusToString(SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_candidateStatusToString(SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_candidateStatusToString(SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_candidateStatusToString(SCL_BOOTSTRAP_CANDIDATE_DOWNLOAD_FAILED));
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_candidateStatusToString(SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED));
}

void
test_candidateStatusToString_returnsNonNull_forUnknownValue(void) {
    TEST_ASSERT_NOT_NULL(OrchestrationUtils_candidateStatusToString((SclBootstrapCandidateStatus) 9999));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_safeStringDup_returnsNull_whenInputIsNull);
    RUN_TEST(test_safeStringDup_returnsIndependentCopy);

    RUN_TEST(test_errorToString_returnsNonNull_forEveryKnownError);
    RUN_TEST(test_errorToString_returnsNonNull_forUnknownValue);

    RUN_TEST(test_stageToString_returnsNonNull_forEveryKnownStage);
    RUN_TEST(test_stageToString_returnsNonNull_forUnknownValue);

    RUN_TEST(test_candidateStatusToString_returnsNonNull_forEveryKnownStatus);
    RUN_TEST(test_candidateStatusToString_returnsNonNull_forUnknownValue);

    return UNITY_END();
}
