#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/mms_dataset_manager/domain/mms_dataset_manager_usecases.h"

/*
 * Unit tests for mms_dataset_manager's pure logic - the dataset-side reference
 * conversion, DO-atomic/whole-device chunking and budget arithmetic that used
 * to live in tests/mms_report_client/test_mms_report_client_usecases.c before
 * dataset provisioning was split out into its own feature. Every case here is
 * the same assertion it was before that split, only renamed to the new prefix.
 *
 * Strictly pure: no IedConnection, no ClientReportControlBlock, no server -
 * these functions take plain reference strings and integers by design, exactly
 * so they stay testable with no wire round-trip.
 */

void
setUp(void) {}

void
tearDown(void) {}

/* ---- buildWireMemberReferences (dynamic-dataset wire-format conversion) ---- */

void
test_buildWireMemberReferences_convertsDollarJoinedToDotBracketForm(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Mod$stVal", "IED1LD1/LLN0$MX$TotW$mag" };
    LinkedList wireRefs = MmsDatasetManagerUseCases_buildWireMemberReferences(refs, 2);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(wireRefs));
    LinkedList element = LinkedList_getNext(wireRefs);
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0.Mod.stVal[ST]", (const char*) LinkedList_getData(element));
    element = LinkedList_getNext(element);
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0.TotW.mag[MX]", (const char*) LinkedList_getData(element));

    LinkedList_destroyDeep(wireRefs, free);
}

void
test_buildWireMemberReferences_joinsNestedSegmentsWithDots(void) {
    const char* refs[] = { "IED1LD1/LLN0$MX$PhV$cVal$mag" };
    LinkedList wireRefs = MmsDatasetManagerUseCases_buildWireMemberReferences(refs, 1);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(wireRefs));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0.PhV.cVal.mag[MX]",
            (const char*) LinkedList_getData(LinkedList_getNext(wireRefs)));

    LinkedList_destroyDeep(wireRefs, free);
}

void
test_buildWireMemberReferences_skipsMalformedReference_tooFewSegments(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST", "IED1LD1/LLN0$ST$Mod$stVal" };
    LinkedList wireRefs = MmsDatasetManagerUseCases_buildWireMemberReferences(refs, 2);

    /* Only the well-formed second entry survives - the malformed first one
     * (nothing after the FC segment) is silently skipped, not partially
     * converted. */
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(wireRefs));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0.Mod.stVal[ST]",
            (const char*) LinkedList_getData(LinkedList_getNext(wireRefs)));

    LinkedList_destroyDeep(wireRefs, free);
}

void
test_buildWireMemberReferences_skipsNullEntry(void) {
    const char* refs[] = { NULL, "IED1LD1/LLN0$ST$Mod$stVal" };
    LinkedList wireRefs = MmsDatasetManagerUseCases_buildWireMemberReferences(refs, 2);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(wireRefs));

    LinkedList_destroyDeep(wireRefs, free);
}

void
test_buildWireMemberReferences_empty_whenCountIsZeroOrNegative(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Mod$stVal" };

    LinkedList wireRefsZero = MmsDatasetManagerUseCases_buildWireMemberReferences(refs, 0);
    TEST_ASSERT_NOT_NULL(wireRefsZero);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(wireRefsZero));
    LinkedList_destroyDeep(wireRefsZero, free);

    LinkedList wireRefsNeg = MmsDatasetManagerUseCases_buildWireMemberReferences(refs, -1);
    TEST_ASSERT_NOT_NULL(wireRefsNeg);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(wireRefsNeg));
    LinkedList_destroyDeep(wireRefsNeg, free);
}

void
test_buildWireMemberReferences_empty_whenArrayIsNull(void) {
    LinkedList wireRefs = MmsDatasetManagerUseCases_buildWireMemberReferences(NULL, 3);

    TEST_ASSERT_NOT_NULL(wireRefs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(wireRefs));

    LinkedList_destroyDeep(wireRefs, free);
}

/* ---- isDynamicDatasetBudgetExhausted (Gap 3: per-connect-cycle dataset-
 * count budget short-circuit) ---- */

