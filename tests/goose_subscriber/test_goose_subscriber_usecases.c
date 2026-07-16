#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/goose_subscriber/domain/goose_subscriber_usecases.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- buildRecord ---- */

void
test_buildRecord_copiesScalarFields(void) {
    uint8_t srcMac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    uint8_t dstMac[6] = { 0x01, 0x0c, 0xcd, 0x01, 0x00, 0x05 };

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", "gcbStatusGoId", "ds1",
            42, 3, 1,
            false, false,
            2000, 1700000000000ULL,
            true, 10, 4, 2000,
            srcMac, dstMac,
            NULL, NULL, 0);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/LLN0$GO$gcbStatus", record->goCbRef);
    TEST_ASSERT_EQUAL_STRING("gcbStatusGoId", record->goId);
    TEST_ASSERT_EQUAL_STRING("ds1", record->dataSet);
    TEST_ASSERT_EQUAL_UINT32(42, record->stNum);
    TEST_ASSERT_EQUAL_UINT32(3, record->sqNum);
    TEST_ASSERT_EQUAL_UINT32(1, record->confRev);
    TEST_ASSERT_FALSE(record->test);
    TEST_ASSERT_FALSE(record->needsCommission);
    TEST_ASSERT_EQUAL_UINT32(2000, record->timeAllowedToLiveMs);
    TEST_ASSERT_EQUAL_UINT64(1700000000000ULL, record->timestampMs);
    TEST_ASSERT_TRUE(record->hasVlan);
    TEST_ASSERT_EQUAL_UINT16(10, record->vlanId);
    TEST_ASSERT_EQUAL_UINT8(4, record->vlanPrio);
    TEST_ASSERT_EQUAL_INT32(2000, record->appId);
    TEST_ASSERT_EQUAL_MEMORY(srcMac, record->srcMac, 6);
    TEST_ASSERT_EQUAL_MEMORY(dstMac, record->dstMac, 6);
    TEST_ASSERT_EQUAL_INT(0, record->entryCount);
    TEST_ASSERT_NULL(record->entries);

    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_deepCopiesEntries_notAliased(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));
    MmsValue_setElement(dataSetValues, 1, MmsValue_newIntegerFromInt32(99));

    uint8_t zeroMac[6] = { 0 };

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, NULL, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(2, record->entryCount);
    TEST_ASSERT_NOT_NULL(record->entries);
    TEST_ASSERT_TRUE(MmsValue_getBoolean(record->entries[0].value));
    TEST_ASSERT_EQUAL_INT32(99, MmsValue_toInt32(record->entries[1].value));

    /* Mutate/free the source after the call - the record must be unaffected.
     * This is what proves the "must not borrow past the callback" contract,
     * since the real opaque GooseSubscriber has no public constructor to test
     * this against directly. */
    MmsValue_setBoolean(MmsValue_getElement(dataSetValues, 0), false);
    MmsValue_delete(dataSetValues);

    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getBoolean(record->entries[0].value),
            "entry value must be a deep copy, unaffected by later mutation/deletion of the source");

    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_populatesReference_fromMemberReferences(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));
    MmsValue_setElement(dataSetValues, 1, MmsValue_newIntegerFromInt32(99));

    uint8_t zeroMac[6] = { 0 };
    char ref0[] = "Reporter1LD1/GGIO1$ST$Ind1$stVal";
    char ref1[] = "Reporter1LD1/GGIO1$ST$Ind1$q";
    char* memberReferences[2] = { ref0, ref1 };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberReferences = memberReferences;
    cache.memberCount = 2;

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_STRING(ref0, record->entries[0].reference);
    TEST_ASSERT_EQUAL_STRING(ref1, record->entries[1].reference);

    /* Mutate the source buffer after the call - proves the reference is a
     * deep copy, same aliasing guarantee as the value entries above. */
    ref0[0] = 'X';
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Reporter1LD1/GGIO1$ST$Ind1$stVal", record->entries[0].reference,
            "entry reference must be a deep copy, unaffected by later mutation of the source buffer");

    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_referenceOutOfRange_leavesReferenceNull(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false));

    uint8_t zeroMac[6] = { 0 };
    char ref0[] = "Reporter1LD1/GGIO1$ST$Ind1$stVal";
    char* memberReferences[1] = { ref0 };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberReferences = memberReferences;
    cache.memberCount = 1;

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_STRING("Reporter1LD1/GGIO1$ST$Ind1$stVal", record->entries[0].reference);
    TEST_ASSERT_NULL(record->entries[1].reference);

    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_handlesNullDataSetValues_zeroEntries(void) {
    /* Real caller (the frame adapter) always derives entryCount from
     * dataSetValues itself (dataSetValues ? MmsValue_getArraySize(...) : 0),
     * so NULL dataSetValues is always paired with entryCount == 0 in practice. */
    uint8_t zeroMac[6] = { 0 };

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            0, 0, 0, false, false, 0, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            NULL, NULL, 0);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(0, record->entryCount);
    TEST_ASSERT_NULL(record->entries);

    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_hasVlanFalse_leavesVlanFieldsAtZero(void) {
    uint8_t zeroMac[6] = { 0 };

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            0, 0, 0, false, false, 0, 0,
            false, 10, 4, -1,
            zeroMac, zeroMac,
            NULL, NULL, 0);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_FALSE(record->hasVlan);
    TEST_ASSERT_EQUAL_UINT16(0, record->vlanId);
    TEST_ASSERT_EQUAL_UINT8(0, record->vlanPrio);

    GooseSubscriberUseCases_freeRecord(record);
}

