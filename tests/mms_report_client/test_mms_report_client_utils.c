#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/mms_report_client/utils/mms_report_client_utils.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- cloneMmsValueArray ---- */

void
test_cloneMmsValueArray_null_whenSourceNull(void) {
    MmsValue** clones = MmsReportClientUtils_cloneMmsValueArray(NULL, 3);
    TEST_ASSERT_NULL(clones);
}

void
test_cloneMmsValueArray_null_whenCountNotPositive(void) {
    MmsValue* array = MmsValue_createEmptyArray(1);
    MmsValue_setElement(array, 0, MmsValue_newBoolean(true));

    TEST_ASSERT_NULL(MmsReportClientUtils_cloneMmsValueArray(array, 0));
    TEST_ASSERT_NULL(MmsReportClientUtils_cloneMmsValueArray(array, -1));

    MmsValue_delete(array);
}

void
test_cloneMmsValueArray_producesIndependentDeepCopies(void) {
    MmsValue* array = MmsValue_createEmptyArray(2);
    MmsValue_setElement(array, 0, MmsValue_newBoolean(true));
    MmsValue_setElement(array, 1, MmsValue_newIntegerFromInt32(42));

    MmsValue** clones = MmsReportClientUtils_cloneMmsValueArray(array, 2);
    TEST_ASSERT_NOT_NULL(clones);
    TEST_ASSERT_NOT_NULL(clones[0]);
    TEST_ASSERT_NOT_NULL(clones[1]);

    TEST_ASSERT_TRUE(MmsValue_getBoolean(clones[0]));
    TEST_ASSERT_EQUAL_INT32(42, MmsValue_toInt32(clones[1]));

    /* Mutate the original array's elements after cloning - the clones must be
     * unaffected, proving this is a deep copy and not an aliased reference. */
    MmsValue_setBoolean(MmsValue_getElement(array, 0), false);
    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getBoolean(clones[0]),
            "clone must be independent of the original array's later mutations");

    MmsValue_delete(clones[0]);
    MmsValue_delete(clones[1]);
    free(clones);
    MmsValue_delete(array);
}

/* ---- cloneReasonArray ---- */

void
test_cloneReasonArray_null_whenSourceNullOrCountNotPositive(void) {
    ReasonForInclusion src[1] = { IEC61850_REASON_DATA_CHANGE };
    TEST_ASSERT_NULL(MmsReportClientUtils_cloneReasonArray(NULL, 1));
    TEST_ASSERT_NULL(MmsReportClientUtils_cloneReasonArray(src, 0));
}

void
test_cloneReasonArray_producesIndependentCopy(void) {
    ReasonForInclusion src[2] = { IEC61850_REASON_DATA_CHANGE, IEC61850_REASON_GI };

    ReasonForInclusion* clone = MmsReportClientUtils_cloneReasonArray(src, 2);
    TEST_ASSERT_NOT_NULL(clone);
    TEST_ASSERT_EQUAL_INT(IEC61850_REASON_DATA_CHANGE, clone[0]);
    TEST_ASSERT_EQUAL_INT(IEC61850_REASON_GI, clone[1]);

    src[0] = IEC61850_REASON_INTEGRITY;
    TEST_ASSERT_EQUAL_INT_MESSAGE(IEC61850_REASON_DATA_CHANGE, clone[0],
            "clone must be independent of the original array's later mutations");

    free(clone);
}

/* ---- safeStringDup ---- */

void
test_safeStringDup_null_whenInputNull(void) {
    TEST_ASSERT_NULL(MmsReportClientUtils_safeStringDup(NULL));
}

void
test_safeStringDup_producesIndependentCopy(void) {
    char original[] = "brcbMain";
    char* copy = MmsReportClientUtils_safeStringDup(original);

    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("brcbMain", copy);
    TEST_ASSERT_TRUE(copy != original);

    original[0] = 'X';
    TEST_ASSERT_EQUAL_STRING_MESSAGE("brcbMain", copy,
            "clone must be independent of the original buffer's later mutations");

    free(copy);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_cloneMmsValueArray_null_whenSourceNull);
    RUN_TEST(test_cloneMmsValueArray_null_whenCountNotPositive);
    RUN_TEST(test_cloneMmsValueArray_producesIndependentDeepCopies);

    RUN_TEST(test_cloneReasonArray_null_whenSourceNullOrCountNotPositive);
    RUN_TEST(test_cloneReasonArray_producesIndependentCopy);

    RUN_TEST(test_safeStringDup_null_whenInputNull);
    RUN_TEST(test_safeStringDup_producesIndependentCopy);

    return UNITY_END();
}
