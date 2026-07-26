#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "features/ied_model_online_loader/domain/ied_model_online_loader_usecases.h"

void
setUp(void) {}

void
tearDown(void) {}

/* Regression test for the real-hardware bug: convertAcsiRefToWireRef used to
 * hand DataSetEntry_create an LD-prefixed first "$"-segment ("LD/LN$FC$DO"),
 * violating this codebase's own no-LD-wire-name-prefix convention
 * (CLAUDE.md's dynamic-model gotcha #1) - silently breaking
 * IedModelUseCases_getDataSetMemberLeafReferences's LogicalNode lookup for
 * every online-discovered dataset member, so Gap-4 decomposition never ran
 * and every value forwarded as a raw, undecomposed structure. */
void
test_convertAcsiRefToWireRef_doLevelMember_stripsLdPrefix(void) {
    char* result = IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(
            "VR4C1C01A1LD0/SP16GGIO5.Ind[ST]");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("SP16GGIO5$ST$Ind", result);
    free(result);
}

void
test_convertAcsiRefToWireRef_leafMember_stripsLdPrefix(void) {
    char* result = IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(
            "VR4C1C01A1LD0/SP16GGIO5.Ind.stVal[ST]");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("SP16GGIO5$ST$Ind$stVal", result);
    free(result);
}

void
test_convertAcsiRefToWireRef_nestedSdoChain_stripsLdPrefix(void) {
    char* result = IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(
            "VR4C1C01A1LD0/MMXU1.PhV.phsA.cVal.mag.f[MX]");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("MMXU1$MX$PhV$phsA$cVal$mag$f", result);
    free(result);
}

void
test_convertAcsiRefToWireRef_arrayIndexAnnotation_isStripped(void) {
    char* result = IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(
            "VR4C1C01A1LD0/SP16GGIO5.Ind(1)component[ST]");

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("SP16GGIO5$ST$Indcomponent", result);
    free(result);
}

void
test_convertAcsiRefToWireRef_missingLdSlash_returnsNull(void) {
    /* Malformed - no "/" between LD and LN in the pre-dot prefix at all. */
    char* result = IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(
            "SomeCombinedName.Ind[ST]");

    TEST_ASSERT_NULL(result);
}

void
test_convertAcsiRefToWireRef_missingFc_returnsNull(void) {
    char* result = IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(
            "VR4C1C01A1LD0/SP16GGIO5.Ind");

    TEST_ASSERT_NULL(result);
}

void
test_convertAcsiRefToWireRef_missingDotChain_returnsNull(void) {
    char* result = IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(
            "VR4C1C01A1LD0/SP16GGIO5[ST]");

    TEST_ASSERT_NULL(result);
}

void
test_convertAcsiRefToWireRef_null_returnsNull(void) {
    TEST_ASSERT_NULL(IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(NULL));
}

int
main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_convertAcsiRefToWireRef_doLevelMember_stripsLdPrefix);
    RUN_TEST(test_convertAcsiRefToWireRef_leafMember_stripsLdPrefix);
    RUN_TEST(test_convertAcsiRefToWireRef_nestedSdoChain_stripsLdPrefix);
    RUN_TEST(test_convertAcsiRefToWireRef_arrayIndexAnnotation_isStripped);
    RUN_TEST(test_convertAcsiRefToWireRef_missingLdSlash_returnsNull);
    RUN_TEST(test_convertAcsiRefToWireRef_missingFc_returnsNull);
    RUN_TEST(test_convertAcsiRefToWireRef_missingDotChain_returnsNull);
    RUN_TEST(test_convertAcsiRefToWireRef_null_returnsNull);
    return UNITY_END();
}
