#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ipc_dispatcher/domain/ipc_dispatcher_usecases.h"

/*
 * Pure logic - hand-built plain arrays only, no MmsValue/cJSON/lws anywhere
 * near this file (see ipc_dispatcher_usecases.h's own doc comment).
 */

void
setUp(void) {}

void
tearDown(void) {}

/* ---- splitReference ---- */

void
test_splitReference_normalCase_splitsOnLastDollar(void) {
    size_t prefixLen;
    const char* daName;

    TEST_ASSERT_TRUE(IpcDispatcherUseCases_splitReference("LD0/LLN0$ST$Ind1$stVal", &prefixLen, &daName));
    TEST_ASSERT_EQUAL_INT(strlen("LD0/LLN0$ST$Ind1"), (int) prefixLen);
    TEST_ASSERT_EQUAL_STRING("stVal", daName);
}

void
test_splitReference_returnsFalse_onNull(void) {
    size_t prefixLen;
    const char* daName;
    TEST_ASSERT_FALSE(IpcDispatcherUseCases_splitReference(NULL, &prefixLen, &daName));
}

void
test_splitReference_returnsFalse_onNoDollar(void) {
    size_t prefixLen;
    const char* daName;
    TEST_ASSERT_FALSE(IpcDispatcherUseCases_splitReference("no-dollar-here", &prefixLen, &daName));
}

/* ---- pairQuality ---- */

void
test_pairQuality_pairsValueWithQSibling(void) {
    const char* refs[2] = { "LD0/LLN0$ST$Ind1$stVal", "LD0/LLN0$ST$Ind1$q" };
    int valueIdx[2], qualityIdx[2];

    int n = IpcDispatcherUseCases_pairQuality(refs, 2, valueIdx, qualityIdx);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, valueIdx[0]);
    TEST_ASSERT_EQUAL_INT(1, qualityIdx[0]);
}

void
test_pairQuality_valueOnly_noQInDataset(void) {
    const char* refs[1] = { "LD0/LLN0$ST$Ind1$stVal" };
    int valueIdx[1], qualityIdx[1];

    int n = IpcDispatcherUseCases_pairQuality(refs, 1, valueIdx, qualityIdx);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, valueIdx[0]);
    TEST_ASSERT_EQUAL_INT(-1, qualityIdx[0]);
}

void
test_pairQuality_loneQ_noSibling_isDropped(void) {
    const char* refs[1] = { "LD0/LLN0$ST$Ind1$q" };
    int valueIdx[1], qualityIdx[1];

    int n = IpcDispatcherUseCases_pairQuality(refs, 1, valueIdx, qualityIdx);

    TEST_ASSERT_EQUAL_INT(0, n);
}

void
test_pairQuality_nestedCmvChain_findsQualitySeveralAncestorLevelsUp(void) {
    /* Real device shape: a CMV's cVal->mag->f nests 3 "$"-segments below the
     * CMV instance ("phsA") that q/t/range actually belong to - a plain
     * last-"$"-strip would look for q at "...phsA$cVal$mag" and never find
     * it. */
    const char* refs[4] = {
        "LD0/MMXU1$MX$PhV$phsA$cVal$mag$f",
        "LD0/MMXU1$MX$PhV$phsA$q",
        "LD0/MMXU1$MX$PhV$phsA$t",
        "LD0/MMXU1$MX$PhV$phsA$range",
    };
    int valueIdx[4], qualityIdx[4];

    int n = IpcDispatcherUseCases_pairQuality(refs, 4, valueIdx, qualityIdx);

    TEST_ASSERT_EQUAL_INT(3, n); /* cVal.mag.f, t, range - q itself never emitted */
    for (int k = 0; k < n; k++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, qualityIdx[k], "every value entry must pair with the DO/SDO-level q");
    }
}

void
test_pairQuality_doesNotOverreach_pastAGenuinelyUnrelatedAncestor(void) {
    /* Two independent CMV instances (phsA, phsB), each with their own q -
     * phsA's nested value must never accidentally pair with phsB's q even
     * though "LD0/MMXU1$MX$PhV" is a shared ancestor of both. */
    const char* refs[2] = {
        "LD0/MMXU1$MX$PhV$phsA$cVal$mag$f",
        "LD0/MMXU1$MX$PhV$phsB$q",
    };
    int valueIdx[1], qualityIdx[1];

    int n = IpcDispatcherUseCases_pairQuality(refs, 2, valueIdx, qualityIdx);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(-1, qualityIdx[0]);
}

