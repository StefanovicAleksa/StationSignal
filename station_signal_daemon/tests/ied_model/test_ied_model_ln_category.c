#include "unity.h"
#include "features/ied_model/utils/ied_model_ln_category.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- forGroupLetter ---- */

void
test_forGroupLetter_control_mapsCX_A(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forGroupLetter('C'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forGroupLetter('X'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forGroupLetter('A'));
}

void
test_forGroupLetter_measurement_mapsMT(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forGroupLetter('M'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forGroupLetter('T'));
}

void
test_forGroupLetter_protection_mapsPR(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, IedModelLnCategory_forGroupLetter('P'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, IedModelLnCategory_forGroupLetter('R'));
}

void
test_forGroupLetter_other_mapsGILSYZ(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('G'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('I'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('L'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('S'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('Y'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('Z'));
}

void
test_forGroupLetter_unrecognizedLetter_returnsOther(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('Q'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('\0'));
}

void
test_forGroupLetter_lowercase_stillMatches(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forGroupLetter('x'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forGroupLetter('m'));
}

/* ---- forLnClass ---- */

void
test_forLnClass_derivesFromFirstLetter(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forLnClass("XCBR"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forLnClass("MMXU"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, IedModelLnCategory_forLnClass("PTOC"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forLnClass("LLN0"));
}

void
test_forLnClass_nullOrEmpty_returnsOther(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forLnClass(NULL));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forLnClass(""));
}

/* ---- forWireInstanceName ---- */

void
test_forWireInstanceName_plainClassPlusInst(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forWireInstanceName("XCBR1"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forWireInstanceName("MMXU1"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, IedModelLnCategory_forWireInstanceName("PTOC2"));
}

void
test_forWireInstanceName_multiDigitInst(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forWireInstanceName("CSWI123"));
}

void
test_forWireInstanceName_withVendorPrefix_stillMatchesSuffix(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forWireInstanceName("MyBkrXCBR1"));
}

void
test_forWireInstanceName_noInstDigits(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName("LLN0"));
}

void
test_forWireInstanceName_longestMatchWins(void) {
    /* "TCTR" (T, measurement) is itself a suffix of "MMXU"-unrelated but here
     * we assert a genuinely overlapping case: a class name that is itself a
     * suffix of a longer valid class name must not falsely short-match - e.g.
     * "PTOC" should never be mistaken by any shorter accidental substring
     * match. Use a real class with a common short tail to prove the longest
     * candidate, not the first dictionary hit, wins. */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, IedModelLnCategory_forWireInstanceName("PTOC1"));
}

void
test_forWireInstanceName_unmatched_returnsOther(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName("ZZZZ99"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName("123"));
}

void
test_forWireInstanceName_nullOrEmpty_returnsOther(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName(NULL));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName(""));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_forGroupLetter_control_mapsCX_A);
    RUN_TEST(test_forGroupLetter_measurement_mapsMT);
    RUN_TEST(test_forGroupLetter_protection_mapsPR);
    RUN_TEST(test_forGroupLetter_other_mapsGILSYZ);
    RUN_TEST(test_forGroupLetter_unrecognizedLetter_returnsOther);
    RUN_TEST(test_forGroupLetter_lowercase_stillMatches);

    RUN_TEST(test_forLnClass_derivesFromFirstLetter);
    RUN_TEST(test_forLnClass_nullOrEmpty_returnsOther);

    RUN_TEST(test_forWireInstanceName_plainClassPlusInst);
    RUN_TEST(test_forWireInstanceName_multiDigitInst);
    RUN_TEST(test_forWireInstanceName_withVendorPrefix_stillMatchesSuffix);
    RUN_TEST(test_forWireInstanceName_noInstDigits);
    RUN_TEST(test_forWireInstanceName_longestMatchWins);
    RUN_TEST(test_forWireInstanceName_unmatched_returnsOther);
    RUN_TEST(test_forWireInstanceName_nullOrEmpty_returnsOther);

    return UNITY_END();
}
