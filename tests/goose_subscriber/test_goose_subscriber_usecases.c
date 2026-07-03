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
            NULL, NULL, 0, 0);

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
            dataSetValues, NULL, 0, 2);

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
    const char* memberReferences[2] = { ref0, ref1 };

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, memberReferences, 2, 2);

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
    const char* memberReferences[1] = { "Reporter1LD1/GGIO1$ST$Ind1$stVal" };

    GooseSubscriberRecord* record = GooseSubscriberUseCases_buildRecord(
            "Breaker1CB1/LLN0$GO$gcbStatus", NULL, NULL,
            1, 0, 1, false, false, 2000, 0,
            false, 0, 0, -1,
            zeroMac, zeroMac,
            dataSetValues, memberReferences, 1, 2);

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
            NULL, NULL, 0, 0);

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
            NULL, NULL, 0, 0);

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

    RUN_TEST(test_detectStatusTransition_validToInvalid_reportsStale);
    RUN_TEST(test_detectStatusTransition_invalidToValid_reportsValid);
    RUN_TEST(test_detectStatusTransition_noChange_returnsFalse);

    RUN_TEST(test_computeLivenessPollIntervalMs_usesConfiguredValueWhenSet);
    RUN_TEST(test_computeLivenessPollIntervalMs_derivesFromMinTal_flooredAt50ms);
    RUN_TEST(test_computeLivenessPollIntervalMs_fallsBackTo1000ms_whenNoTalKnown);

    return UNITY_END();
}