void
test_isDynamicDatasetBudgetExhausted_zero_isExhausted(void) {
    TEST_ASSERT_TRUE_MESSAGE(MmsDatasetManagerUseCases_isDynamicDatasetBudgetExhausted(0),
            "a genuine zero remaining budget must be treated as exhausted");
}

void
test_isDynamicDatasetBudgetExhausted_positive_isNotExhausted(void) {
    TEST_ASSERT_FALSE(MmsDatasetManagerUseCases_isDynamicDatasetBudgetExhausted(5));
    TEST_ASSERT_FALSE(MmsDatasetManagerUseCases_isDynamicDatasetBudgetExhausted(1));
}

void
test_isDynamicDatasetBudgetExhausted_unknownNegativeOne_isNotExhausted(void) {
    TEST_ASSERT_FALSE_MESSAGE(MmsDatasetManagerUseCases_isDynamicDatasetBudgetExhausted(-1),
            "-1 means SCL never declared a DynDataSet max at all - must never trigger the "
            "short-circuit, or every device without a declared cap would stop self-creating "
            "datasets after zero attempts");
}

/* ---- computeInitialDynamicDatasetBudget (real-server-state-aware budget
 * seeding, correcting the naive "just copy SCL's own max" reset) ---- */

void
test_computeInitialDynamicDatasetBudget_subtractsExistingFromDeclaredMax(void) {
    TEST_ASSERT_EQUAL_INT(12, MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(15, 3));
    TEST_ASSERT_EQUAL_INT(0, MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(15, 15));
}

void
test_computeInitialDynamicDatasetBudget_existingExceedsMax_clampsToZero_neverNegative(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(15, 20),
            "leftover datasets exceeding the declared max must clamp to 0, not go negative");
}

void
test_computeInitialDynamicDatasetBudget_sclMaxUnknown_staysUncapped_regardlessOfExisting(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(-1, 0),
            "no declared cap means nothing to correct against - stays -1 (uncapped)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(-1, 50),
            "still uncapped even with many existing datasets discovered - there's no real cap to "
            "compare against");
}

void
test_computeInitialDynamicDatasetBudget_zeroExisting_matchesDeclaredMax(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(15, MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget(15, 0),
            "nothing pre-existing on the server means the full declared max is available, same as "
            "today's naive seeding");
}

/* ---- extractDoGroupKey (Gap 3: DO-atomic chunking's grouping key) ---- */

void
test_extractDoGroupKey_simpleReference_returnsDoSegment(void) {
    char* key = MmsDatasetManagerUseCases_extractDoGroupKey("IED1LD1/LLN0$ST$Mod$stVal");
    TEST_ASSERT_EQUAL_STRING("Mod", key);
    free(key);
}

void
test_extractDoGroupKey_nestedSdoReference_returnsTopLevelDoSegment(void) {
    /* "LD/LN$FC$DO$SDO...$DA" - the 3rd segment (the top-level DO) is the
     * group key even when the reference descends into nested SDOs, so every
     * leaf under the same top-level DO (however deeply nested) still groups
     * together, never split across a chunk boundary. */
    char* key = MmsDatasetManagerUseCases_extractDoGroupKey("IED1LD1/LLN0$MX$PhV$cVal$mag");
    TEST_ASSERT_EQUAL_STRING("PhV", key);
    free(key);
}

void
test_extractDoGroupKey_malformedFewerThanTwoDollarSigns_returnsWholeStringAsSingleton(void) {
    char* key = MmsDatasetManagerUseCases_extractDoGroupKey("IED1LD1/LLN0$ST");
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0$ST", key);
    free(key);

    char* keyNoDollar = MmsDatasetManagerUseCases_extractDoGroupKey("garbage");
    TEST_ASSERT_EQUAL_STRING("garbage", keyNoDollar);
    free(keyNoDollar);
}

void
test_extractDoGroupKey_null_returnsNull(void) {
    TEST_ASSERT_NULL(MmsDatasetManagerUseCases_extractDoGroupKey(NULL));
}