void
test_pairQuality_unparseableOrNullReference_passesThroughAsValue(void) {
    const char* refs[2] = { NULL, "no-dollar-here" };
    int valueIdx[2], qualityIdx[2];

    int n = IpcDispatcherUseCases_pairQuality(refs, 2, valueIdx, qualityIdx);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, valueIdx[0]);
    TEST_ASSERT_EQUAL_INT(-1, qualityIdx[0]);
    TEST_ASSERT_EQUAL_INT(1, valueIdx[1]);
    TEST_ASSERT_EQUAL_INT(-1, qualityIdx[1]);
}

void
test_pairQuality_multipleIndependentGroups_doNotCrossMix(void) {
    const char* refs[3] = {
        "LD0/LLN0$ST$Ind1$stVal",
        "LD0/LLN0$ST$Ind1$q",
        "LD0/LLN0$ST$Ind2$stVal"
    };
    int valueIdx[3], qualityIdx[3];

    int n = IpcDispatcherUseCases_pairQuality(refs, 3, valueIdx, qualityIdx);

    TEST_ASSERT_EQUAL_INT(2, n);
    /* Ind1$stVal paired with Ind1$q */
    TEST_ASSERT_EQUAL_INT(0, valueIdx[0]);
    TEST_ASSERT_EQUAL_INT(1, qualityIdx[0]);
    /* Ind2$stVal has no q sibling - must not pick up Ind1's q */
    TEST_ASSERT_EQUAL_INT(2, valueIdx[1]);
    TEST_ASSERT_EQUAL_INT(-1, qualityIdx[1]);
}

void
test_pairQuality_zeroCount_returnsZero(void) {
    int valueIdx[1], qualityIdx[1];
    TEST_ASSERT_EQUAL_INT(0, IpcDispatcherUseCases_pairQuality(NULL, 0, valueIdx, qualityIdx));
}

/* ---- shouldIncludeValuePoint ---- */

void
test_shouldIncludeValuePoint_ownChange_alwaysIncluded(void) {
    TEST_ASSERT_TRUE(IpcDispatcherUseCases_shouldIncludeValuePoint(true, false, false));
    TEST_ASSERT_TRUE(IpcDispatcherUseCases_shouldIncludeValuePoint(true, true, false));
    TEST_ASSERT_TRUE(IpcDispatcherUseCases_shouldIncludeValuePoint(true, true, true));
}

void
test_shouldIncludeValuePoint_noOwnChange_noQualityPair_isExcluded(void) {
    /* e.g. a DPC's "t" - no quality sibling in this record at all, and its
     * own value didn't change - purely along for the group's ride. */
    TEST_ASSERT_FALSE(IpcDispatcherUseCases_shouldIncludeValuePoint(false, false, false));
}

void
test_shouldIncludeValuePoint_noOwnChange_qualityUnchanged_isExcluded(void) {
    /* This is the reported bug: a DPC's "t"/"stSeld" dragged in only
     * because their "stVal" sibling changed - neither their own value nor
     * the shared quality actually changed, so they must be suppressed. */
    TEST_ASSERT_FALSE(IpcDispatcherUseCases_shouldIncludeValuePoint(false, true, false));
}

void
test_shouldIncludeValuePoint_noOwnChange_qualityChanged_isIncluded(void) {
    /* A genuine quality-only change must still surface its value sibling -
     * preserves mms_report_client's existing, deliberately-symmetric
     * group-extension behavior for this specific case. */
    TEST_ASSERT_TRUE(IpcDispatcherUseCases_shouldIncludeValuePoint(false, true, true));
}

/* ---- assembleMessage / freeMessage ---- */

