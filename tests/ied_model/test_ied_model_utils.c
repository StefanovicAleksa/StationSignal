#include "unity.h"
#include "features/ied_model/utils/ied_model_utils.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- attrOrDefault ---- */

void
test_attrOrDefault_returnsAttrValue_whenPresent(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");
    mxmlElementSetAttr(node, "foo", "bar");

    TEST_ASSERT_EQUAL_STRING("bar", IedModelUtils_attrOrDefault(node, "foo", "fallback"));

    mxmlDelete(node);
}

void
test_attrOrDefault_returnsDefault_whenAbsent(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");

    TEST_ASSERT_EQUAL_STRING("fallback", IedModelUtils_attrOrDefault(node, "foo", "fallback"));

    mxmlDelete(node);
}

void
test_attrOrDefault_returnsNull_whenAbsentAndDefaultIsNull(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");

    TEST_ASSERT_NULL(IedModelUtils_attrOrDefault(node, "foo", NULL));

    mxmlDelete(node);
}

/* ---- attrRequired ---- */

void
test_attrRequired_returnsValue_whenPresent(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");
    mxmlElementSetAttr(node, "name", "gcbEvents");

    TEST_ASSERT_EQUAL_STRING("gcbEvents", IedModelUtils_attrRequired(node, "name"));

    mxmlDelete(node);
}

void
test_attrRequired_returnsNull_whenAbsent(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");

    TEST_ASSERT_NULL(IedModelUtils_attrRequired(node, "name"));

    mxmlDelete(node);
}

/* ---- attrBool ---- */

void
test_attrBool_true_whenValueIsLiteralTrue(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");
    mxmlElementSetAttr(node, "dchg", "true");

    TEST_ASSERT_TRUE(IedModelUtils_attrBool(node, "dchg", false));

    mxmlDelete(node);
}

void
test_attrBool_true_whenValueIsOne(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");
    mxmlElementSetAttr(node, "dchg", "1");

    TEST_ASSERT_TRUE(IedModelUtils_attrBool(node, "dchg", false));

    mxmlDelete(node);
}

void
test_attrBool_returnsDefault_whenAbsent(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");

    TEST_ASSERT_TRUE(IedModelUtils_attrBool(node, "dchg", true));
    TEST_ASSERT_FALSE(IedModelUtils_attrBool(node, "dchg", false));

    mxmlDelete(node);
}

void
test_attrBool_false_whenValuePresentButNotTruthy_evenIfDefaultIsTrue(void) {
    /* Edge case worth locking in: defaultValue only applies when the attribute is
     * ABSENT. A present-but-non-"true"/"1" value (e.g. explicit "false") always
     * yields false regardless of what the caller passed as defaultValue. */
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");
    mxmlElementSetAttr(node, "dchg", "false");

    TEST_ASSERT_FALSE(IedModelUtils_attrBool(node, "dchg", true));

    mxmlDelete(node);
}

/* ---- attrInt ---- */

void
test_attrInt_parsesNumericValue(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");
    mxmlElementSetAttr(node, "confRev", "42");

    TEST_ASSERT_EQUAL_INT(42, IedModelUtils_attrInt(node, "confRev", -1));

    mxmlDelete(node);
}

void
test_attrInt_parsesNegativeValue(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");
    mxmlElementSetAttr(node, "minTime", "-1");

    TEST_ASSERT_EQUAL_INT(-1, IedModelUtils_attrInt(node, "minTime", 0));

    mxmlDelete(node);
}

void
test_attrInt_returnsDefault_whenAbsent(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");

    TEST_ASSERT_EQUAL_INT(7, IedModelUtils_attrInt(node, "confRev", 7));

    mxmlDelete(node);
}

void
test_attrInt_returnsZero_forNonNumericGarbage(void) {
    /* Documents current behavior (atoi's weak error handling) rather than an
     * ideal one - a guard against silent regressions, not a claim this is
     * the "right" answer for malformed input. */
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "Test");
    mxmlElementSetAttr(node, "confRev", "not-a-number");

    TEST_ASSERT_EQUAL_INT(0, IedModelUtils_attrInt(node, "confRev", 99));

    mxmlDelete(node);
}