/* ---- chunkReferencesByDoGroup (Gap 3: greedy DO-atomic packing against
 * SCL's declared maxAttributes cap) ---- */

void
test_chunkReferencesByDoGroup_fitsInOneChunk_singleChunkContainingAll(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Ind1$stVal", "IED1LD1/LLN0$ST$Ind1$q" };
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesByDoGroup(refs, 2, 10);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(chunks));
    LinkedList chunk0 = (LinkedList) LinkedList_getData(LinkedList_getNext(chunks));
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(chunk0));

    LinkedList chunkElement = LinkedList_getNext(chunks);
    while (chunkElement) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(chunkElement), free);
        chunkElement = LinkedList_getNext(chunkElement);
    }
    LinkedList_destroyStatic(chunks);
}

void
test_chunkReferencesByDoGroup_twoDoGroups_splitsIntoTwoChunks_neverSplittingADo(void) {
    /* Ind1's 3-member group (stVal/q/t) plus SPCSO1's 3-member group
     * (stVal/q/t) - matches GGIO1's real leaf order in reporter1_chunking.cid.
     * maxAttributes=3 exactly fits one group but not both together. */
    const char* refs[] = {
        "IED1LD1/GGIO1$ST$Ind1$stVal", "IED1LD1/GGIO1$ST$Ind1$q", "IED1LD1/GGIO1$ST$Ind1$t",
        "IED1LD1/GGIO1$ST$SPCSO1$stVal", "IED1LD1/GGIO1$ST$SPCSO1$q", "IED1LD1/GGIO1$ST$SPCSO1$t",
    };
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesByDoGroup(refs, 6, 3);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(chunks));

    LinkedList chunk0 = (LinkedList) LinkedList_getData(LinkedList_getNext(chunks));
    TEST_ASSERT_EQUAL_INT(3, LinkedList_size(chunk0));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/GGIO1$ST$Ind1$stVal", (char*) LinkedList_getData(LinkedList_getNext(chunk0)));

    LinkedList chunk1 = (LinkedList) LinkedList_getData(LinkedList_getNext(LinkedList_getNext(chunks)));
    TEST_ASSERT_EQUAL_INT(3, LinkedList_size(chunk1));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/GGIO1$ST$SPCSO1$stVal", (char*) LinkedList_getData(LinkedList_getNext(chunk1)));

    LinkedList chunkElement = LinkedList_getNext(chunks);
    while (chunkElement) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(chunkElement), free);
        chunkElement = LinkedList_getNext(chunkElement);
    }
    LinkedList_destroyStatic(chunks);
}

void
test_chunkReferencesByDoGroup_oversizedSingleDoGroup_becomesItsOwnChunkExceedingCap(void) {
    /* One DO group alone (4 members) exceeds maxAttributes=3 - must become
     * its own chunk anyway (never split a DO's own leaves), even though that
     * chunk itself then exceeds the cap. */
    const char* refs[] = {
        "IED1LD1/LLN0$CO$Oper$ctlVal", "IED1LD1/LLN0$CO$Oper$origin", "IED1LD1/LLN0$CO$Oper$ctlNum",
        "IED1LD1/LLN0$CO$Oper$T",
    };
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesByDoGroup(refs, 4, 3);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, LinkedList_size(chunks),
            "a single oversized DO group must land in exactly one chunk, not be split or dropped");
    LinkedList chunk0 = (LinkedList) LinkedList_getData(LinkedList_getNext(chunks));
    TEST_ASSERT_EQUAL_INT(4, LinkedList_size(chunk0));

    LinkedList chunkElement = LinkedList_getNext(chunks);
    while (chunkElement) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(chunkElement), free);
        chunkElement = LinkedList_getNext(chunkElement);
    }
    LinkedList_destroyStatic(chunks);
}

void
test_chunkReferencesByDoGroup_maxAttributesZeroOrNegative_returnsEmpty(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Mod$stVal" };

    LinkedList chunksZero = MmsDatasetManagerUseCases_chunkReferencesByDoGroup(refs, 1, 0);
    TEST_ASSERT_NOT_NULL(chunksZero);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(chunksZero));
    LinkedList_destroyStatic(chunksZero);

    LinkedList chunksNeg = MmsDatasetManagerUseCases_chunkReferencesByDoGroup(refs, 1, -1);
    TEST_ASSERT_NOT_NULL(chunksNeg);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(chunksNeg));
    LinkedList_destroyStatic(chunksNeg);
}