void
test_assembleMessage_deepCopiesEverything_notAliased(void) {
    char ref[] = "LD0/LLN0$ST$Ind1$stVal";
    char sourceRef[] = "LD0/LLN0.BR.brcbMain";
    const char* pointRefs[1] = { ref };

    IpcScalarValue value;
    value.type = IPC_SCALAR_BOOL;
    value.value.b = true;

    bool hasQuality = true;
    IpcQuality quality = { IPC_QUALITY_GOOD, 0 };

    IpcMessage* message = IpcDispatcherUseCases_assembleMessage(
            IPC_SOURCE_MMS_REPORT, sourceRef,
            true, true, true, 123456789ULL,
            pointRefs, &value, &hasQuality, &quality, NULL, 1);

    TEST_ASSERT_NOT_NULL(message);
    TEST_ASSERT_EQUAL_INT(IPC_SOURCE_MMS_REPORT, message->sourceType);
    TEST_ASSERT_EQUAL_STRING(sourceRef, message->sourceReference);
    TEST_ASSERT_TRUE(message->hasBuffered);
    TEST_ASSERT_TRUE(message->buffered);
    TEST_ASSERT_TRUE(message->hasTimestamp);
    TEST_ASSERT_EQUAL_UINT64(123456789ULL, message->timestampMs);
    TEST_ASSERT_EQUAL_INT(1, message->dataPointCount);
    TEST_ASSERT_EQUAL_STRING(ref, message->dataPoints[0].reference);
    TEST_ASSERT_TRUE(message->dataPoints[0].hasQuality);
    TEST_ASSERT_EQUAL_INT(IPC_QUALITY_GOOD, message->dataPoints[0].quality.validity);

    /* Mutate the source buffers after the call - message must be unaffected. */
    ref[0] = 'X';
    sourceRef[0] = 'X';
    TEST_ASSERT_EQUAL_STRING_MESSAGE("LD0/LLN0$ST$Ind1$stVal", message->dataPoints[0].reference,
            "data point reference must be a deep copy");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("LD0/LLN0.BR.brcbMain", message->sourceReference,
            "source reference must be a deep copy");

    IpcDispatcherUseCases_freeMessage(message);
}

void
test_assembleMessage_stringScalar_isDeepCopied(void) {
    char text[] = "hello";
    IpcScalarValue value;
    value.type = IPC_SCALAR_STRING;
    value.value.str = text;

    IpcMessage* message = IpcDispatcherUseCases_assembleMessage(
            IPC_SOURCE_GOOSE, NULL, false, false, true, 1,
            (const char* const[]) { "ref" }, &value, NULL, NULL, NULL, 1);

    TEST_ASSERT_NOT_NULL(message);
    TEST_ASSERT_EQUAL_STRING("hello", message->dataPoints[0].value.value.str);

    text[0] = 'X';
    TEST_ASSERT_EQUAL_STRING_MESSAGE("hello", message->dataPoints[0].value.value.str,
            "string scalar must be a deep copy");

    IpcDispatcherUseCases_freeMessage(message);
}

void
test_assembleMessage_nullSourceReference_copiedThroughAsNull(void) {
    IpcMessage* message = IpcDispatcherUseCases_assembleMessage(
            IPC_SOURCE_GOOSE, NULL, false, false, false, 0, NULL, NULL, NULL, NULL, NULL, 0);

    TEST_ASSERT_NOT_NULL(message);
    TEST_ASSERT_NULL(message->sourceReference);
    TEST_ASSERT_EQUAL_INT(0, message->dataPointCount);
    TEST_ASSERT_NULL(message->dataPoints);

    IpcDispatcherUseCases_freeMessage(message);
}

void
test_assembleMessage_noQuality_hasQualityFalse(void) {
    const char* pointRefs[1] = { "ref" };
    IpcScalarValue value;
    value.type = IPC_SCALAR_BOOL;
    value.value.b = false;

    IpcMessage* message = IpcDispatcherUseCases_assembleMessage(
            IPC_SOURCE_MMS_REPORT, "rcb", true, false, false, 0,
            pointRefs, &value, NULL, NULL, NULL, 1);

    TEST_ASSERT_NOT_NULL(message);
    TEST_ASSERT_FALSE(message->dataPoints[0].hasQuality);

    IpcDispatcherUseCases_freeMessage(message);
}

void
test_assembleMessage_extrasNull_everyHasFlagFalse(void) {
    const char* pointRefs[1] = { "ref" };
    IpcScalarValue value;
    value.type = IPC_SCALAR_BOOL;
    value.value.b = true;

    IpcMessage* message = IpcDispatcherUseCases_assembleMessage(
            IPC_SOURCE_MMS_REPORT, "rcb", false, false, false, 0,
            pointRefs, &value, NULL, NULL, NULL, 1);

    TEST_ASSERT_NOT_NULL(message);
    TEST_ASSERT_FALSE(message->dataPoints[0].hasPreviousValue);
    TEST_ASSERT_FALSE(message->dataPoints[0].hasPreviousQuality);

    IpcDispatcherUseCases_freeMessage(message);
}

