#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- buildReportRecord ---- */

void
test_buildReportRecord_copiesScalarFields(void) {
    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL,
            true, 1700000000000ULL,
            true, 7,
            NULL, NULL, NULL, 0);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/LLN0.BR.brcbMain", record->rcbReference);
    TEST_ASSERT_TRUE(record->buffered);
    TEST_ASSERT_EQUAL_STRING("brcbMain", record->rptId);
    TEST_ASSERT_FALSE(record->hasEntryId);
    TEST_ASSERT_NULL(record->entryId);
    TEST_ASSERT_TRUE(record->hasTimestamp);
    TEST_ASSERT_EQUAL_UINT64(1700000000000ULL, record->timestampMs);
    TEST_ASSERT_TRUE(record->hasSeqNum);
    TEST_ASSERT_EQUAL_UINT16(7, record->seqNum);
    TEST_ASSERT_EQUAL_INT(0, record->entryCount);
    TEST_ASSERT_NULL(record->entries);

    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_deepCopiesEntries_notAliased(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));
    MmsValue_setElement(dataSetValues, 1, MmsValue_newIntegerFromInt32(99));

    ReasonForInclusion reasons[2] = { IEC61850_REASON_DATA_CHANGE, IEC61850_REASON_GI };
    char ref0[] = "Breaker1CB1/XCBR1.Pos.stVal";
    char ref1[] = "Breaker1CB1/MMXU1.TotW.mag";
    const char* dataReferences[2] = { ref0, ref1 };

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, dataReferences, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(2, record->entryCount);
    TEST_ASSERT_NOT_NULL(record->entries);

    TEST_ASSERT_TRUE(MmsValue_getBoolean(record->entries[0].value));
    TEST_ASSERT_EQUAL_INT32(99, MmsValue_toInt32(record->entries[1].value));
    TEST_ASSERT_EQUAL_STRING(ref0, record->entries[0].reference);
    TEST_ASSERT_EQUAL_STRING(ref1, record->entries[1].reference);
    TEST_ASSERT_EQUAL_INT(IEC61850_REASON_DATA_CHANGE, record->entries[0].reason);
    TEST_ASSERT_EQUAL_INT(IEC61850_REASON_GI, record->entries[1].reason);

    /* Mutate/free the inputs after the call - the record must be unaffected.
     * This is what proves the "must not borrow past the callback" contract,
     * since the real opaque ClientReport has no public constructor to test
     * this against directly. */
    MmsValue_setBoolean(MmsValue_getElement(dataSetValues, 0), false);
    ref0[0] = 'X';
    MmsValue_delete(dataSetValues);

    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getBoolean(record->entries[0].value),
            "entry value must be a deep copy, unaffected by later mutation/deletion of the source");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Breaker1CB1/XCBR1.Pos.stVal", record->entries[0].reference,
            "entry reference must be a deep copy, unaffected by later mutation of the source buffer");

    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_copiesEntryId_whenPresent(void) {
    MmsValue* entryId = MmsValue_newOctetString(8, 8);

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            true, entryId, false, 0, false, 0,
            NULL, NULL, NULL, 0);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_TRUE(record->hasEntryId);
    TEST_ASSERT_NOT_NULL(record->entryId);
    TEST_ASSERT_TRUE(record->entryId != entryId);

    MmsValue_delete(entryId);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_freeReportRecord_doesNotCrash_onNull(void) {
    MmsReportClientUseCases_freeReportRecord(NULL);
}

/* ---- computeNextBackoffDelay ---- */

void
test_computeNextBackoffDelay_returnsInitial_whenCurrentIsZero(void) {
    TEST_ASSERT_EQUAL_UINT32(1000, MmsReportClientUseCases_computeNextBackoffDelay(0, 1000, 30000));
}

void
test_computeNextBackoffDelay_doublesUntilCap(void) {
    uint32_t delay = 0;
    delay = MmsReportClientUseCases_computeNextBackoffDelay(delay, 1000, 30000);
    TEST_ASSERT_EQUAL_UINT32(1000, delay);
    delay = MmsReportClientUseCases_computeNextBackoffDelay(delay, 1000, 30000);
    TEST_ASSERT_EQUAL_UINT32(2000, delay);
    delay = MmsReportClientUseCases_computeNextBackoffDelay(delay, 1000, 30000);
    TEST_ASSERT_EQUAL_UINT32(4000, delay);
    delay = MmsReportClientUseCases_computeNextBackoffDelay(delay, 1000, 30000);
    TEST_ASSERT_EQUAL_UINT32(8000, delay);
    delay = MmsReportClientUseCases_computeNextBackoffDelay(delay, 1000, 30000);
    TEST_ASSERT_EQUAL_UINT32(16000, delay);
    delay = MmsReportClientUseCases_computeNextBackoffDelay(delay, 1000, 30000);
    TEST_ASSERT_EQUAL_UINT32(30000, delay); /* would be 32000, capped at 30000 */
    delay = MmsReportClientUseCases_computeNextBackoffDelay(delay, 1000, 30000);
    TEST_ASSERT_EQUAL_UINT32(30000, delay); /* stays capped */
}

void
test_computeNextBackoffDelay_doesNotOverflow_whenCurrentNearUint32Max(void) {
    uint32_t delay = MmsReportClientUseCases_computeNextBackoffDelay(3000000000U, 1000, 30000);
    TEST_ASSERT_EQUAL_UINT32(30000, delay);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_buildReportRecord_copiesScalarFields);
    RUN_TEST(test_buildReportRecord_deepCopiesEntries_notAliased);
    RUN_TEST(test_buildReportRecord_copiesEntryId_whenPresent);
    RUN_TEST(test_freeReportRecord_doesNotCrash_onNull);

    RUN_TEST(test_computeNextBackoffDelay_returnsInitial_whenCurrentIsZero);
    RUN_TEST(test_computeNextBackoffDelay_doublesUntilCap);
    RUN_TEST(test_computeNextBackoffDelay_doesNotOverflow_whenCurrentNearUint32Max);

    return UNITY_END();
}