void
test_chunkReferencesByDoGroup_countZeroOrNegative_returnsEmpty(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Mod$stVal" };
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesByDoGroup(refs, 0, 10);
    TEST_ASSERT_NOT_NULL(chunks);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(chunks));
    LinkedList_destroyStatic(chunks);
}

void
test_chunkReferencesByDoGroup_nullReferences_returnsEmpty(void) {
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesByDoGroup(NULL, 3, 10);
    TEST_ASSERT_NOT_NULL(chunks);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(chunks));
    LinkedList_destroyStatic(chunks);
}

/* ---- chunkReferencesAcrossWholeDevice (whole-device dataset clustering:
 * same greedy DO-atomic packing as chunkReferencesByDoGroup, but safe for a
 * flat reference list spanning multiple LNs) ---- */

void
test_chunkReferencesAcrossWholeDevice_combinesTwoSmallLnsIntoOneChunk(void) {
    /* Two DIFFERENT small LNs (LLN0 with 2 members, blkGGIO2 with 2 members)
     * fit together under maxAttributes=10 - proves cross-LN bin-packing
     * actually happens, maximizing device coverage within a tight
     * dataset-count budget (the whole point of this function vs the
     * per-LN-only chunkReferencesByDoGroup). */
    const char* refs[] = {
        "IED1LD1/LLN0$ST$Mod$stVal", "IED1LD1/LLN0$ST$Mod$q",
        "IED1LD1/blkGGIO2$ST$Ind1$stVal", "IED1LD1/blkGGIO2$ST$Ind1$q",
    };
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(refs, 4, 10);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, LinkedList_size(chunks),
            "two small LNs' worth of leaves should pack into one shared chunk when they fit");
    LinkedList chunk0 = (LinkedList) LinkedList_getData(LinkedList_getNext(chunks));
    TEST_ASSERT_EQUAL_INT(4, LinkedList_size(chunk0));

    LinkedList chunkElement = LinkedList_getNext(chunks);
    while (chunkElement) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(chunkElement), free);
        chunkElement = LinkedList_getNext(chunkElement);
    }
    LinkedList_destroyStatic(chunks);
}

void
test_chunkReferencesAcrossWholeDevice_sameDoNameOnDifferentLns_neverMergedAsOneGroup(void) {
    /* LLN0's own "Mod" DO (1 member) is immediately followed by a DIFFERENT
     * LN (blkGGIO2) whose FIRST DO also happens to be named "Mod" (2
     * members) - a real, not-hypothetical collision (common-data DO names
     * like Mod/Beh/Health repeat across many unrelated LN types). With
     * maxAttributes=2, if these were wrongly treated as one atomic 3-member
     * "Mod" group (chunkReferencesByDoGroup's own bare-DO-name bug for
     * cross-LN input), they'd all land in one oversized chunk. Correctly
     * keyed by LD/LN too, they must split into two separate chunks instead -
     * proves the whole-device grouping key genuinely disambiguates by LN,
     * not just DO name. */
    const char* refs[] = {
        "IED1LD1/LLN0$ST$Mod$stVal",
        "IED1LD1/blkGGIO2$ST$Mod$stVal", "IED1LD1/blkGGIO2$ST$Mod$q",
    };
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(refs, 3, 2);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, LinkedList_size(chunks),
            "LLN0's Mod and blkGGIO2's own, unrelated Mod must never be merged into one group "
            "just because they share a bare DO name");

    LinkedList chunk0 = (LinkedList) LinkedList_getData(LinkedList_getNext(chunks));
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(chunk0));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0$ST$Mod$stVal", (char*) LinkedList_getData(LinkedList_getNext(chunk0)));

    LinkedList chunk1 = (LinkedList) LinkedList_getData(LinkedList_getNext(LinkedList_getNext(chunks)));
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(chunk1));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/blkGGIO2$ST$Mod$stVal", (char*) LinkedList_getData(LinkedList_getNext(chunk1)));

    LinkedList chunkElement = LinkedList_getNext(chunks);
    while (chunkElement) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(chunkElement), free);
        chunkElement = LinkedList_getNext(chunkElement);
    }
    LinkedList_destroyStatic(chunks);
}