/* ---- buildLnName ---- */

void
test_buildLnName_concatenatesPrefixClassInst(void) {
    char* name = IedModelUtils_buildLnName("Q", "XCBR", "1");

    TEST_ASSERT_EQUAL_STRING("QXCBR1", name);

    free(name);
}

void
test_buildLnName_handlesLLN0EmptyPrefixAndInst(void) {
    char* name = IedModelUtils_buildLnName("", "LLN0", "");

    TEST_ASSERT_EQUAL_STRING("LLN0", name);

    free(name);
}

/* ---- mapBType ---- */

void
test_mapBType_mapsCommonPrimitives(void) {
    TEST_ASSERT_EQUAL(IEC61850_BOOLEAN, IedModelUtils_mapBType("BOOLEAN"));
    TEST_ASSERT_EQUAL(IEC61850_FLOAT32, IedModelUtils_mapBType("FLOAT32"));
    TEST_ASSERT_EQUAL(IEC61850_ENUMERATED, IedModelUtils_mapBType("Enum"));
    TEST_ASSERT_EQUAL(IEC61850_CONSTRUCTED, IedModelUtils_mapBType("Struct"));
}

void
test_mapBType_appliesDocumentedApproximations(void) {
    /* Locks in the two deliberate approximations documented in
     * ied_model_utils.c: signed INT24 -> INT32, ObjRef -> VisString129. */
    TEST_ASSERT_EQUAL(IEC61850_INT32, IedModelUtils_mapBType("INT24"));
    TEST_ASSERT_EQUAL(IEC61850_VISIBLE_STRING_129, IedModelUtils_mapBType("ObjRef"));
}

void
test_mapBType_returnsUnknown_forUnrecognizedString(void) {
    TEST_ASSERT_EQUAL(IEC61850_UNKNOWN_TYPE, IedModelUtils_mapBType("NotARealBType"));
}

/* ---- buildTrgOps ---- */

void
test_buildTrgOps_returnsZero_whenNodeIsNull(void) {
    TEST_ASSERT_EQUAL_UINT8(0, IedModelUtils_buildTrgOps(NULL));
}

void
test_buildTrgOps_setsBitsForPresentTrueFlags(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "TrgOps");
    mxmlElementSetAttr(node, "dchg", "true");
    mxmlElementSetAttr(node, "qchg", "true");

    uint8_t trgOps = IedModelUtils_buildTrgOps(node);

    TEST_ASSERT_TRUE(trgOps & TRG_OPT_DATA_CHANGED);
    TEST_ASSERT_TRUE(trgOps & TRG_OPT_QUALITY_CHANGED);
    TEST_ASSERT_FALSE(trgOps & TRG_OPT_DATA_UPDATE);
    TEST_ASSERT_FALSE(trgOps & TRG_OPT_INTEGRITY);
    TEST_ASSERT_FALSE(trgOps & TRG_OPT_GI);

    mxmlDelete(node);
}

void
test_buildTrgOps_setsAllBits_whenAllFlagsTrue(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "TrgOps");
    mxmlElementSetAttr(node, "dchg", "true");
    mxmlElementSetAttr(node, "qchg", "true");
    mxmlElementSetAttr(node, "dupd", "true");
    mxmlElementSetAttr(node, "period", "true");
    mxmlElementSetAttr(node, "gi", "true");

    uint8_t expected = TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_DATA_UPDATE
            | TRG_OPT_INTEGRITY | TRG_OPT_GI;

    TEST_ASSERT_EQUAL_UINT8(expected, IedModelUtils_buildTrgOps(node));

    mxmlDelete(node);
}

/* ---- buildOptFlds ---- */

void
test_buildOptFlds_returnsZero_whenNodeIsNull(void) {
    TEST_ASSERT_EQUAL_UINT8(0, IedModelUtils_buildOptFlds(NULL));
}

