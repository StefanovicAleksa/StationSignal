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
    /* 'U' is not an IEC 61850-7-4 group letter at all - it shows up in the
     * field only as the first letter of vendor classes like Siemens' "USER".
     * (This case used to use 'Q', which is now a real Ed2 group - see
     * test_forGroupLetter_ed2Groups_qMeasurementFAndKOther.) */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('U'));
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

/* ---- IEC 61850-7-4 Ed2 group letters (Q/F/K) ---- */

void
test_forGroupLetter_ed2Groups_qMeasurementFAndKOther(void) {
    /* Q = power quality (QVVR/QVUB/QITR/QIUB) - measurements of supply
     * quality, bucketed with M/T rather than left in the catch-all. */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forGroupLetter('Q'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forLnClass("QVVR"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forLnClass("QVUB"));

    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('F'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('K'));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forLnClass("FCNT"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forLnClass("KVLV"));
}

/* Domain-extension groups are deliberately NOT in the table - they must still
 * degrade to OTHER rather than being mis-bucketed by some accidental match. */
void
test_forGroupLetter_domainExtensionGroups_stillOther(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('D')); /* DER, 7-420 */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('H')); /* hydro, 7-410 */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('W')); /* wind, 61400-25 */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forGroupLetter('U')); /* not a group at all */
}

/* ---- forWireInstanceName's 4-character structural fallback ---- */

/* Every one of these is a real class observed in a real station file that the
 * dictionary does not contain - the whole point of the fallback. */
void
test_forWireInstanceName_unknownClass_classifiedByFourCharFallback(void) {
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forWireInstanceName("QVVR1"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forWireInstanceName("QVUB1"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, IedModelLnCategory_forWireInstanceName("PTUF1"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, IedModelLnCategory_forWireInstanceName("PSOF1"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forWireInstanceName("CBAY1"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName("LPDI37"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName("LPDO24"));
}

void
test_forWireInstanceName_fourCharFallback_readsPastAVendorPrefix(void) {
    /* The class is the LAST four characters, so an arbitrary prefix glued in
     * front is skipped - taking the FIRST letter instead would read 'M' here
     * and wrongly say MEASUREMENT. */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT, IedModelLnCategory_forWireInstanceName("MyBkrQVVR2"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forWireInstanceName("TR8CBAY1"));
}

void
test_forWireInstanceName_dictionaryStillWinsOverFallback(void) {
    /* Both stages would fire for a prefixed standard class, and they disagree:
     * the dictionary reads "PTOC" (PROTECTION), the fallback would read the
     * last four characters "kPTO"... so the dictionary must run first. */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION, IedModelLnCategory_forWireInstanceName("MyBkrPTOC2"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL, IedModelLnCategory_forWireInstanceName("Q28XCBR1"));
}

void
test_forWireInstanceName_tooShortForFallback_returnsOther(void) {
    /* Fewer than four characters left after stripping the inst - there is no
     * class name to read, so no guess is made. */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName("XY1"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName("PQ"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forWireInstanceName("123"));
}

/* ---- isAlwaysIncluded* (the LLN0 filter exemption) ---- */

void
test_isAlwaysIncludedLnClass_onlyLln0(void) {
    TEST_ASSERT_TRUE(IedModelLnCategory_isAlwaysIncludedLnClass("LLN0"));

    /* Deliberately NOT the rest of the 'L' group, nor anything else that
     * merely classifies OTHER - the exemption is about an LD's own status
     * node, not about the category it happens to land in. */
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedLnClass("LPHD"));
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedLnClass("LGOS"));
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedLnClass("GGIO"));
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedLnClass("XCBR"));
}

void
test_isAlwaysIncludedLnClass_stillClassifiesAsOther(void) {
    /* The exemption is orthogonal to the category: LLN0 must keep reporting
     * OTHER, since that value is what ipc_dispatcher puts on the wire. */
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER, IedModelLnCategory_forLnClass("LLN0"));
}

void
test_isAlwaysIncludedLnClass_nullOrEmpty_returnsFalse(void) {
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedLnClass(NULL));
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedLnClass(""));
}

void
test_isAlwaysIncludedWireInstanceName_matchesRawLln0(void) {
    /* The wire form of LLN0 is the bare class name - no prefix, no inst, per
     * IEC 61850-6 - which is exactly why this can't route through
     * forWireInstanceName's digit-strip (that would leave "LLN"). */
    TEST_ASSERT_TRUE(IedModelLnCategory_isAlwaysIncludedWireInstanceName("LLN0"));

    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedWireInstanceName("LLN"));
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedWireInstanceName("LPHD1"));
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedWireInstanceName("XCBR1"));
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedWireInstanceName(NULL));
    TEST_ASSERT_FALSE(IedModelLnCategory_isAlwaysIncludedWireInstanceName(""));
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

    RUN_TEST(test_forGroupLetter_ed2Groups_qMeasurementFAndKOther);
    RUN_TEST(test_forGroupLetter_domainExtensionGroups_stillOther);

    RUN_TEST(test_forWireInstanceName_unknownClass_classifiedByFourCharFallback);
    RUN_TEST(test_forWireInstanceName_fourCharFallback_readsPastAVendorPrefix);
    RUN_TEST(test_forWireInstanceName_dictionaryStillWinsOverFallback);
    RUN_TEST(test_forWireInstanceName_tooShortForFallback_returnsOther);

    RUN_TEST(test_isAlwaysIncludedLnClass_onlyLln0);
    RUN_TEST(test_isAlwaysIncludedLnClass_stillClassifiesAsOther);
    RUN_TEST(test_isAlwaysIncludedLnClass_nullOrEmpty_returnsFalse);
    RUN_TEST(test_isAlwaysIncludedWireInstanceName_matchesRawLln0);

    return UNITY_END();
}