void
test_chunkReferencesAcrossWholeDevice_oversizedSingleDoGroup_becomesItsOwnChunkExceedingCap(void) {
    const char* refs[] = {
        "IED1LD1/LLN0$CO$Oper$ctlVal", "IED1LD1/LLN0$CO$Oper$origin", "IED1LD1/LLN0$CO$Oper$ctlNum",
        "IED1LD1/LLN0$CO$Oper$T",
    };
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(refs, 4, 3);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(chunks));
    LinkedList chunk0 = (LinkedList) LinkedList_getData(LinkedList_getNext(chunks));
    TEST_ASSERT_EQUAL_INT(4, LinkedList_size(chunk0));

    LinkedList chunkElement = LinkedList_getNext(chunks);
    while (chunkElement) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(chunkElement), free);
        chunkElement = LinkedList_getNext(chunkElement);
    }
    LinkedList_destroyStatic(chunks);
}

void
test_chunkReferencesAcrossWholeDevice_maxAttributesZeroOrNegative_returnsEmpty(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Mod$stVal" };

    LinkedList chunksZero = MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(refs, 1, 0);
    TEST_ASSERT_NOT_NULL(chunksZero);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(chunksZero));
    LinkedList_destroyStatic(chunksZero);

    LinkedList chunksNeg = MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(refs, 1, -1);
    TEST_ASSERT_NOT_NULL(chunksNeg);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(chunksNeg));
    LinkedList_destroyStatic(chunksNeg);
}

void
test_chunkReferencesAcrossWholeDevice_nullReferences_returnsEmpty(void) {
    LinkedList chunks = MmsDatasetManagerUseCases_chunkReferencesAcrossWholeDevice(NULL, 3, 10);
    TEST_ASSERT_NOT_NULL(chunks);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(chunks));
    LinkedList_destroyStatic(chunks);
}

/* ---- groupReferencesByLn (whole-device clustering's no-maxAttributes-known
 * fallback: one group per LN, unbounded size) ---- */

void
test_groupReferencesByLn_splitsIntoOneGroupPerContiguousLnRun(void) {
    const char* refs[] = {
        "IED1LD1/LLN0$ST$Mod$stVal", "IED1LD1/LLN0$MX$TotW$mag",
        "IED1LD1/GGIO1$ST$Ind1$stVal", "IED1LD1/GGIO1$ST$Ind1$q", "IED1LD1/GGIO1$ST$Ind1$t",
    };
    LinkedList groups = MmsDatasetManagerUseCases_groupReferencesByLn(refs, 5);

    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(groups));

    LinkedList group0 = (LinkedList) LinkedList_getData(LinkedList_getNext(groups));
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(group0));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0$ST$Mod$stVal", (char*) LinkedList_getData(LinkedList_getNext(group0)));

    LinkedList group1 = (LinkedList) LinkedList_getData(LinkedList_getNext(LinkedList_getNext(groups)));
    TEST_ASSERT_EQUAL_INT(3, LinkedList_size(group1));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/GGIO1$ST$Ind1$stVal", (char*) LinkedList_getData(LinkedList_getNext(group1)));

    LinkedList groupElement = LinkedList_getNext(groups);
    while (groupElement) {
        LinkedList_destroyDeep((LinkedList) LinkedList_getData(groupElement), free);
        groupElement = LinkedList_getNext(groupElement);
    }
    LinkedList_destroyStatic(groups);
}