void
test_buildOptFlds_setsBitsForPresentTrueFlags(void) {
    mxml_node_t* node = mxmlNewElement(MXML_NO_PARENT, "OptFields");
    mxmlElementSetAttr(node, "seqNum", "true");
    mxmlElementSetAttr(node, "timeStamp", "true");
    mxmlElementSetAttr(node, "bufOvfl", "false");

    uint8_t optFlds = IedModelUtils_buildOptFlds(node);

    TEST_ASSERT_TRUE(optFlds & RPT_OPT_SEQ_NUM);
    TEST_ASSERT_TRUE(optFlds & RPT_OPT_TIME_STAMP);
    TEST_ASSERT_FALSE(optFlds & RPT_OPT_BUFFER_OVERFLOW);
    TEST_ASSERT_FALSE(optFlds & RPT_OPT_REASON_FOR_INCLUSION);

    mxmlDelete(node);
}

/* ---- buildFcdaVariableName ---- */

void
test_buildFcdaVariableName_includesDaName_whenGiven(void) {
    char* variable = IedModelUtils_buildFcdaVariableName("ied1Inverter", "MMXU", "1", "", "ST", "Mod", "q");

    TEST_ASSERT_EQUAL_STRING("ied1Inverter/MMXU1$ST$Mod$q", variable);

    free(variable);
}

void
test_buildFcdaVariableName_omitsDaSegment_whenDaNameIsNull(void) {
    char* variable = IedModelUtils_buildFcdaVariableName("ied1Inverter", "LLN0", "", "", "ST", "Mod", NULL);

    TEST_ASSERT_EQUAL_STRING("ied1Inverter/LLN0$ST$Mod", variable);

    free(variable);
}

void
test_buildFcdaVariableName_handlesNullPrefixAndLnInst(void) {
    /* FCDA's lnInst/prefix are optional in SCL; the function must guard against
     * NULL rather than require the caller to pre-substitute "". */
    char* variable = IedModelUtils_buildFcdaVariableName("ied1Inverter", "LLN0", NULL, NULL, "ST", "Mod", "q");

    TEST_ASSERT_EQUAL_STRING("ied1Inverter/LLN0$ST$Mod$q", variable);

    free(variable);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_attrOrDefault_returnsAttrValue_whenPresent);
    RUN_TEST(test_attrOrDefault_returnsDefault_whenAbsent);
    RUN_TEST(test_attrOrDefault_returnsNull_whenAbsentAndDefaultIsNull);

    RUN_TEST(test_attrRequired_returnsValue_whenPresent);
    RUN_TEST(test_attrRequired_returnsNull_whenAbsent);

    RUN_TEST(test_attrBool_true_whenValueIsLiteralTrue);
    RUN_TEST(test_attrBool_true_whenValueIsOne);
    RUN_TEST(test_attrBool_returnsDefault_whenAbsent);
    RUN_TEST(test_attrBool_false_whenValuePresentButNotTruthy_evenIfDefaultIsTrue);

    RUN_TEST(test_attrInt_parsesNumericValue);
    RUN_TEST(test_attrInt_parsesNegativeValue);
    RUN_TEST(test_attrInt_returnsDefault_whenAbsent);
    RUN_TEST(test_attrInt_returnsZero_forNonNumericGarbage);

    RUN_TEST(test_buildLnName_concatenatesPrefixClassInst);
    RUN_TEST(test_buildLnName_handlesLLN0EmptyPrefixAndInst);

    RUN_TEST(test_mapBType_mapsCommonPrimitives);
    RUN_TEST(test_mapBType_appliesDocumentedApproximations);
    RUN_TEST(test_mapBType_returnsUnknown_forUnrecognizedString);

    RUN_TEST(test_buildTrgOps_returnsZero_whenNodeIsNull);
    RUN_TEST(test_buildTrgOps_setsBitsForPresentTrueFlags);
    RUN_TEST(test_buildTrgOps_setsAllBits_whenAllFlagsTrue);

    RUN_TEST(test_buildOptFlds_returnsZero_whenNodeIsNull);
    RUN_TEST(test_buildOptFlds_setsBitsForPresentTrueFlags);

    RUN_TEST(test_buildFcdaVariableName_includesDaName_whenGiven);
    RUN_TEST(test_buildFcdaVariableName_omitsDaSegment_whenDaNameIsNull);
    RUN_TEST(test_buildFcdaVariableName_handlesNullPrefixAndLnInst);

    return UNITY_END();
}