void
test_freeRecord_doesNotCrash_onNull(void) {
    GooseSubscriberUseCases_freeRecord(NULL);
}

/* ---- buildRecord: Gap 4 structure decomposition ---- */

void
test_buildRecord_decomposesStructuredEntry_intoFlatLeaves(void) {
    MmsValue* stVal = MmsValue_newBoolean(true);
    MmsValue* q = MmsValue_newBitString(13);
    MmsValue* structVal = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal, 0, stVal);
    MmsValue_setElement(structVal, 1, q);

    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    uint8_t zeroMac[6] = { 0 };
    char* leafRefs0[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 2 };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(2, record->entryCount);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$stVal", record->entries[0].reference);
    TEST_ASSERT_TRUE(MmsValue_getBoolean(record->entries[0].value));
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$q", record->entries[1].reference);

    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_decomposition_countMismatch_fallsBackToRawEntry(void) {
    /* Claims 3 leaves, but the actual structure only has 2 elements - must
     * not mis-pair labels to values. */
    MmsValue* stVal = MmsValue_newBoolean(true);
    MmsValue* q = MmsValue_newBitString(13);
    MmsValue* structVal = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal, 0, stVal);
    MmsValue_setElement(structVal, 1, q);

    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    uint8_t zeroMac[6] = { 0 };
    char ref0[] = "Breaker1CB1/XCBR1.Pos";
    char* memberReferences[1] = { ref0 };
    char* leafRefs0[3] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q", "Breaker1CB1/XCBR1.Pos$t" };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 3 }; /* mismatch: structVal only has 2 elements */
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberReferences = memberReferences;
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, record->entryCount,
            "a leaf-count mismatch must fall back to one raw (non-decomposed) entry, not mis-paired leaves");
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos", record->entries[0].reference);

    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_decomposition_withWireTypesPresent_stillDecomposesWhenTypesMatch(void) {
    /* Regression case: memberLeafWireTypes populated AND genuinely matching
     * the wire's actual types must still decompose normally. */
    MmsValue* stVal = MmsValue_newBoolean(true);
    MmsValue* q = MmsValue_newBitString(13);
    MmsValue* structVal = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal, 0, stVal);
    MmsValue_setElement(structVal, 1, q);

    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    uint8_t zeroMac[6] = { 0 };
    char* leafRefs0[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 2 };
    DataAttributeType wireTypes0[2] = { IEC61850_BOOLEAN, IEC61850_QUALITY };
    DataAttributeType* memberLeafWireTypes[1] = { wireTypes0 };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;
    cache.memberLeafWireTypes = memberLeafWireTypes;

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "matching wire types must not block a genuinely well-ordered decomposition");
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$stVal", record->entries[0].reference);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$q", record->entries[1].reference);

    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

/* ---- buildRecord: per-position value-diff filter ---- */