void
test_groupReferencesByLn_noSizeCap_oneLnWithManyLeavesStaysOneGroup(void) {
    const char* refs[] = {
        "IED1LD1/LLN0$ST$A$stVal", "IED1LD1/LLN0$ST$B$stVal", "IED1LD1/LLN0$ST$C$stVal",
        "IED1LD1/LLN0$ST$D$stVal",
    };
    LinkedList groups = MmsDatasetManagerUseCases_groupReferencesByLn(refs, 4);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, LinkedList_size(groups), "one LN, however many leaves, is one group");
    LinkedList group0 = (LinkedList) LinkedList_getData(LinkedList_getNext(groups));
    TEST_ASSERT_EQUAL_INT(4, LinkedList_size(group0));

    LinkedList_destroyDeep(group0, free);
    LinkedList_destroyStatic(groups);
}

void
test_groupReferencesByLn_countZeroOrNegative_returnsEmpty(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Mod$stVal" };
    LinkedList groups = MmsDatasetManagerUseCases_groupReferencesByLn(refs, 0);
    TEST_ASSERT_NOT_NULL(groups);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(groups));
    LinkedList_destroyStatic(groups);
}

void
test_groupReferencesByLn_nullReferences_returnsEmpty(void) {
    LinkedList groups = MmsDatasetManagerUseCases_groupReferencesByLn(NULL, 3);
    TEST_ASSERT_NOT_NULL(groups);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(groups));
    LinkedList_destroyStatic(groups);
}

/* ---- convertAcsiRefToMemberReference (tier-2 pulled-dataset ACSI -> this
 * feature's own "$"-joined member-reference conversion - the mirror image of
 * buildWireMemberReferences above, and NOT the same as
 * ied_model_online_loader's own IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef,
 * which strips the LD prefix for a different consumer's convention) ---- */

void
test_convertAcsiRefToMemberReference_doLevelRef_preservesLdPrefix(void) {
    char* out = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference("IED1LD1/LLN0.Mod[ST]");

    /* The one behavioral difference from IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef -
     * this feature's own memberReferences[] convention is LD-prefixed. */
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0$ST$Mod", out);
    free(out);
}

void
test_convertAcsiRefToMemberReference_leafRef_joinsDoAndDaWithDollar(void) {
    char* out = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference("IED1LD1/LLN0.Mod.stVal[ST]");

    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0$ST$Mod$stVal", out);
    free(out);
}

void
test_convertAcsiRefToMemberReference_nestedSdo_joinsEverySegmentWithDollar(void) {
    char* out = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference("IED1LD1/LLN0.PhV.cVal.mag[MX]");

    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0$MX$PhV$cVal$mag", out);
    free(out);
}

void
test_convertAcsiRefToMemberReference_stripsArrayIndexAnnotation(void) {
    char* out = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference("IED1LD1/LLN0.Arr(1)item[ST]");

    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0$ST$Arritem", out);
    free(out);
}

void
test_convertAcsiRefToMemberReference_null_onMissingTrailingFc(void) {
    char* out = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference("IED1LD1/LLN0.Mod.stVal");
    TEST_ASSERT_NULL(out);
}

void
test_convertAcsiRefToMemberReference_null_onMissingDotAfterLdLnPrefix(void) {
    char* out = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference("IED1LD1/LLN0[ST]");
    TEST_ASSERT_NULL(out);
}

void
test_convertAcsiRefToMemberReference_null_onMissingSlashInLdLnPrefix(void) {
    char* out = MmsDatasetManagerUseCases_convertAcsiRefToMemberReference("IED1LD1LLN0.Mod.stVal[ST]");
    TEST_ASSERT_NULL(out);
}

void
test_convertAcsiRefToMemberReference_null_onNullInput(void) {
    TEST_ASSERT_NULL(MmsDatasetManagerUseCases_convertAcsiRefToMemberReference(NULL));
}