void
test_assembleMessage_extrasPopulated_roundTripsEveryField(void) {
    const char* pointRefs[1] = { "ref" };
    IpcScalarValue value;
    value.type = IPC_SCALAR_BOOL;
    value.value.b = true;

    bool hasPreviousValue = true;
    char prevText[] = "was-off";
    IpcScalarValue previousValue;
    previousValue.type = IPC_SCALAR_STRING;
    previousValue.value.str = prevText;

    bool hasPreviousQuality = true;
    IpcQuality previousQuality = { IPC_QUALITY_INVALID, 7 };

    IpcDataPointExtras extras = {
        .pointHasPreviousValue = &hasPreviousValue,
        .pointPreviousValue = &previousValue,
        .pointHasPreviousQuality = &hasPreviousQuality,
        .pointPreviousQuality = &previousQuality,
    };

    IpcMessage* message = IpcDispatcherUseCases_assembleMessage(
            IPC_SOURCE_MMS_REPORT, "rcb", false, false, false, 0,
            pointRefs, &value, NULL, NULL, &extras, 1);

    TEST_ASSERT_NOT_NULL(message);
    TEST_ASSERT_TRUE(message->dataPoints[0].hasPreviousValue);
    TEST_ASSERT_EQUAL_STRING("was-off", message->dataPoints[0].previousValue.value.str);
    TEST_ASSERT_TRUE_MESSAGE(message->dataPoints[0].previousValue.value.str != prevText,
            "previousValue string must be a deep copy, not aliased");
    TEST_ASSERT_TRUE(message->dataPoints[0].hasPreviousQuality);
    TEST_ASSERT_EQUAL_INT(IPC_QUALITY_INVALID, message->dataPoints[0].previousQuality.validity);
    TEST_ASSERT_EQUAL_INT(7, message->dataPoints[0].previousQuality.detailFlags);

    IpcDispatcherUseCases_freeMessage(message);
}

void
test_freeMessage_doesNotCrash_onNull(void) {
    IpcDispatcherUseCases_freeMessage(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_splitReference_normalCase_splitsOnLastDollar);
    RUN_TEST(test_splitReference_returnsFalse_onNull);
    RUN_TEST(test_splitReference_returnsFalse_onNoDollar);

    RUN_TEST(test_pairQuality_pairsValueWithQSibling);
    RUN_TEST(test_pairQuality_valueOnly_noQInDataset);
    RUN_TEST(test_pairQuality_loneQ_noSibling_isDropped);
    RUN_TEST(test_pairQuality_nestedCmvChain_findsQualitySeveralAncestorLevelsUp);
    RUN_TEST(test_pairQuality_doesNotOverreach_pastAGenuinelyUnrelatedAncestor);
    RUN_TEST(test_pairQuality_unparseableOrNullReference_passesThroughAsValue);
    RUN_TEST(test_pairQuality_multipleIndependentGroups_doNotCrossMix);
    RUN_TEST(test_pairQuality_zeroCount_returnsZero);

    RUN_TEST(test_shouldIncludeValuePoint_ownChange_alwaysIncluded);
    RUN_TEST(test_shouldIncludeValuePoint_noOwnChange_noQualityPair_isExcluded);
    RUN_TEST(test_shouldIncludeValuePoint_noOwnChange_qualityUnchanged_isExcluded);
    RUN_TEST(test_shouldIncludeValuePoint_noOwnChange_qualityChanged_isIncluded);

    RUN_TEST(test_assembleMessage_deepCopiesEverything_notAliased);
    RUN_TEST(test_assembleMessage_stringScalar_isDeepCopied);
    RUN_TEST(test_assembleMessage_nullSourceReference_copiedThroughAsNull);
    RUN_TEST(test_assembleMessage_noQuality_hasQualityFalse);
    RUN_TEST(test_assembleMessage_extrasNull_everyHasFlagFalse);
    RUN_TEST(test_assembleMessage_extrasPopulated_roundTripsEveryField);
    RUN_TEST(test_freeMessage_doesNotCrash_onNull);

    return UNITY_END();
}