void
test_buildRecord_firstEverValue_isSuppressed_andSeedsCache(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { NULL }; /* never cached yet */
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "the very first frame for a position must never reach the websocket - it's cache-seed "
            "only, GOOSE's equivalent of MMS's GI-suppression");
    TEST_ASSERT_NULL(record->entries);
    TEST_ASSERT_NOT_NULL_MESSAGE(cache.lastForwardedValues[0],
            "the cache slot must still be silently seeded, so the first GENUINE change afterward "
            "has a real previous value to report");
    TEST_ASSERT_TRUE(MmsValue_getBoolean(cache.lastForwardedValues[0]));

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_unchangedValue_isDroppedAfterSeed(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(true) }; /* already forwarded once, same value */
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "a re-send of an unchanged value must be dropped once the cache is seeded");
    TEST_ASSERT_NULL(record->entries);

    MmsValue_delete(lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_changedValue_isForwarded_andUpdatesCache(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(false) }; /* last forwarded value differs */
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, record->entryCount, "an entry whose value genuinely differs must survive");
    TEST_ASSERT_NOT_NULL(cache.lastForwardedValues[0]);
    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getBoolean(cache.lastForwardedValues[0]),
            "the cache must be updated to the new value after forwarding");
    TEST_ASSERT_NOT_NULL_MESSAGE(record->entries[0].previousValue,
            "a genuinely changed entry must carry the prior cached value as its previousValue");
    TEST_ASSERT_FALSE_MESSAGE(MmsValue_getBoolean(record->entries[0].previousValue),
            "previousValue must be the OLD cached value, not the new one");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_firstEverRealChange_noPriorBootstrap_previousValueIsNull(void) {
    /* slot < 0 (no memberRefCache at all) - the one accepted structural case
     * where previousValue can never be known. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, NULL, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(1, record->entryCount);
    TEST_ASSERT_NULL(record->entries[0].previousValue);

    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

/* ---- buildRecord: group-aware forwarding (value <-> quality pairing) ---- */

void
test_buildRecord_valueForwarded_dragsUnchangedQualitySibling(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));  /* stVal: differs from cache */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false)); /* q: matches cache */

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), MmsValue_newBoolean(false) };
    char* memberReferences[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "quality's own diff-check says unchanged, but it must still be dragged along "
            "because its value sibling (same DO) genuinely changed");
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$stVal", record->entries[0].reference);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$q", record->entries[1].reference);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_draggedAlongSibling_previousValueEqualsOwnCurrentValue(void) {
    /* Same scenario as test_buildRecord_valueForwarded_dragsUnchangedQualitySibling -
     * asserts the dragged-along quality's own previousValue: since it didn't
     * itself change, previousValue must equal its own current value. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));  /* stVal: differs from cache */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false)); /* q: matches cache */

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), MmsValue_newBoolean(false) };
    char* memberReferences[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(2, record->entryCount);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$q", record->entries[1].reference);
    TEST_ASSERT_NOT_NULL(record->entries[1].previousValue);
    TEST_ASSERT_EQUAL_MESSAGE(MmsValue_getBoolean(record->entries[1].value),
            MmsValue_getBoolean(record->entries[1].previousValue),
            "the dragged-along quality entry didn't itself change, so previousValue must equal value");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_qualityForwarded_dragsUnchangedValueSibling(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(false)); /* stVal: matches cache */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(true));  /* q: differs from cache */

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), MmsValue_newBoolean(false) };
    char* memberReferences[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "a genuine quality-only change must not be dropped as a lone entry - its unchanged "
            "value sibling must be dragged along too, matching what ipc_dispatcher needs to pair them");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_bothSiblingsUnchanged_neitherForwarded(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(false));
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false));

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), MmsValue_newBoolean(false) };
    char* memberReferences[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(0, record->entryCount);
    TEST_ASSERT_NULL(record->entries);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_ungroupableEntry_fallsBackToSoloDiffCheck(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true)); /* differs from cache */

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(false) };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;
    /* memberReferences left NULL - this entry's reference can never resolve,
     * so it must be its own ungroupable singleton, behaving exactly like the
     * plain solo diff-check. */

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(1, record->entryCount);
    TEST_ASSERT_NULL(record->entries[0].reference);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_decomposedGroup_changedLeafDragsUnchangedSiblingLeaf(void) {
    char* leafRefs0[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 2 };
    int leafSlotOffsets[1] = { 0 };
    /* "$q" must be a real 13-bit MMS_BIT_STRING (Quality's own fixed wire
     * encoding, see reorderFlattenedToMatchReferences's own doc comment) -
     * not a placeholder boolean - the reorder step now requires this to
     * resolve which flattened wire value actually IS "q". */
    MmsValue* cachedQ = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(cachedQ, 0);
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), cachedQ };

    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;

    MmsValue* newQ = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(newQ, 0);
    MmsValue* structVal = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal, 0, MmsValue_newBoolean(true)); /* stVal: differs from cache */
    MmsValue_setElement(structVal, 1, newQ);                     /* q: matches cache */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "the unchanged q leaf must be dragged along by its changed stVal sibling, even "
            "though both leaves came from the same decomposed DO-level position (Gap 4)");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_nestedCmvValue_dragsQualitySeveralAncestorLevelsUp(void) {
    /* Real device shape: cVal.mag.f (2 raw dataset positions - "cVal$mag$f"
     * and "q" - authored as separate DA-level FCDA entries, not decomposed
     * from one DO-level entry) nests 3 "$"-segments below the CMV instance
     * ("phsA") that q actually belongs to. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newFloat(50.0f)); /* cVal.mag.f: differs from cache */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false)); /* q: matches cache */

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newFloat(49.0f), MmsValue_newBoolean(false) };
    char* memberReferences[2] = {
        "LD0/MMXU1$MX$PhV$phsA$cVal$mag$f",
        "LD0/MMXU1$MX$PhV$phsA$q",
    };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "q must be dragged along by its changed cVal.mag.f sibling despite being several "
            "\"$\"-segments shallower than the nested measured value's own reference");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_doesNotOverreach_pastAGenuinelyUnrelatedAncestor(void) {
    /* Two independent CMV instances (phsA, phsB) - phsA's value must never
     * accidentally group with phsB's q just because "PhV" is a shared
     * ancestor of both. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newFloat(50.0f)); /* phsA's cVal.mag.f: differs */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false)); /* phsB's q: matches cache */

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newFloat(49.0f), MmsValue_newBoolean(false) };
    char* memberReferences[2] = {
        "LD0/MMXU1$MX$PhV$phsA$cVal$mag$f",
        "LD0/MMXU1$MX$PhV$phsB$q",
    };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, record->entryCount,
            "phsB's unrelated, unchanged q must NOT be dragged along by phsA's changed value");
    TEST_ASSERT_EQUAL_STRING("LD0/MMXU1$MX$PhV$phsA$cVal$mag$f", record->entries[0].reference);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