int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_buildWireMemberReferences_convertsDollarJoinedToDotBracketForm);
    RUN_TEST(test_buildWireMemberReferences_joinsNestedSegmentsWithDots);
    RUN_TEST(test_buildWireMemberReferences_skipsMalformedReference_tooFewSegments);
    RUN_TEST(test_buildWireMemberReferences_skipsNullEntry);
    RUN_TEST(test_buildWireMemberReferences_empty_whenCountIsZeroOrNegative);
    RUN_TEST(test_buildWireMemberReferences_empty_whenArrayIsNull);

    RUN_TEST(test_isDynamicDatasetBudgetExhausted_zero_isExhausted);
    RUN_TEST(test_isDynamicDatasetBudgetExhausted_positive_isNotExhausted);
    RUN_TEST(test_isDynamicDatasetBudgetExhausted_unknownNegativeOne_isNotExhausted);

    RUN_TEST(test_computeInitialDynamicDatasetBudget_subtractsExistingFromDeclaredMax);
    RUN_TEST(test_computeInitialDynamicDatasetBudget_existingExceedsMax_clampsToZero_neverNegative);
    RUN_TEST(test_computeInitialDynamicDatasetBudget_sclMaxUnknown_staysUncapped_regardlessOfExisting);
    RUN_TEST(test_computeInitialDynamicDatasetBudget_zeroExisting_matchesDeclaredMax);

    RUN_TEST(test_extractDoGroupKey_simpleReference_returnsDoSegment);
    RUN_TEST(test_extractDoGroupKey_nestedSdoReference_returnsTopLevelDoSegment);
    RUN_TEST(test_extractDoGroupKey_malformedFewerThanTwoDollarSigns_returnsWholeStringAsSingleton);
    RUN_TEST(test_extractDoGroupKey_null_returnsNull);

    RUN_TEST(test_chunkReferencesByDoGroup_fitsInOneChunk_singleChunkContainingAll);
    RUN_TEST(test_chunkReferencesByDoGroup_twoDoGroups_splitsIntoTwoChunks_neverSplittingADo);
    RUN_TEST(test_chunkReferencesByDoGroup_oversizedSingleDoGroup_becomesItsOwnChunkExceedingCap);
    RUN_TEST(test_chunkReferencesByDoGroup_maxAttributesZeroOrNegative_returnsEmpty);
    RUN_TEST(test_chunkReferencesByDoGroup_countZeroOrNegative_returnsEmpty);
    RUN_TEST(test_chunkReferencesByDoGroup_nullReferences_returnsEmpty);

    RUN_TEST(test_chunkReferencesAcrossWholeDevice_combinesTwoSmallLnsIntoOneChunk);
    RUN_TEST(test_chunkReferencesAcrossWholeDevice_sameDoNameOnDifferentLns_neverMergedAsOneGroup);
    RUN_TEST(test_chunkReferencesAcrossWholeDevice_oversizedSingleDoGroup_becomesItsOwnChunkExceedingCap);
    RUN_TEST(test_chunkReferencesAcrossWholeDevice_maxAttributesZeroOrNegative_returnsEmpty);
    RUN_TEST(test_chunkReferencesAcrossWholeDevice_nullReferences_returnsEmpty);

    RUN_TEST(test_groupReferencesByLn_splitsIntoOneGroupPerContiguousLnRun);
    RUN_TEST(test_groupReferencesByLn_noSizeCap_oneLnWithManyLeavesStaysOneGroup);
    RUN_TEST(test_groupReferencesByLn_countZeroOrNegative_returnsEmpty);
    RUN_TEST(test_groupReferencesByLn_nullReferences_returnsEmpty);

    RUN_TEST(test_convertAcsiRefToMemberReference_doLevelRef_preservesLdPrefix);
    RUN_TEST(test_convertAcsiRefToMemberReference_leafRef_joinsDoAndDaWithDollar);
    RUN_TEST(test_convertAcsiRefToMemberReference_nestedSdo_joinsEverySegmentWithDollar);
    RUN_TEST(test_convertAcsiRefToMemberReference_stripsArrayIndexAnnotation);
    RUN_TEST(test_convertAcsiRefToMemberReference_null_onMissingTrailingFc);
    RUN_TEST(test_convertAcsiRefToMemberReference_null_onMissingDotAfterLdLnPrefix);
    RUN_TEST(test_convertAcsiRefToMemberReference_null_onMissingSlashInLdLnPrefix);
    RUN_TEST(test_convertAcsiRefToMemberReference_null_onNullInput);


    return UNITY_END();
}