/* ---- value-diff cache persistence across a simulated liveness recovery ----
 * The cache is now NEVER reset (the old resetValueDiffCache function and its
 * every-recovery call site in the frame adapter are both gone) - it's
 * populated exactly once, on this target's first-ever valid frame, and
 * preserved for the rest of the subscriber's lifetime. These tests drive
 * buildRecord multiple times in a row with NO reset call anywhere in between
 * (there is no such call left to make) to prove: the first call's snapshot
 * silently seeds the cache and flips everPopulated; a later call simulating
 * a recovery's own fresh full snapshot correctly diffs against the REAL
 * preserved prior value - a genuine change forwards with a real (non-NULL)
 * previousValue, an unchanged resend is suppressed exactly like any other
 * duplicate. */

void
test_buildRecord_firstFrame_seedsCache_andSetsEverPopulated(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { NULL };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    TEST_ASSERT_FALSE(cache.everPopulated);

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "the very first-ever frame must never reach the callback - bootstrap-only");
    TEST_ASSERT_NOT_NULL_MESSAGE(cache.lastForwardedValues[0], "the cache must be seeded from the first frame");
    TEST_ASSERT_TRUE_MESSAGE(cache.everPopulated, "everPopulated must flip true after the first frame");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_simulatedRecovery_genuineChangeForwards_withRealPreviousValue(void) {
    /* Simulates a liveness recovery: the cache already holds a real value
     * from before the "outage" (no reset ever runs in between - there is no
     * such call anymore), and the "recovery"'s own fresh full snapshot
     * carries a genuinely different value. Must forward, WITH a real
     * (non-NULL) previousValue reflecting the true pre-outage state - not
     * NULL, which is exactly the bug this design fixes. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(false)); /* changed while "stale" */

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(true) }; /* real pre-outage value, still cached */
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;
    cache.everPopulated = true; /* this target already completed its first-ever frame before */

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, record->entryCount,
            "a genuine change discovered on recovery must forward, diffed against the preserved cache");
    TEST_ASSERT_FALSE(MmsValue_getBoolean(record->entries[0].value));
    TEST_ASSERT_NOT_NULL_MESSAGE(record->entries[0].previousValue,
            "previousValue must be the REAL preserved pre-outage value, never NULL, after a recovery");
    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getBoolean(record->entries[0].previousValue),
            "previousValue must reflect the true value from before the outage");

    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

void
test_buildRecord_simulatedRecovery_unchangedResend_isSuppressed(void) {
    /* Same simulated-recovery setup, but this time the recovery's own fresh
     * full snapshot carries the SAME value as before the outage - must be
     * suppressed by the ordinary diff check, exactly like any other
     * unchanged resend, with no special-cased "recovery" behavior needed. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true)); /* unchanged across the "outage" */

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(true) };
    GooseSubscriberMemberRefCache cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;
    cache.everPopulated = true;

    uint8_t zeroMac[6] = { 0 };
    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "an unchanged resend after a recovery must be suppressed, same as any other duplicate");
    TEST_ASSERT_NOT_NULL(cache.lastForwardedValues[0]);
    TEST_ASSERT_TRUE(MmsValue_getBoolean(cache.lastForwardedValues[0]));

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    GooseSubscriberUseCases_freeRecord(record);
}

/* ---- isDuplicateValue ---- */

void
test_isDuplicateValue_falseWhenCachedIsNull(void) {
    MmsValue* newValue = MmsValue_newBoolean(true);
    TEST_ASSERT_FALSE(GooseSubscriberUseCases_isDuplicateValue(NULL, newValue));
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_trueWhenEqual(void) {
    MmsValue* cached = MmsValue_newBoolean(true);
    MmsValue* newValue = MmsValue_newBoolean(true);
    TEST_ASSERT_TRUE(GooseSubscriberUseCases_isDuplicateValue(cached, newValue));
    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_falseWhenDifferent(void) {
    MmsValue* cached = MmsValue_newBoolean(true);
    MmsValue* newValue = MmsValue_newBoolean(false);
    TEST_ASSERT_FALSE(GooseSubscriberUseCases_isDuplicateValue(cached, newValue));
    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

/* Type-aware comparison cases - see valuesAreSemanticallyEqual's own doc
 * comment in goose_subscriber_usecases.c (and its identical, independently
 * duplicated twin in mms_report_client_usecases.c) for the full real-hardware
 * finding: MmsValue_equals is a raw byte-exact comparison, wrong for
 * MMS_UTC_TIME (includes the TimeQuality byte) and MMS_BIT_STRING (includes
 * unused padding bits) - both types show up constantly in real GOOSE
 * datasets too. */

void
test_isDuplicateValue_utcTime_sameMsDifferentQualityByte_isDuplicate(void) {
    MmsValue* cached = MmsValue_newUtcTimeByMsTime(1700000000123ULL);
    MmsValue* newValue = MmsValue_newUtcTimeByMsTime(1700000000123ULL);
    MmsValue_setUtcTimeMsEx(cached, 1700000000123ULL, 0x00);
    MmsValue_setUtcTimeMsEx(newValue, 1700000000123ULL, 0x0A);

    TEST_ASSERT_FALSE_MESSAGE(MmsValue_equals(cached, newValue),
            "sanity: the OLD raw byte-exact comparison sees these as different - documents the bug this fixes");
    TEST_ASSERT_TRUE_MESSAGE(GooseSubscriberUseCases_isDuplicateValue(cached, newValue),
            "a differing TimeQuality byte alone must not be treated as a real change");

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_utcTime_genuinelyDifferentMs_isNotDuplicate(void) {
    MmsValue* cached = MmsValue_newUtcTimeByMsTime(1700000000123ULL);
    MmsValue* newValue = MmsValue_newUtcTimeByMsTime(1700000000456ULL);

    TEST_ASSERT_FALSE(GooseSubscriberUseCases_isDuplicateValue(cached, newValue));

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_bitString_sameSizeSameBits_isDuplicate(void) {
    MmsValue* cached = MmsValue_newBitString(2);
    MmsValue_setBitStringBit(cached, 0, false);
    MmsValue_setBitStringBit(cached, 1, true);
    MmsValue* newValue = MmsValue_newBitString(2);
    MmsValue_setBitStringBit(newValue, 0, false);
    MmsValue_setBitStringBit(newValue, 1, true);

    TEST_ASSERT_TRUE(GooseSubscriberUseCases_isDuplicateValue(cached, newValue));

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_bitString_genuinelyDifferentBits_isNotDuplicate(void) {
    MmsValue* cached = MmsValue_newBitString(2);
    MmsValue_setBitStringBit(cached, 0, false);
    MmsValue_setBitStringBit(cached, 1, true);
    MmsValue* newValue = MmsValue_newBitString(2);
    MmsValue_setBitStringBit(newValue, 0, true);
    MmsValue_setBitStringBit(newValue, 1, true);

    TEST_ASSERT_FALSE(GooseSubscriberUseCases_isDuplicateValue(cached, newValue));

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_bitString_sameDecodedIntegerDifferentSize_isNotDuplicate(void) {
    /* See mms_report_client's identical test's own comment for why the
     * real-world padding-bit scenario itself can't be reproduced through the
     * public MmsValue API - this proves the added size guard instead. */
    MmsValue* cached = MmsValue_newBitString(2);
    MmsValue_setBitStringBit(cached, 0, true);
    MmsValue_setBitStringBit(cached, 1, false);

    MmsValue* newValue = MmsValue_newBitString(8);
    MmsValue_setBitStringBit(newValue, 0, true);

    TEST_ASSERT_EQUAL_UINT32(MmsValue_getBitStringAsInteger(cached), MmsValue_getBitStringAsInteger(newValue));
    TEST_ASSERT_FALSE_MESSAGE(GooseSubscriberUseCases_isDuplicateValue(cached, newValue),
            "a genuine size difference must never be masked by a coincidentally-matching decoded integer");

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_typeMismatch_isNotDuplicate(void) {
    MmsValue* cached = MmsValue_newBoolean(true);
    MmsValue* newValue = MmsValue_newBitString(2);
    MmsValue_setBitStringBit(newValue, 0, true);

    TEST_ASSERT_FALSE(GooseSubscriberUseCases_isDuplicateValue(cached, newValue));

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

/* ---- shouldForwardAcrossTarget (cross-target duplicate-content suppression) ---- */

static GooseSubscriberEntry*
makeCrossTargetDedupEntries(const char* ref0, bool val0, const char* ref1, bool val1) {
    GooseSubscriberEntry* entries = calloc(2, sizeof(GooseSubscriberEntry));
    entries[0].reference = strdup(ref0);
    entries[0].value = MmsValue_newBoolean(val0);
    entries[1].reference = strdup(ref1);
    entries[1].value = MmsValue_newBoolean(val1);
    return entries;
}

static void
freeCrossTargetDedupEntries(GooseSubscriberEntry* entries, int count) {
    for (int i = 0; i < count; i++) {
        free(entries[i].reference);
        if (entries[i].value) MmsValue_delete(entries[i].value);
    }
    free(entries);
}

void
test_shouldForwardAcrossTarget_firstEverContent_isForwarded_andSeedsCache(void) {
    GooseSubscriberCrossTargetDedupCache cache = { 0 };
    GooseSubscriberEntry* entries = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);

    bool result = GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbA", entries, 2);

    TEST_ASSERT_TRUE_MESSAGE(result, "nothing cached yet - must always forward");
    TEST_ASSERT_EQUAL_STRING("LD/LLN0$GO$gcbA", cache.goCbRef);
    TEST_ASSERT_EQUAL_INT(2, cache.entryCount);

    freeCrossTargetDedupEntries(entries, 2);
    GooseSubscriberUseCases_destroyCrossTargetDedupCache(&cache);
}

void
test_shouldForwardAcrossTarget_sameTargetIdenticalContent_isStillForwarded(void) {
    /* A repeat from the SAME GoCB is this stage's non-concern - that
     * target's own per-position filter already decided to forward it. */
    GooseSubscriberCrossTargetDedupCache cache = { 0 };
    GooseSubscriberEntry* entries1 = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    TEST_ASSERT_TRUE(GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbA", entries1, 2));

    GooseSubscriberEntry* entries2 = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    bool result = GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbA", entries2, 2);

    TEST_ASSERT_TRUE(result);

    freeCrossTargetDedupEntries(entries1, 2);
    freeCrossTargetDedupEntries(entries2, 2);
    GooseSubscriberUseCases_destroyCrossTargetDedupCache(&cache);
}

void
test_shouldForwardAcrossTarget_differentTargetIdenticalContent_isSuppressed(void) {
    GooseSubscriberCrossTargetDedupCache cache = { 0 };
    GooseSubscriberEntry* entries1 = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    TEST_ASSERT_TRUE(GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbA", entries1, 2));

    GooseSubscriberEntry* entries2 = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    bool result = GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbB", entries2, 2);

    TEST_ASSERT_FALSE_MESSAGE(result,
            "a different GoCB reporting byte-identical content must be suppressed as a duplicate");

    freeCrossTargetDedupEntries(entries1, 2);
    freeCrossTargetDedupEntries(entries2, 2);
    GooseSubscriberUseCases_destroyCrossTargetDedupCache(&cache);
}

void
test_shouldForwardAcrossTarget_differentTargetDifferentContent_isForwarded(void) {
    GooseSubscriberCrossTargetDedupCache cache = { 0 };
    GooseSubscriberEntry* entries1 = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    TEST_ASSERT_TRUE(GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbA", entries1, 2));

    GooseSubscriberEntry* entries2 = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", false, "LD/GGIO1$ST$SPCSO5$stVal", true);
    bool result = GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbB", entries2, 2);

    TEST_ASSERT_TRUE_MESSAGE(result, "genuinely different content from a different GoCB must be forwarded");
    TEST_ASSERT_EQUAL_STRING("LD/LLN0$GO$gcbB", cache.goCbRef);

    freeCrossTargetDedupEntries(entries1, 2);
    freeCrossTargetDedupEntries(entries2, 2);
    GooseSubscriberUseCases_destroyCrossTargetDedupCache(&cache);
}

void
test_shouldForwardAcrossTarget_suppressionDoesNotDisturbEstablishedBaseline(void) {
    GooseSubscriberCrossTargetDedupCache cache = { 0 };
    GooseSubscriberEntry* entriesA = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    TEST_ASSERT_TRUE(GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbA", entriesA, 2));

    GooseSubscriberEntry* entriesB = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    TEST_ASSERT_FALSE(GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbB", entriesB, 2));

    GooseSubscriberEntry* entriesC = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    bool result = GooseSubscriberUseCases_shouldForwardAcrossTarget(&cache, "LD/LLN0$GO$gcbC", entriesC, 2);

    TEST_ASSERT_FALSE_MESSAGE(result, "C must still be recognized as a duplicate of A's original content, "
            "even though B's suppressed record never touched the cache");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("LD/LLN0$GO$gcbA", cache.goCbRef,
            "the cache must still reflect A, the only one actually forwarded");

    freeCrossTargetDedupEntries(entriesA, 2);
    freeCrossTargetDedupEntries(entriesB, 2);
    freeCrossTargetDedupEntries(entriesC, 2);
    GooseSubscriberUseCases_destroyCrossTargetDedupCache(&cache);
}

void
test_shouldForwardAcrossTarget_isNoOp_whenCacheIsNull(void) {
    GooseSubscriberEntry* entries = makeCrossTargetDedupEntries(
            "LD/GGIO1$ST$SPCSO4$stVal", true, "LD/GGIO1$ST$SPCSO5$stVal", true);
    TEST_ASSERT_TRUE(GooseSubscriberUseCases_shouldForwardAcrossTarget(NULL, "LD/LLN0$GO$gcbA", entries, 2));
    freeCrossTargetDedupEntries(entries, 2);
}

void
test_destroyCrossTargetDedupCache_doesNotCrash_onNull(void) {
    GooseSubscriberUseCases_destroyCrossTargetDedupCache(NULL);
}

/* ---- detectStatusTransition ---- */

void
test_detectStatusTransition_validToInvalid_reportsStale(void) {
    GooseSubscriberStatus status;
    bool transitioned = GooseSubscriberUseCases_detectStatusTransition(true, false, &status);

    TEST_ASSERT_TRUE(transitioned);
    TEST_ASSERT_EQUAL_INT(GOOSE_SUBSCRIBER_STATUS_STALE, status);
}

void
test_detectStatusTransition_invalidToValid_reportsValid(void) {
    GooseSubscriberStatus status;
    bool transitioned = GooseSubscriberUseCases_detectStatusTransition(false, true, &status);

    TEST_ASSERT_TRUE(transitioned);
    TEST_ASSERT_EQUAL_INT(GOOSE_SUBSCRIBER_STATUS_VALID, status);
}

void
test_detectStatusTransition_noChange_returnsFalse(void) {
    GooseSubscriberStatus status;
    TEST_ASSERT_FALSE(GooseSubscriberUseCases_detectStatusTransition(true, true, &status));
    TEST_ASSERT_FALSE(GooseSubscriberUseCases_detectStatusTransition(false, false, &status));
}

/* ---- computeLivenessPollIntervalMs ---- */

void
test_computeLivenessPollIntervalMs_usesConfiguredValueWhenSet(void) {
    TEST_ASSERT_EQUAL_UINT32(500, GooseSubscriberUseCases_computeLivenessPollIntervalMs(500, 2000));
}

void
test_computeLivenessPollIntervalMs_derivesFromMinTal_flooredAt50ms(void) {
    /* 2000/4 = 500, above the floor */
    TEST_ASSERT_EQUAL_UINT32(500, GooseSubscriberUseCases_computeLivenessPollIntervalMs(0, 2000));
    /* 100/4 = 25, below the floor, clamped to 50 */
    TEST_ASSERT_EQUAL_UINT32(50, GooseSubscriberUseCases_computeLivenessPollIntervalMs(0, 100));
}

void
test_computeLivenessPollIntervalMs_fallsBackTo1000ms_whenNoTalKnown(void) {
    TEST_ASSERT_EQUAL_UINT32(1000, GooseSubscriberUseCases_computeLivenessPollIntervalMs(0, 0));
    TEST_ASSERT_EQUAL_UINT32(1000, GooseSubscriberUseCases_computeLivenessPollIntervalMs(0, -1));
}

/* ---- isDuplicateStNum ---- */

void
test_isDuplicateStNum_falseWhenNothingForwardedYet(void) {
    TEST_ASSERT_FALSE(GooseSubscriberUseCases_isDuplicateStNum(false, 0, 1));
    TEST_ASSERT_FALSE(GooseSubscriberUseCases_isDuplicateStNum(false, 5, 5));
}

void
test_isDuplicateStNum_trueWhenStNumUnchanged(void) {
    TEST_ASSERT_TRUE(GooseSubscriberUseCases_isDuplicateStNum(true, 5, 5));
}

void
test_isDuplicateStNum_falseWhenStNumAdvanced(void) {
    TEST_ASSERT_FALSE(GooseSubscriberUseCases_isDuplicateStNum(true, 5, 6));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_buildRecord_copiesScalarFields);
    RUN_TEST(test_buildRecord_deepCopiesEntries_notAliased);
    RUN_TEST(test_buildRecord_populatesReference_fromMemberReferences);
    RUN_TEST(test_buildRecord_referenceOutOfRange_leavesReferenceNull);
    RUN_TEST(test_buildRecord_handlesNullDataSetValues_zeroEntries);
    RUN_TEST(test_buildRecord_hasVlanFalse_leavesVlanFieldsAtZero);
    RUN_TEST(test_freeRecord_doesNotCrash_onNull);
    RUN_TEST(test_buildRecord_decomposesStructuredEntry_intoFlatLeaves);
    RUN_TEST(test_buildRecord_decomposition_countMismatch_fallsBackToRawEntry);
    RUN_TEST(test_buildRecord_decomposition_withWireTypesPresent_stillDecomposesWhenTypesMatch);

    RUN_TEST(test_buildRecord_firstEverValue_isSuppressed_andSeedsCache);
    RUN_TEST(test_buildRecord_unchangedValue_isDroppedAfterSeed);
    RUN_TEST(test_buildRecord_changedValue_isForwarded_andUpdatesCache);
    RUN_TEST(test_buildRecord_firstEverRealChange_noPriorBootstrap_previousValueIsNull);

    RUN_TEST(test_buildRecord_valueForwarded_dragsUnchangedQualitySibling);
    RUN_TEST(test_buildRecord_draggedAlongSibling_previousValueEqualsOwnCurrentValue);
    RUN_TEST(test_buildRecord_qualityForwarded_dragsUnchangedValueSibling);
    RUN_TEST(test_buildRecord_bothSiblingsUnchanged_neitherForwarded);
    RUN_TEST(test_buildRecord_ungroupableEntry_fallsBackToSoloDiffCheck);
    RUN_TEST(test_buildRecord_decomposedGroup_changedLeafDragsUnchangedSiblingLeaf);
    RUN_TEST(test_buildRecord_nestedCmvValue_dragsQualitySeveralAncestorLevelsUp);
    RUN_TEST(test_buildRecord_doesNotOverreach_pastAGenuinelyUnrelatedAncestor);

    RUN_TEST(test_buildRecord_firstFrame_seedsCache_andSetsEverPopulated);
    RUN_TEST(test_buildRecord_simulatedRecovery_genuineChangeForwards_withRealPreviousValue);
    RUN_TEST(test_buildRecord_simulatedRecovery_unchangedResend_isSuppressed);

    RUN_TEST(test_isDuplicateValue_falseWhenCachedIsNull);
    RUN_TEST(test_isDuplicateValue_trueWhenEqual);
    RUN_TEST(test_isDuplicateValue_falseWhenDifferent);
    RUN_TEST(test_isDuplicateValue_utcTime_sameMsDifferentQualityByte_isDuplicate);
    RUN_TEST(test_isDuplicateValue_utcTime_genuinelyDifferentMs_isNotDuplicate);
    RUN_TEST(test_isDuplicateValue_bitString_sameSizeSameBits_isDuplicate);
    RUN_TEST(test_isDuplicateValue_bitString_genuinelyDifferentBits_isNotDuplicate);
    RUN_TEST(test_isDuplicateValue_bitString_sameDecodedIntegerDifferentSize_isNotDuplicate);
    RUN_TEST(test_isDuplicateValue_typeMismatch_isNotDuplicate);

    RUN_TEST(test_shouldForwardAcrossTarget_firstEverContent_isForwarded_andSeedsCache);
    RUN_TEST(test_shouldForwardAcrossTarget_sameTargetIdenticalContent_isStillForwarded);
    RUN_TEST(test_shouldForwardAcrossTarget_differentTargetIdenticalContent_isSuppressed);
    RUN_TEST(test_shouldForwardAcrossTarget_differentTargetDifferentContent_isForwarded);
    RUN_TEST(test_shouldForwardAcrossTarget_suppressionDoesNotDisturbEstablishedBaseline);
    RUN_TEST(test_shouldForwardAcrossTarget_isNoOp_whenCacheIsNull);
    RUN_TEST(test_destroyCrossTargetDedupCache_doesNotCrash_onNull);

    RUN_TEST(test_detectStatusTransition_validToInvalid_reportsStale);
    RUN_TEST(test_detectStatusTransition_invalidToValid_reportsValid);
    RUN_TEST(test_detectStatusTransition_noChange_returnsFalse);

    RUN_TEST(test_computeLivenessPollIntervalMs_usesConfiguredValueWhenSet);
    RUN_TEST(test_computeLivenessPollIntervalMs_derivesFromMinTal_flooredAt50ms);
    RUN_TEST(test_computeLivenessPollIntervalMs_fallsBackTo1000ms_whenNoTalKnown);

    RUN_TEST(test_isDuplicateStNum_falseWhenNothingForwardedYet);
    RUN_TEST(test_isDuplicateStNum_trueWhenStNumUnchanged);
    RUN_TEST(test_isDuplicateStNum_falseWhenStNumAdvanced);

    return UNITY_END();
}
