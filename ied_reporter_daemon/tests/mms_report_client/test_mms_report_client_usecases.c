#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/mms_report_client/domain/mms_report_client_usecases.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- isDuplicateValue (type-aware comparison) ----
 * See valuesAreSemanticallyEqual's own doc comment in
 * mms_report_client_usecases.c for the full real-hardware finding this
 * covers: MmsValue_equals is a raw byte-exact comparison, wrong for
 * MMS_UTC_TIME (includes the TimeQuality byte) and MMS_BIT_STRING (includes
 * unused padding bits) - both types show up constantly in real report
 * datasets. */

void
test_isDuplicateValue_utcTime_sameMsDifferentQualityByte_isDuplicate(void) {
    MmsValue* cached = MmsValue_newUtcTimeByMsTime(1700000000123ULL);
    MmsValue* newValue = MmsValue_newUtcTimeByMsTime(1700000000123ULL);
    /* Same millisecond timestamp, deliberately different TimeQuality byte
     * (leap-second-known/clock-failure/clock-not-synchronized/accuracy) -
     * exactly the real-world case (a device's clock-sync state wobbling
     * right around a reconnect) that made the old raw comparison unsafe. */
    MmsValue_setUtcTimeMsEx(cached, 1700000000123ULL, 0x00);
    MmsValue_setUtcTimeMsEx(newValue, 1700000000123ULL, 0x0A);

    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getUtcTimeInMs(cached) == MmsValue_getUtcTimeInMs(newValue),
            "sanity: both must render the identical millisecond timestamp");
    TEST_ASSERT_FALSE_MESSAGE(MmsValue_equals(cached, newValue),
            "sanity: the OLD raw byte-exact comparison sees these as different - documents the bug this fixes");

    TEST_ASSERT_TRUE_MESSAGE(MmsReportClientUseCases_isDuplicateValue(cached, newValue),
            "a differing TimeQuality byte alone must not be treated as a real change");

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_utcTime_genuinelyDifferentMs_isNotDuplicate(void) {
    MmsValue* cached = MmsValue_newUtcTimeByMsTime(1700000000123ULL);
    MmsValue* newValue = MmsValue_newUtcTimeByMsTime(1700000000456ULL);

    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_isDuplicateValue(cached, newValue),
            "a genuinely different millisecond timestamp must still be treated as a real change");

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

    TEST_ASSERT_TRUE(MmsReportClientUseCases_isDuplicateValue(cached, newValue));

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

    TEST_ASSERT_FALSE(MmsReportClientUseCases_isDuplicateValue(cached, newValue));

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_bitString_sameDecodedIntegerDifferentSize_isNotDuplicate(void) {
    /* Guards the size check added alongside the decoded-integer compare -
     * two genuinely different DA declarations (a real size difference) must
     * never be conflated just because they happen to decode to the same
     * low-order integer.
     *
     * NOTE: the real-world padding-bit scenario this fix primarily targets
     * (same declared size, differing UNUSED bits beyond that size within the
     * same allocated byte, see valuesAreSemanticallyEqual's own doc comment
     * in mms_report_client_usecases.c) can't be reproduced here -
     * MmsValue_setBitStringBit itself refuses to touch bit positions >=
     * size (confirmed directly in libiec61850's own source), so every
     * MmsValue this test can construct via the public API has its padding
     * bits permanently zeroed by MmsValue_newBitString's own calloc. That
     * gap only exists in a real device's own wire encoding, not in
     * anything reachable through well-behaved client code - which is
     * exactly why it was a genuine, hard-to-suspect field bug. */
    MmsValue* cached = MmsValue_newBitString(2);
    MmsValue_setBitStringBit(cached, 0, true);
    MmsValue_setBitStringBit(cached, 1, false);

    MmsValue* newValue = MmsValue_newBitString(8);
    MmsValue_setBitStringBit(newValue, 0, true);

    TEST_ASSERT_EQUAL_UINT32(MmsValue_getBitStringAsInteger(cached), MmsValue_getBitStringAsInteger(newValue));
    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_isDuplicateValue(cached, newValue),
            "a genuine size difference must never be masked by a coincidentally-matching decoded integer");

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_typeMismatch_isNotDuplicate(void) {
    MmsValue* cached = MmsValue_newBoolean(true);
    MmsValue* newValue = MmsValue_newBitString(2);
    MmsValue_setBitStringBit(newValue, 0, true);

    TEST_ASSERT_FALSE(MmsReportClientUseCases_isDuplicateValue(cached, newValue));

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

void
test_isDuplicateValue_booleanUnchanged_isDuplicate(void) {
    MmsValue* cached = MmsValue_newBoolean(true);
    MmsValue* newValue = MmsValue_newBoolean(true);

    TEST_ASSERT_TRUE_MESSAGE(MmsReportClientUseCases_isDuplicateValue(cached, newValue),
            "non-implicated types must still fall through to the unchanged MmsValue_equals path");

    MmsValue_delete(cached);
    MmsValue_delete(newValue);
}

/* ---- isEntryIdStale (non-monotonic/duplicate EntryID guard) ----
 * See MmsReportClientUseCases_isEntryIdStale's own doc comment for the full
 * real-hardware finding this covers: EntryID observed arriving as
 * 1,4,5,6,7,8,1,9,4,A,5,... within one continuous session - duplicate/stale
 * redelivery interleaved with genuinely new entries. */

static MmsValue*
newEntryId(const uint8_t* bytes, int size) {
    MmsValue* v = MmsValue_newOctetString(size, size);
    for (int i = 0; i < size; i++) MmsValue_setOctetStringOctet(v, i, bytes[i]);
    return v;
}

void
test_isEntryIdStale_incomingGreaterThanLastSeen_isNotStale(void) {
    uint8_t lastBytes[8] = { 0, 0, 0, 0, 0, 0, 0, 5 };
    uint8_t incomingBytes[8] = { 0, 0, 0, 0, 0, 0, 0, 8 };
    MmsValue* lastSeen = newEntryId(lastBytes, 8);
    MmsValue* incoming = newEntryId(incomingBytes, 8);

    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_isEntryIdStale(incoming, lastSeen),
            "a genuinely newer EntryID must never be treated as stale");

    MmsValue_delete(lastSeen);
    MmsValue_delete(incoming);
}

void
test_isEntryIdStale_incomingEqualsLastSeen_isStale(void) {
    /* The exact "EntryID 1 recurring" case from the real capture. */
    uint8_t bytes[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
    MmsValue* lastSeen = newEntryId(bytes, 8);
    MmsValue* incoming = newEntryId(bytes, 8);

    TEST_ASSERT_TRUE_MESSAGE(MmsReportClientUseCases_isEntryIdStale(incoming, lastSeen),
            "an exact repeat of the last-seen EntryID must be treated as stale/duplicate redelivery");

    MmsValue_delete(lastSeen);
    MmsValue_delete(incoming);
}

void
test_isEntryIdStale_incomingLessThanLastSeen_isStale(void) {
    /* The out-of-order case from the real capture (...,8,1,...). */
    uint8_t lastBytes[8] = { 0, 0, 0, 0, 0, 0, 0, 8 };
    uint8_t incomingBytes[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
    MmsValue* lastSeen = newEntryId(lastBytes, 8);
    MmsValue* incoming = newEntryId(incomingBytes, 8);

    TEST_ASSERT_TRUE_MESSAGE(MmsReportClientUseCases_isEntryIdStale(incoming, lastSeen),
            "an EntryID older than the last one already durably processed must be treated as stale");

    MmsValue_delete(lastSeen);
    MmsValue_delete(incoming);
}

void
test_isEntryIdStale_incomingNull_isNotStale_failsOpen(void) {
    uint8_t bytes[8] = { 0, 0, 0, 0, 0, 0, 0, 5 };
    MmsValue* lastSeen = newEntryId(bytes, 8);

    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_isEntryIdStale(NULL, lastSeen),
            "a report with no EntryID at all must never be judged stale (fails open)");

    MmsValue_delete(lastSeen);
}

void
test_isEntryIdStale_lastSeenNull_isNotStale_failsOpen(void) {
    /* The bootstrap/first-ever-report case - nothing to compare against yet. */
    uint8_t bytes[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
    MmsValue* incoming = newEntryId(bytes, 8);

    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_isEntryIdStale(incoming, NULL),
            "the first-ever report for an RCB (no cached lastEntryId yet) must never be judged stale");

    MmsValue_delete(incoming);
}

void
test_isEntryIdStale_mismatchedByteSizes_isNotStale_failsOpen(void) {
    uint8_t lastBytes[8] = { 0, 0, 0, 0, 0, 0, 0, 5 };
    uint8_t incomingBytes[4] = { 0, 0, 0, 5 };
    MmsValue* lastSeen = newEntryId(lastBytes, 8);
    MmsValue* incoming = newEntryId(incomingBytes, 4);

    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_isEntryIdStale(incoming, lastSeen),
            "an EntryID shape this function doesn't recognize must fail open rather than drop a report");

    MmsValue_delete(lastSeen);
    MmsValue_delete(incoming);
}

void
test_isEntryIdStale_nonOctetStringType_isNotStale_failsOpen(void) {
    uint8_t bytes[8] = { 0, 0, 0, 0, 0, 0, 0, 5 };
    MmsValue* lastSeen = newEntryId(bytes, 8);
    MmsValue* incoming = MmsValue_newBoolean(true);

    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_isEntryIdStale(incoming, lastSeen),
            "a non-octet-string EntryID (unexpected shape) must fail open rather than drop a report");

    MmsValue_delete(lastSeen);
    MmsValue_delete(incoming);
}

void
test_isEntryIdStale_multiByteBigEndianOrdering_isCorrect(void) {
    /* {0x01,0x00} = 256, {0x00,0xFF} = 255 - proves this is a real big-endian
     * numeric comparison, not accidentally a last-byte-only or
     * little-endian one. */
    uint8_t lastBytes[2] = { 0x00, 0xFF };
    uint8_t incomingBytes[2] = { 0x01, 0x00 };
    MmsValue* lastSeen = newEntryId(lastBytes, 2);
    MmsValue* incoming = newEntryId(incomingBytes, 2);

    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_isEntryIdStale(incoming, lastSeen),
            "256 must be recognized as greater than 255 under big-endian byte comparison");

    MmsValue_delete(lastSeen);
    MmsValue_delete(incoming);
}

/* ---- shouldRequestGiOnEnable ---- */

void
test_shouldRequestGiOnEnable_unbufferedWithNoResumableEntryId_requestsGi(void) {
    TEST_ASSERT_TRUE_MESSAGE(MmsReportClientUseCases_shouldRequestGiOnEnable(false, false),
            "an unbuffered RCB has no backlog at all - GI is the only way to catch a change made "
            "while disconnected, so it must always be requested");
}

void
test_shouldRequestGiOnEnable_unbufferedWithResumableEntryId_stillRequestsGi(void) {
    /* hasResumableEntryId is meaningless for an unbuffered RCB (EntryID only
     * applies to buffered ones) - must not accidentally suppress GI here. */
    TEST_ASSERT_TRUE_MESSAGE(MmsReportClientUseCases_shouldRequestGiOnEnable(false, true),
            "GI must still be requested for an unbuffered RCB regardless of the resumable flag");
}

void
test_shouldRequestGiOnEnable_bufferedWithNoResumableEntryId_requestsGi(void) {
    /* First-ever enable, or after an EntryID rejection resets the cache back
     * to NULL - nothing to resume from, same full-backlog safety net as
     * before GI became conditional. */
    TEST_ASSERT_TRUE_MESSAGE(MmsReportClientUseCases_shouldRequestGiOnEnable(true, false),
            "a buffered RCB with nothing to resume from yet must still request GI");
}

void
test_shouldRequestGiOnEnable_bufferedWithResumableEntryId_skipsGi(void) {
    /* The one case this function exists for: a buffered RCB's own EntryID
     * resume already guarantees delivery of everything that happened while
     * disconnected - GI adds nothing and was found polluting the backlog
     * with duplicate snapshots on real hardware. */
    TEST_ASSERT_FALSE_MESSAGE(MmsReportClientUseCases_shouldRequestGiOnEnable(true, true),
            "a buffered RCB with a valid resumable EntryID must skip GI");
}

/* ---- buildReportRecord ---- */

void
test_buildReportRecord_copiesScalarFields(void) {
    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL,
            true, 1700000000000ULL,
            true, 7,
            NULL, NULL, NULL, NULL, 0);

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

    ReasonForInclusion reasons[2] = { IEC61850_REASON_DATA_CHANGE, IEC61850_REASON_QUALITY_CHANGE };
    char ref0[] = "Breaker1CB1/XCBR1.Pos.stVal";
    char ref1[] = "Breaker1CB1/MMXU1.TotW.mag";
    const char* dataReferences[2] = { ref0, ref1 };

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, dataReferences, NULL, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(2, record->entryCount);
    TEST_ASSERT_NOT_NULL(record->entries);

    TEST_ASSERT_TRUE(MmsValue_getBoolean(record->entries[0].value));
    TEST_ASSERT_EQUAL_INT32(99, MmsValue_toInt32(record->entries[1].value));
    TEST_ASSERT_EQUAL_STRING(ref0, record->entries[0].reference);
    TEST_ASSERT_EQUAL_STRING(ref1, record->entries[1].reference);
    TEST_ASSERT_EQUAL_INT(IEC61850_REASON_DATA_CHANGE, record->entries[0].reason);
    TEST_ASSERT_EQUAL_INT(IEC61850_REASON_QUALITY_CHANGE, record->entries[1].reason);

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
test_buildReportRecord_prefersServerDataReference_overFallback(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    const char* dataReferences[1] = { "Server/Supplied.reference" };
    char* fallbackReferences[1] = { "Fallback/Resolved.reference" };
    MmsReportClientMemberRefCacheEntry memberRefCache = { 0 };
    memberRefCache.memberReferences = fallbackReferences;
    memberRefCache.memberCount = 1;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, NULL, dataReferences, &memberRefCache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_STRING("Server/Supplied.reference", record->entries[0].reference);

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_usesFallbackReference_whenServerDataReferenceMissing(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    char fallbackBuf[] = "Fallback/Resolved.reference";
    char* fallbackReferences[1] = { fallbackBuf };
    MmsReportClientMemberRefCacheEntry memberRefCache = { 0 };
    memberRefCache.memberReferences = fallbackReferences;
    memberRefCache.memberCount = 1;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, NULL, NULL, &memberRefCache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_STRING("Fallback/Resolved.reference", record->entries[0].reference);

    /* Mutate the source buffer after the call - proves the fallback string is
     * deep-copied too, same aliasing guarantee as server-supplied references. */
    fallbackBuf[0] = 'X';
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Fallback/Resolved.reference", record->entries[0].reference,
            "fallback reference must be a deep copy, unaffected by later mutation of the source buffer");

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_fallbackOutOfRange_leavesReferenceNull(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false));

    char* fallbackReferences[1] = { "Fallback/Resolved.reference" };
    MmsReportClientMemberRefCacheEntry memberRefCache = { 0 };
    memberRefCache.memberReferences = fallbackReferences;
    memberRefCache.memberCount = 1; /* only covers index 0 - dataset has 2 entries */

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, NULL, NULL, &memberRefCache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_STRING("Fallback/Resolved.reference", record->entries[0].reference);
    TEST_ASSERT_NULL(record->entries[1].reference);

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_copiesEntryId_whenPresent(void) {
    MmsValue* entryId = MmsValue_newOctetString(8, 8);

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            true, entryId, false, 0, false, 0,
            NULL, NULL, NULL, NULL, 0);

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

/* ---- buildReportRecord: hybrid event filter (value-diff cache) ----
 *
 * Without a memberRefCache (NULL, as most tests above pass), there is no
 * value-diff cache slot to gate on, so every entry is an unconditional
 * passthrough regardless of reason - these tests instead build a real
 * MmsReportClientMemberRefCacheEntry with its leafSlotOffsets/
 * lastForwardedValues populated, the same shape mms_report_client_api.c's
 * buildMemberRefCache constructs at MmsReportClient_start. */

void
test_buildReportRecord_giReason_firstEverValue_isSuppressed_andSeedsCache(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    ReasonForInclusion reasons[1] = { IEC61850_REASON_GI };

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { NULL }; /* never cached yet */
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "a GI-reasoned report (this client now deterministically requests GI itself on every "
            "enable, but must handle one identically regardless of source - e.g. a foreign "
            "client's own interrogation) must never reach the websocket when it's the very first "
            "report for this position - it's cache-seed-only");
    TEST_ASSERT_NULL(record->entries);
    TEST_ASSERT_NOT_NULL_MESSAGE(cache.lastForwardedValues[0],
            "the cache slot must still be silently seeded, so the first GENUINE change afterward "
            "has a real previous value to report");
    TEST_ASSERT_TRUE(MmsValue_getBoolean(cache.lastForwardedValues[0]));

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_integrityReason_unchangedValue_isDroppedAfterSeed(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    ReasonForInclusion reasons[1] = { IEC61850_REASON_INTEGRITY };

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(true) }; /* already forwarded once, same value */
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "a periodic-only re-send of an unchanged value must be dropped once the cache is seeded");
    TEST_ASSERT_NULL(record->entries);

    MmsValue_delete(lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_integrityReason_changedValue_isForwarded(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    ReasonForInclusion reasons[1] = { IEC61850_REASON_INTEGRITY };

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(false) }; /* last forwarded value differs */
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, record->entryCount,
            "a periodic-only entry whose value genuinely differs from the cache must still survive");
    TEST_ASSERT_NOT_NULL(cache.lastForwardedValues[0]);
    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getBoolean(cache.lastForwardedValues[0]),
            "the cache must be updated to the new value after forwarding");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_dataChangeReason_sameValueAsCache_isDropped(void) {
    /* Regression test for a real-hardware finding: a live IED was observed
     * tagging hundreds of consecutive, byte-identical reports as DATA_CHANGE
     * even though the value never actually changed (confirmed via
     * previousValue == value on every one of them). The reason bit is no
     * longer trusted as a bypass of the value-diff check - see
     * shouldForwardAndUpdateCache's own doc comment for the full story. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    ReasonForInclusion reasons[1] = { IEC61850_REASON_DATA_CHANGE };

    int leafSlotOffsets[1] = { 0 };
    /* server says DATA_CHANGE, but the value happens to equal the cache */
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(true) };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "a DATA_CHANGE reason must NOT bypass the value-diff check - a real device has "
            "been observed to tag unchanged resends as DATA_CHANGE, and this must still be "
            "dropped like any other unchanged duplicate");
    TEST_ASSERT_NULL(record->entries);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_realChangeReason_previousValueEqualsPriorCache(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    ReasonForInclusion reasons[1] = { IEC61850_REASON_DATA_CHANGE };

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(false) }; /* prior cache */
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(1, record->entryCount);
    TEST_ASSERT_NOT_NULL_MESSAGE(record->entries[0].previousValue,
            "a real DATA_CHANGE report must carry the prior cached value as its previousValue");
    TEST_ASSERT_FALSE_MESSAGE(MmsValue_getBoolean(record->entries[0].previousValue),
            "previousValue must be the OLD cached value, not the new one");
    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getBoolean(record->entries[0].value), "value itself must be the new one");
    TEST_ASSERT_TRUE_MESSAGE(record->entries[0].previousValue != cache.lastForwardedValues[0],
            "previousValue must be an independent clone, not aliased to the live cache slot");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_firstEverRealChange_noPriorBootstrap_previousValueIsNull(void) {
    /* slot < 0 (no memberRefCache at all) - the one accepted structural case
     * where previousValue can never be known. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    ReasonForInclusion reasons[1] = { IEC61850_REASON_DATA_CHANGE };

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, NULL, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(1, record->entryCount);
    TEST_ASSERT_NULL(record->entries[0].previousValue);

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_unknownReason_threeCallSequence_matchesNoReasonCodeDevice(void) {
    /* Reproduces a real device that never populates ReasonForInclusion at all
     * (every entry reads back as IEC61850_REASON_UNKNOWN) - the value-diff
     * cache is the only thing that can filter its periodic re-sends. */
    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { NULL };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    ReasonForInclusion reasons[1] = { IEC61850_REASON_UNKNOWN };

    /* Call 1: first-ever value is bootstrap-suppressed (cache-seed only). */
    MmsValue* dataSetValues1 = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues1, 0, MmsValue_newBoolean(true));
    MmsReportRecord* record1 = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues1, reasons, NULL, &cache, 1);
    TEST_ASSERT_EQUAL_INT(0, record1->entryCount);
    TEST_ASSERT_NOT_NULL_MESSAGE(cache.lastForwardedValues[0], "call 1 must still silently seed the cache");
    MmsValue_delete(dataSetValues1);
    MmsReportClientUseCases_freeReportRecord(record1);

    /* Call 2: identical value re-sent - must be dropped. */
    MmsValue* dataSetValues2 = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues2, 0, MmsValue_newBoolean(true));
    MmsReportRecord* record2 = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues2, reasons, NULL, &cache, 1);
    TEST_ASSERT_EQUAL_INT(0, record2->entryCount);
    MmsValue_delete(dataSetValues2);
    MmsReportClientUseCases_freeReportRecord(record2);

    /* Call 3: genuinely different value - must survive, with call 1/2's
     * bootstrap-seeded value as its previousValue. */
    MmsValue* dataSetValues3 = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues3, 0, MmsValue_newBoolean(false));
    MmsReportRecord* record3 = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues3, reasons, NULL, &cache, 1);
    TEST_ASSERT_EQUAL_INT(1, record3->entryCount);
    TEST_ASSERT_NOT_NULL_MESSAGE(record3->entries[0].previousValue,
            "the bootstrap-seeded value must surface as the previous value on the first genuine change");
    TEST_ASSERT_TRUE(MmsValue_getBoolean(record3->entries[0].previousValue));
    MmsValue_delete(dataSetValues3);
    MmsReportClientUseCases_freeReportRecord(record3);

    MmsValue_delete(cache.lastForwardedValues[0]);
}

/* ---- buildReportRecord: Gap 4 structure decomposition ---- */

void
test_buildReportRecord_decomposesStructuredEntry_intoFlatLeaves(void) {
    MmsValue* stVal = MmsValue_newBoolean(true);
    MmsValue* q = MmsValue_newBitString(13);
    MmsValue* structVal = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal, 0, stVal);
    MmsValue_setElement(structVal, 1, q);

    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    ReasonForInclusion reasons[1] = { IEC61850_REASON_DATA_CHANGE };

    char* leafRefs0[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 2 };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(2, record->entryCount);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$stVal", record->entries[0].reference);
    TEST_ASSERT_TRUE(MmsValue_getBoolean(record->entries[0].value));
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$q", record->entries[1].reference);
    TEST_ASSERT_EQUAL_INT(IEC61850_REASON_DATA_CHANGE, record->entries[0].reason);
    TEST_ASSERT_EQUAL_INT(IEC61850_REASON_DATA_CHANGE, record->entries[1].reason);

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_decomposition_countMismatch_fallsBackToRawEntry(void) {
    /* Claims 3 leaves, but the actual structure only has 2 elements - the
     * wire-order assumption broke for this position, must not mis-pair. */
    MmsValue* stVal = MmsValue_newBoolean(true);
    MmsValue* q = MmsValue_newBitString(13);
    MmsValue* structVal = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal, 0, stVal);
    MmsValue_setElement(structVal, 1, q);

    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    ReasonForInclusion reasons[1] = { IEC61850_REASON_DATA_CHANGE };

    char* memberRefs[1] = { "Breaker1CB1/XCBR1.Pos" };
    char* leafRefs0[3] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q", "Breaker1CB1/XCBR1.Pos$t" };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 3 }; /* mismatch: structVal only has 2 elements */
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberReferences = memberRefs;
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, record->entryCount,
            "a leaf-count mismatch must fall back to one raw (non-decomposed) entry, not mis-paired leaves");
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos", record->entries[0].reference);

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_decomposition_withWireTypesPresent_stillDecomposesWhenTypesMatch(void) {
    /* Regression case: memberLeafWireTypes populated AND genuinely matching
     * the wire's actual types must still decompose normally - the new
     * per-leaf type check must not false-positive-reject a well-ordered
     * structure. */
    MmsValue* stVal = MmsValue_newBoolean(true);
    MmsValue* q = MmsValue_newBitString(13);
    MmsValue* structVal = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal, 0, stVal);
    MmsValue_setElement(structVal, 1, q);

    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    ReasonForInclusion reasons[1] = { IEC61850_REASON_DATA_CHANGE };

    char* leafRefs0[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 2 };
    DataAttributeType wireTypes0[2] = { IEC61850_BOOLEAN, IEC61850_QUALITY };
    DataAttributeType* memberLeafWireTypes[1] = { wireTypes0 };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;
    cache.memberLeafWireTypes = memberLeafWireTypes;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "matching wire types must not block a genuinely well-ordered decomposition");
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$stVal", record->entries[0].reference);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$q", record->entries[1].reference);

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_decomposition_stValAndStSeld_reorderedByTypeNotPosition(void) {
    /* Regression test for a real production bug: a DPC "Pos"'s stVal (Dbpos,
     * IEC61850_CODEDENUM -> a 2-bit MMS_BIT_STRING) and stSeld
     * (IEC61850_BOOLEAN -> MMS_BOOLEAN) got their reference/value pairs
     * swapped in the live report stream whenever the device's real wire
     * encoding order for those two non-q/t siblings didn't match this
     * daemon's locally-resolved SCL <DOType> order - both orderings have the
     * same leaf count, so the old code's blind positional fallback (used
     * for anything besides "q"/"t") silently mispaired them. Wire order here
     * deliberately swaps stVal/stSeld's ends relative to leafRefs0's model
     * order, while q/t still resolve correctly via their own fixed wire
     * type - exactly the shape reorderFlattenedToMatchReferences's
     * expectedTypes disambiguation pass exists to catch. */
    char* leafRefs0[4] = {
        "Breaker1CB1/XSWI1.Pos$stVal", "Breaker1CB1/XSWI1.Pos$q",
        "Breaker1CB1/XSWI1.Pos$t", "Breaker1CB1/XSWI1.Pos$stSeld",
    };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 4 };
    DataAttributeType wireTypes0[4] = {
        IEC61850_CODEDENUM, IEC61850_QUALITY, IEC61850_TIMESTAMP, IEC61850_BOOLEAN,
    };
    DataAttributeType* memberLeafWireTypes[1] = { wireTypes0 };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;
    cache.memberLeafWireTypes = memberLeafWireTypes;

    /* Wire/report encoding order: stSeld, q, t, stVal - the two ambiguous
     * non-q/t siblings sit at opposite ends versus leafRefs0's model order. */
    MmsValue* stSeldWire = MmsValue_newBoolean(false);
    MmsValue* qWire = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(qWire, 0);
    MmsValue* tWire = MmsValue_newUtcTimeByMsTime(1700000000000ULL);
    MmsValue* stValWire = MmsValue_newBitString(2);
    MmsValue_setBitStringFromInteger(stValWire, 2); /* Dbpos "on" */

    MmsValue* structVal = MmsValue_createEmptyStructure(4);
    MmsValue_setElement(structVal, 0, stSeldWire);
    MmsValue_setElement(structVal, 1, qWire);
    MmsValue_setElement(structVal, 2, tWire);
    MmsValue_setElement(structVal, 3, stValWire);

    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    ReasonForInclusion reasons[1] = { IEC61850_REASON_DATA_CHANGE };

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.RP.rcbMain", false, "rcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, record->entryCount,
            "all four leaves of a well-ordered (count-matching) decomposition must still forward");

    bool sawStVal = false, sawStSeld = false;
    for (int i = 0; i < record->entryCount; i++) {
        MmsReportEntry* entry = &record->entries[i];
        if (strcmp(entry->reference, "Breaker1CB1/XSWI1.Pos$stVal") == 0) {
            sawStVal = true;
            TEST_ASSERT_EQUAL_INT_MESSAGE(MMS_BIT_STRING, MmsValue_getType(entry->value),
                    "stVal must carry the real Dbpos bitstring value, not stSeld's boolean");
            TEST_ASSERT_EQUAL_INT(2, MmsValue_getBitStringSize(entry->value));
            TEST_ASSERT_EQUAL_UINT32(2, MmsValue_getBitStringAsInteger(entry->value));
        } else if (strcmp(entry->reference, "Breaker1CB1/XSWI1.Pos$stSeld") == 0) {
            sawStSeld = true;
            TEST_ASSERT_EQUAL_INT_MESSAGE(MMS_BOOLEAN, MmsValue_getType(entry->value),
                    "stSeld must carry the real boolean value, not stVal's Dbpos bitstring");
            TEST_ASSERT_FALSE(MmsValue_getBoolean(entry->value));
        } else if (strcmp(entry->reference, "Breaker1CB1/XSWI1.Pos$q") == 0) {
            TEST_ASSERT_EQUAL_INT(MMS_BIT_STRING, MmsValue_getType(entry->value));
            TEST_ASSERT_EQUAL_INT(13, MmsValue_getBitStringSize(entry->value));
        } else if (strcmp(entry->reference, "Breaker1CB1/XSWI1.Pos$t") == 0) {
            TEST_ASSERT_EQUAL_INT(MMS_UTC_TIME, MmsValue_getType(entry->value));
        } else {
            TEST_FAIL_MESSAGE("unexpected entry reference");
        }
    }
    TEST_ASSERT_TRUE(sawStVal);
    TEST_ASSERT_TRUE(sawStSeld);

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_decomposedGiEntry_isSuppressed_seedsCache_thenDuplicateDropped(void) {
    char* leafRefs0[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 2 };
    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[2] = { NULL, NULL };

    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;

    ReasonForInclusion giReasons[1] = { IEC61850_REASON_GI };
    ReasonForInclusion integrityReasons[1] = { IEC61850_REASON_INTEGRITY };

    /* Call 1 (GI, first ever): both leaves are suppressed but seed the cache. */
    MmsValue* structVal1 = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal1, 0, MmsValue_newBoolean(true));
    MmsValue_setElement(structVal1, 1, MmsValue_newBitString(13));
    MmsValue* dataSetValues1 = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues1, 0, structVal1);

    MmsReportRecord* record1 = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues1, giReasons, NULL, &cache, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record1->entryCount,
            "the first-ever GI report must never deliver either decomposed leaf to the websocket");
    TEST_ASSERT_NOT_NULL_MESSAGE(cache.lastForwardedValues[0], "both leaf slots must still be seeded");
    TEST_ASSERT_NOT_NULL_MESSAGE(cache.lastForwardedValues[1], "both leaf slots must still be seeded");
    MmsValue_delete(dataSetValues1);
    MmsReportClientUseCases_freeReportRecord(record1);

    /* Call 2 (INTEGRITY, identical values): both leaves are duplicates, dropped. */
    MmsValue* structVal2 = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal2, 0, MmsValue_newBoolean(true));
    MmsValue_setElement(structVal2, 1, MmsValue_newBitString(13));
    MmsValue* dataSetValues2 = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues2, 0, structVal2);

    MmsReportRecord* record2 = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues2, integrityReasons, NULL, &cache, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record2->entryCount,
            "a subsequent periodic re-send of the same decomposed values must be filtered entirely");
    MmsValue_delete(dataSetValues2);
    MmsReportClientUseCases_freeReportRecord(record2);

    MmsValue_delete(lastForwardedValues[0]);
    MmsValue_delete(lastForwardedValues[1]);
}

/* ---- buildReportRecord: group-aware forwarding (value <-> quality pairing fix) ---- */

void
test_buildReportRecord_valueForwarded_dragsUnchangedQualitySibling(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));  /* stVal: differs from cache */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false)); /* q: matches cache */

    ReasonForInclusion reasons[2] = { IEC61850_REASON_DATA_CHANGE, IEC61850_REASON_UNKNOWN };

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), MmsValue_newBoolean(false) };
    char* memberReferences[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "quality's own diff-check says unchanged, but it must still be dragged along "
            "because its value sibling (same DO) was forwarded due to a real change");
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$stVal", record->entries[0].reference);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/XCBR1.Pos$q", record->entries[1].reference);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_qualityForwarded_dragsUnchangedValueSibling(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(false)); /* stVal: matches cache */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(true));  /* q: differs from cache */

    ReasonForInclusion reasons[2] = { IEC61850_REASON_UNKNOWN, IEC61850_REASON_QUALITY_CHANGE };

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), MmsValue_newBoolean(false) };
    char* memberReferences[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "a genuine quality-only change must not be dropped as a lone entry - its unchanged "
            "value sibling must be dragged along too, matching what ipc_dispatcher needs to pair them");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_draggedAlongSibling_previousValueEqualsOwnCurrentValue(void) {
    /* Same scenario as test_buildReportRecord_valueForwarded_dragsUnchangedQualitySibling
     * (quality's own diff-check says unchanged, dragged in by its changed
     * value sibling) - here asserting the dragged-along quality's own
     * previousValue: since it didn't itself change, previousValue must equal
     * its own current value (that's exactly why it needed dragging in - see
     * this repo's own documented judgment call on "changed" being judged at
     * the pair level, not the raw scalar level). */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));  /* stVal: differs from cache */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false)); /* q: matches cache */

    ReasonForInclusion reasons[2] = { IEC61850_REASON_DATA_CHANGE, IEC61850_REASON_UNKNOWN };

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), MmsValue_newBoolean(false) };
    char* memberReferences[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 2);

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
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_bothSiblingsUnchanged_neitherForwarded(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(false));
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false));

    ReasonForInclusion reasons[2] = { IEC61850_REASON_UNKNOWN, IEC61850_REASON_UNKNOWN };

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newBoolean(false), MmsValue_newBoolean(false) };
    char* memberReferences[2] = { "Breaker1CB1/XCBR1.Pos$stVal", "Breaker1CB1/XCBR1.Pos$q" };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(0, record->entryCount);
    TEST_ASSERT_NULL(record->entries);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_ungroupableEntry_fallsBackToSoloDiffCheck(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true)); /* differs from cache */

    ReasonForInclusion reasons[1] = { IEC61850_REASON_UNKNOWN };

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(false) };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;
    /* memberReferences left NULL - this entry's reference can never resolve,
     * so it must be its own ungroupable singleton, behaving exactly like the
     * pre-existing (pre-grouping) solo diff-check. */

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(1, record->entryCount);
    TEST_ASSERT_NULL(record->entries[0].reference);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_decomposedGroup_changedLeafDragsUnchangedSiblingLeaf(void) {
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

    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;

    /* Shared reason - both leaves of one decomposed DO-level position always
     * share the raw position's one reason, so only the value-diff check can
     * decide either leaf's fate individually here. */
    ReasonForInclusion reasons[1] = { IEC61850_REASON_INTEGRITY };

    MmsValue* newQ = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(newQ, 0);
    MmsValue* structVal = MmsValue_createEmptyStructure(2);
    MmsValue_setElement(structVal, 0, MmsValue_newBoolean(true)); /* stVal: differs from cache */
    MmsValue_setElement(structVal, 1, newQ);                     /* q: matches cache */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "the unchanged q leaf must be dragged along by its changed stVal sibling, even "
            "though both leaves came from the same decomposed DO-level position (Gap 4)");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

/* Regression test for the "only send what actually changed" websocket fix:
 * a real DPC-shaped decomposition has FOUR leaves (t/stVal/stSeld/q), not
 * just two (value/q). The group-extension pass above still drags all four
 * into record->entries (that membership is deliberately unchanged - see
 * buildEntries' own doc comment), but MmsReportEntry.ownChangeDetected must
 * distinguish stVal (the one leaf that actually changed) from t/stSeld/q
 * (unchanged, only present because a sibling changed) - it's what
 * ipc_dispatcher's IpcDispatcherUseCases_shouldIncludeValuePoint uses to
 * suppress the latter three from the outbound websocket message. */
void
test_buildReportRecord_decomposedGroup_fourLeafDpc_onlyChangedLeafHasOwnChangeDetected(void) {
    char* leafRefs0[4] = {
        "Breaker1CB1/XSWI1.Pos$t", "Breaker1CB1/XSWI1.Pos$stVal",
        "Breaker1CB1/XSWI1.Pos$stSeld", "Breaker1CB1/XSWI1.Pos$q",
    };
    char** memberLeafReferences[1] = { leafRefs0 };
    int memberLeafCounts[1] = { 4 };
    int leafSlotOffsets[1] = { 0 };

    MmsValue* cachedQ = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(cachedQ, 0);
    MmsValue* lastForwardedValues[4] = {
        MmsValue_newUtcTimeByMsTime(1700000000000ULL), /* t: will match new value */
        MmsValue_newBoolean(false),                    /* stVal: will differ from new value */
        MmsValue_newBoolean(false),                    /* stSeld: will match new value */
        cachedQ,                                       /* q: will match new value */
    };

    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.memberLeafReferences = memberLeafReferences;
    cache.memberLeafCounts = memberLeafCounts;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 4;
    cache.lastForwardedValues = lastForwardedValues;

    ReasonForInclusion reasons[1] = { IEC61850_REASON_DATA_CHANGE };

    MmsValue* tWire = MmsValue_newUtcTimeByMsTime(1700000000000ULL); /* unchanged */
    MmsValue* stValWire = MmsValue_newBoolean(true);                /* CHANGED: false -> true */
    MmsValue* stSeldWire = MmsValue_newBoolean(false);               /* unchanged */
    MmsValue* qWire = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(qWire, 0);                      /* unchanged */

    MmsValue* structVal = MmsValue_createEmptyStructure(4);
    MmsValue_setElement(structVal, 0, tWire);
    MmsValue_setElement(structVal, 1, stValWire);
    MmsValue_setElement(structVal, 2, stSeldWire);
    MmsValue_setElement(structVal, 3, qWire);

    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, structVal);

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.RP.rcbMain", false, "rcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, record->entryCount,
            "group membership itself must stay unchanged - all four leaves still forward");

    for (int i = 0; i < record->entryCount; i++) {
        MmsReportEntry* entry = &record->entries[i];
        if (strcmp(entry->reference, "Breaker1CB1/XSWI1.Pos$stVal") == 0) {
            TEST_ASSERT_TRUE_MESSAGE(entry->ownChangeDetected,
                    "stVal genuinely changed value - ownChangeDetected must be true");
        } else if (strcmp(entry->reference, "Breaker1CB1/XSWI1.Pos$t") == 0
                || strcmp(entry->reference, "Breaker1CB1/XSWI1.Pos$stSeld") == 0
                || strcmp(entry->reference, "Breaker1CB1/XSWI1.Pos$q") == 0) {
            TEST_ASSERT_FALSE_MESSAGE(entry->ownChangeDetected,
                    "this leaf did not itself change - it was only dragged in by its stVal sibling");
        } else {
            TEST_FAIL_MESSAGE("unexpected entry reference");
        }
    }

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(cache.lastForwardedValues[2]);
    MmsValue_delete(cache.lastForwardedValues[3]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_nestedCmvValue_dragsQualitySeveralAncestorLevelsUp(void) {
    /* Real device shape: cVal.mag.f (2 raw dataset positions - "cVal$mag$f"
     * and "q" - authored as separate DA-level FCDA entries, not decomposed
     * from one DO-level entry) nests 3 "$"-segments below the CMV instance
     * ("phsA") that q actually belongs to. Before the ancestor-walk fix, the
     * grouping logic only ever stripped the last "$" segment and never
     * recognized these two as sharing a scope at all. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newFloat(50.0f)); /* cVal.mag.f: differs from cache */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false)); /* q: matches cache */

    ReasonForInclusion reasons[2] = { IEC61850_REASON_DATA_CHANGE, IEC61850_REASON_UNKNOWN };

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newFloat(49.0f), MmsValue_newBoolean(false) };
    char* memberReferences[2] = {
        "LD0/MMXU1$MX$PhV$phsA$cVal$mag$f",
        "LD0/MMXU1$MX$PhV$phsA$q",
    };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.RP.rcbMain", false, "rcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, record->entryCount,
            "q must be dragged along by its changed cVal.mag.f sibling despite being several "
            "\"$\"-segments shallower than the nested measured value's own reference");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_doesNotOverreach_pastAGenuinelyUnrelatedAncestor(void) {
    /* Two independent CMV instances (phsA, phsB) - phsA's value must never
     * accidentally group with phsB's q just because "PhV" is a shared
     * ancestor of both. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(2);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newFloat(50.0f)); /* phsA's cVal.mag.f: differs */
    MmsValue_setElement(dataSetValues, 1, MmsValue_newBoolean(false)); /* phsB's q: matches cache */

    ReasonForInclusion reasons[2] = { IEC61850_REASON_DATA_CHANGE, IEC61850_REASON_UNKNOWN };

    int leafSlotOffsets[2] = { 0, 1 };
    MmsValue* lastForwardedValues[2] = { MmsValue_newFloat(49.0f), MmsValue_newBoolean(false) };
    char* memberReferences[2] = {
        "LD0/MMXU1$MX$PhV$phsA$cVal$mag$f",
        "LD0/MMXU1$MX$PhV$phsB$q",
    };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 2;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 2;
    cache.lastForwardedValues = lastForwardedValues;
    cache.memberReferences = memberReferences;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.RP.rcbMain", false, "rcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 2);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, record->entryCount,
            "phsB's unrelated, unchanged q must NOT be dragged along by phsA's changed value");
    TEST_ASSERT_EQUAL_STRING("LD0/MMXU1$MX$PhV$phsA$cVal$mag$f", record->entries[0].reference);

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(cache.lastForwardedValues[1]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

/* ---- value-diff cache persistence across a simulated reconnect ----
 * The cache is now NEVER reset (the old resetValueDiffCache function and its
 * every-(re-)enable call site are both gone) - it's populated exactly once,
 * on the RCB's first-ever report, and preserved for the rest of the client's
 * lifetime. These tests drive buildReportRecord multiple times in a row with
 * NO reset call anywhere in between (there is no such call left to make) to
 * prove: the first call's snapshot silently seeds the cache and flips
 * everPopulated; a later call simulating a reconnect's own fresh GI snapshot
 * correctly diffs against the REAL preserved prior value - a genuine change
 * forwards with a real (non-NULL) previousValue, an unchanged resend is
 * suppressed exactly like any other duplicate. */

void
test_buildReportRecord_firstReport_seedsCache_andSetsEverPopulated(void) {
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true));

    ReasonForInclusion reasons[1] = { IEC61850_REASON_GI };

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { NULL };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;

    TEST_ASSERT_FALSE(cache.everPopulated);

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "the very first-ever report must never reach the websocket - bootstrap-only");
    TEST_ASSERT_NOT_NULL_MESSAGE(cache.lastForwardedValues[0], "the cache must be seeded from the first report");
    TEST_ASSERT_TRUE_MESSAGE(cache.everPopulated, "everPopulated must flip true after the first report");

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_simulatedReconnect_genuineChangeForwards_withRealPreviousValue(void) {
    /* Simulates a reconnect: the cache already holds a real value from
     * before the "disconnect" (no reset ever runs in between - there is no
     * such call anymore), and the "reconnect"'s own fresh GI snapshot
     * carries a genuinely different value (e.g. the breaker actually opened
     * while the client was disconnected). This must forward, WITH a real
     * (non-NULL) previousValue reflecting the true pre-disconnect state -
     * not NULL, which is exactly the bug this design fixes. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(false)); /* changed while "disconnected" */

    ReasonForInclusion reasons[1] = { IEC61850_REASON_GI };

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(true) }; /* real pre-disconnect value, still cached */
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;
    cache.everPopulated = true; /* this RCB already completed its first-ever report before */

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, record->entryCount,
            "a genuine change discovered on reconnect must forward, diffed against the preserved cache");
    TEST_ASSERT_FALSE(MmsValue_getBoolean(record->entries[0].value));
    TEST_ASSERT_NOT_NULL_MESSAGE(record->entries[0].previousValue,
            "previousValue must be the REAL preserved pre-disconnect value, never NULL, after a reconnect");
    TEST_ASSERT_TRUE_MESSAGE(MmsValue_getBoolean(record->entries[0].previousValue),
            "previousValue must reflect the true value from before the disconnect");

    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

void
test_buildReportRecord_simulatedReconnect_unchangedResend_isSuppressed(void) {
    /* Same simulated-reconnect setup, but this time the reconnect's own
     * fresh GI snapshot carries the SAME value as before the disconnect -
     * must be suppressed by the ordinary diff check, exactly like any other
     * unchanged resend, with no special-cased "reconnect" behavior needed. */
    MmsValue* dataSetValues = MmsValue_createEmptyArray(1);
    MmsValue_setElement(dataSetValues, 0, MmsValue_newBoolean(true)); /* unchanged across the "disconnect" */

    ReasonForInclusion reasons[1] = { IEC61850_REASON_GI };

    int leafSlotOffsets[1] = { 0 };
    MmsValue* lastForwardedValues[1] = { MmsValue_newBoolean(true) };
    MmsReportClientMemberRefCacheEntry cache = { 0 };
    cache.memberCount = 1;
    cache.leafSlotOffsets = leafSlotOffsets;
    cache.totalLeafSlots = 1;
    cache.lastForwardedValues = lastForwardedValues;
    cache.everPopulated = true;

    MmsReportRecord* record = MmsReportClientUseCases_buildReportRecord(
            "Breaker1CB1/LLN0.BR.brcbMain", true, "brcbMain",
            false, NULL, false, 0, false, 0,
            dataSetValues, reasons, NULL, &cache, 1);

    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, record->entryCount,
            "an unchanged resend after a reconnect must be suppressed, same as any other duplicate");
    TEST_ASSERT_NOT_NULL(cache.lastForwardedValues[0]);
    TEST_ASSERT_TRUE(MmsValue_getBoolean(cache.lastForwardedValues[0]));

    MmsValue_delete(cache.lastForwardedValues[0]);
    MmsValue_delete(dataSetValues);
    MmsReportClientUseCases_freeReportRecord(record);
}

/* ---- shouldForwardAcrossRcb (cross-RCB duplicate-content suppression) ---- */

static MmsReportEntry*
makeDedupEntries(const char* ref0, bool val0, const char* ref1, bool val1) {
    MmsReportEntry* entries = calloc(2, sizeof(MmsReportEntry));
    entries[0].reference = strdup(ref0);
    entries[0].value = MmsValue_newBoolean(val0);
    entries[1].reference = strdup(ref1);
    entries[1].value = MmsValue_newBoolean(val1);
    return entries;
}

static void
freeDedupEntries(MmsReportEntry* entries, int count) {
    for (int i = 0; i < count; i++) {
        free(entries[i].reference);
        if (entries[i].value) MmsValue_delete(entries[i].value);
    }
    free(entries);
}

void
test_shouldForwardAcrossRcb_firstEverContent_isForwarded_andSeedsCache(void) {
    MmsReportClientCrossRcbDedupCache cache = { 0 };
    MmsReportEntry* entries = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);

    bool result = MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbA01", entries, 2);

    TEST_ASSERT_TRUE_MESSAGE(result, "nothing cached yet - must always forward");
    TEST_ASSERT_EQUAL_STRING("LD/GGIO1.RP.urcbA01", cache.rcbReference);
    TEST_ASSERT_EQUAL_INT(2, cache.entryCount);

    freeDedupEntries(entries, 2);
    MmsReportClientUseCases_destroyCrossRcbDedupCache(&cache);
}

void
test_shouldForwardAcrossRcb_sameRcbIdenticalContent_isStillForwarded(void) {
    /* A repeat from the SAME RCB is this stage's non-concern - the per-RCB
     * hybrid filter already decided to forward it (e.g. a trusted
     * DATA_CHANGE reason even though the value happened to match), so this
     * stage must not second-guess that decision. */
    MmsReportClientCrossRcbDedupCache cache = { 0 };
    MmsReportEntry* entries1 = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    TEST_ASSERT_TRUE(MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbA01", entries1, 2));

    MmsReportEntry* entries2 = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    bool result = MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbA01", entries2, 2);

    TEST_ASSERT_TRUE(result);

    freeDedupEntries(entries1, 2);
    freeDedupEntries(entries2, 2);
    MmsReportClientUseCases_destroyCrossRcbDedupCache(&cache);
}

void
test_shouldForwardAcrossRcb_differentRcbIdenticalContent_isSuppressed(void) {
    MmsReportClientCrossRcbDedupCache cache = { 0 };
    MmsReportEntry* entries1 = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    TEST_ASSERT_TRUE(MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbA01", entries1, 2));

    MmsReportEntry* entries2 = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    bool result = MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbB01", entries2, 2);

    TEST_ASSERT_FALSE_MESSAGE(result,
            "a different RCB reporting byte-identical content must be suppressed as a duplicate");

    freeDedupEntries(entries1, 2);
    freeDedupEntries(entries2, 2);
    MmsReportClientUseCases_destroyCrossRcbDedupCache(&cache);
}

void
test_shouldForwardAcrossRcb_differentRcbDifferentContent_isForwarded(void) {
    MmsReportClientCrossRcbDedupCache cache = { 0 };
    MmsReportEntry* entries1 = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    TEST_ASSERT_TRUE(MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbA01", entries1, 2));

    MmsReportEntry* entries2 = makeDedupEntries("LD/GGIO1$ST$SPCSO2$stVal", true, "LD/GGIO1$ST$SPCSO2$t", 1000);
    bool result = MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbB01", entries2, 2);

    TEST_ASSERT_TRUE_MESSAGE(result, "genuinely different content from a different RCB must be forwarded");
    TEST_ASSERT_EQUAL_STRING("LD/GGIO1.RP.urcbB01", cache.rcbReference);

    freeDedupEntries(entries1, 2);
    freeDedupEntries(entries2, 2);
    MmsReportClientUseCases_destroyCrossRcbDedupCache(&cache);
}

void
test_shouldForwardAcrossRcb_suppressionDoesNotDisturbEstablishedBaseline(void) {
    /* Three redundant RCBs (A, B, C) all reporting the same content: only A
     * forwards; B and C are both suppressed against A's original content -
     * suppression must not overwrite the cache, or a later repeat could
     * wrongly compare against a suppressed entry's own (never-cached) RCB. */
    MmsReportClientCrossRcbDedupCache cache = { 0 };
    MmsReportEntry* entriesA = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    TEST_ASSERT_TRUE(MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbA01", entriesA, 2));

    MmsReportEntry* entriesB = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    TEST_ASSERT_FALSE(MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbB01", entriesB, 2));

    MmsReportEntry* entriesC = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    bool result = MmsReportClientUseCases_shouldForwardAcrossRcb(&cache, "LD/GGIO1.RP.urcbC01", entriesC, 2);

    TEST_ASSERT_FALSE_MESSAGE(result, "C must still be recognized as a duplicate of A's original content, "
            "even though B's suppressed report never touched the cache");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("LD/GGIO1.RP.urcbA01", cache.rcbReference,
            "the cache must still reflect A, the only one actually forwarded");

    freeDedupEntries(entriesA, 2);
    freeDedupEntries(entriesB, 2);
    freeDedupEntries(entriesC, 2);
    MmsReportClientUseCases_destroyCrossRcbDedupCache(&cache);
}

void
test_shouldForwardAcrossRcb_isNoOp_whenCacheIsNull(void) {
    MmsReportEntry* entries = makeDedupEntries("LD/GGIO1$ST$SPCSO1$stVal", true, "LD/GGIO1$ST$SPCSO1$t", 1000);
    TEST_ASSERT_TRUE(MmsReportClientUseCases_shouldForwardAcrossRcb(NULL, "LD/GGIO1.RP.urcbA01", entries, 2));
    freeDedupEntries(entries, 2);
}

void
test_destroyCrossRcbDedupCache_doesNotCrash_onNull(void) {
    MmsReportClientUseCases_destroyCrossRcbDedupCache(NULL);
}

/* ---- buildWireMemberReferences (dynamic-dataset wire-format conversion) ---- */

void
test_buildWireMemberReferences_convertsDollarJoinedToDotBracketForm(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Mod$stVal", "IED1LD1/LLN0$MX$TotW$mag" };
    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(refs, 2);

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
    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(refs, 1);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(wireRefs));
    TEST_ASSERT_EQUAL_STRING("IED1LD1/LLN0.PhV.cVal.mag[MX]",
            (const char*) LinkedList_getData(LinkedList_getNext(wireRefs)));

    LinkedList_destroyDeep(wireRefs, free);
}

void
test_buildWireMemberReferences_skipsMalformedReference_tooFewSegments(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST", "IED1LD1/LLN0$ST$Mod$stVal" };
    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(refs, 2);

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
    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(refs, 2);

    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(wireRefs));

    LinkedList_destroyDeep(wireRefs, free);
}

void
test_buildWireMemberReferences_empty_whenCountIsZeroOrNegative(void) {
    const char* refs[] = { "IED1LD1/LLN0$ST$Mod$stVal" };

    LinkedList wireRefsZero = MmsReportClientUseCases_buildWireMemberReferences(refs, 0);
    TEST_ASSERT_NOT_NULL(wireRefsZero);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(wireRefsZero));
    LinkedList_destroyDeep(wireRefsZero, free);

    LinkedList wireRefsNeg = MmsReportClientUseCases_buildWireMemberReferences(refs, -1);
    TEST_ASSERT_NOT_NULL(wireRefsNeg);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(wireRefsNeg));
    LinkedList_destroyDeep(wireRefsNeg, free);
}

void
test_buildWireMemberReferences_empty_whenArrayIsNull(void) {
    LinkedList wireRefs = MmsReportClientUseCases_buildWireMemberReferences(NULL, 3);

    TEST_ASSERT_NOT_NULL(wireRefs);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(wireRefs));

    LinkedList_destroyDeep(wireRefs, free);
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

    RUN_TEST(test_isDuplicateValue_utcTime_sameMsDifferentQualityByte_isDuplicate);
    RUN_TEST(test_isDuplicateValue_utcTime_genuinelyDifferentMs_isNotDuplicate);
    RUN_TEST(test_isDuplicateValue_bitString_sameSizeSameBits_isDuplicate);
    RUN_TEST(test_isDuplicateValue_bitString_genuinelyDifferentBits_isNotDuplicate);
    RUN_TEST(test_isDuplicateValue_bitString_sameDecodedIntegerDifferentSize_isNotDuplicate);
    RUN_TEST(test_isDuplicateValue_typeMismatch_isNotDuplicate);
    RUN_TEST(test_isDuplicateValue_booleanUnchanged_isDuplicate);

    RUN_TEST(test_isEntryIdStale_incomingGreaterThanLastSeen_isNotStale);
    RUN_TEST(test_isEntryIdStale_incomingEqualsLastSeen_isStale);
    RUN_TEST(test_isEntryIdStale_incomingLessThanLastSeen_isStale);
    RUN_TEST(test_isEntryIdStale_incomingNull_isNotStale_failsOpen);
    RUN_TEST(test_isEntryIdStale_lastSeenNull_isNotStale_failsOpen);
    RUN_TEST(test_isEntryIdStale_mismatchedByteSizes_isNotStale_failsOpen);
    RUN_TEST(test_isEntryIdStale_nonOctetStringType_isNotStale_failsOpen);
    RUN_TEST(test_isEntryIdStale_multiByteBigEndianOrdering_isCorrect);

    RUN_TEST(test_shouldRequestGiOnEnable_unbufferedWithNoResumableEntryId_requestsGi);
    RUN_TEST(test_shouldRequestGiOnEnable_unbufferedWithResumableEntryId_stillRequestsGi);
    RUN_TEST(test_shouldRequestGiOnEnable_bufferedWithNoResumableEntryId_requestsGi);
    RUN_TEST(test_shouldRequestGiOnEnable_bufferedWithResumableEntryId_skipsGi);

    RUN_TEST(test_buildReportRecord_copiesScalarFields);
    RUN_TEST(test_buildReportRecord_deepCopiesEntries_notAliased);
    RUN_TEST(test_buildReportRecord_prefersServerDataReference_overFallback);
    RUN_TEST(test_buildReportRecord_usesFallbackReference_whenServerDataReferenceMissing);
    RUN_TEST(test_buildReportRecord_fallbackOutOfRange_leavesReferenceNull);
    RUN_TEST(test_buildReportRecord_copiesEntryId_whenPresent);
    RUN_TEST(test_freeReportRecord_doesNotCrash_onNull);
    RUN_TEST(test_buildReportRecord_giReason_firstEverValue_isSuppressed_andSeedsCache);
    RUN_TEST(test_buildReportRecord_integrityReason_unchangedValue_isDroppedAfterSeed);
    RUN_TEST(test_buildReportRecord_integrityReason_changedValue_isForwarded);
    RUN_TEST(test_buildReportRecord_dataChangeReason_sameValueAsCache_isDropped);
    RUN_TEST(test_buildReportRecord_realChangeReason_previousValueEqualsPriorCache);
    RUN_TEST(test_buildReportRecord_firstEverRealChange_noPriorBootstrap_previousValueIsNull);
    RUN_TEST(test_buildReportRecord_unknownReason_threeCallSequence_matchesNoReasonCodeDevice);
    RUN_TEST(test_buildReportRecord_decomposesStructuredEntry_intoFlatLeaves);
    RUN_TEST(test_buildReportRecord_decomposition_countMismatch_fallsBackToRawEntry);
    RUN_TEST(test_buildReportRecord_decomposition_withWireTypesPresent_stillDecomposesWhenTypesMatch);
    RUN_TEST(test_buildReportRecord_decomposition_stValAndStSeld_reorderedByTypeNotPosition);
    RUN_TEST(test_buildReportRecord_decomposedGiEntry_isSuppressed_seedsCache_thenDuplicateDropped);

    RUN_TEST(test_buildReportRecord_valueForwarded_dragsUnchangedQualitySibling);
    RUN_TEST(test_buildReportRecord_qualityForwarded_dragsUnchangedValueSibling);
    RUN_TEST(test_buildReportRecord_draggedAlongSibling_previousValueEqualsOwnCurrentValue);
    RUN_TEST(test_buildReportRecord_bothSiblingsUnchanged_neitherForwarded);
    RUN_TEST(test_buildReportRecord_ungroupableEntry_fallsBackToSoloDiffCheck);
    RUN_TEST(test_buildReportRecord_decomposedGroup_changedLeafDragsUnchangedSiblingLeaf);
    RUN_TEST(test_buildReportRecord_decomposedGroup_fourLeafDpc_onlyChangedLeafHasOwnChangeDetected);
    RUN_TEST(test_buildReportRecord_nestedCmvValue_dragsQualitySeveralAncestorLevelsUp);
    RUN_TEST(test_buildReportRecord_doesNotOverreach_pastAGenuinelyUnrelatedAncestor);

    RUN_TEST(test_buildReportRecord_firstReport_seedsCache_andSetsEverPopulated);
    RUN_TEST(test_buildReportRecord_simulatedReconnect_genuineChangeForwards_withRealPreviousValue);
    RUN_TEST(test_buildReportRecord_simulatedReconnect_unchangedResend_isSuppressed);

    RUN_TEST(test_shouldForwardAcrossRcb_firstEverContent_isForwarded_andSeedsCache);
    RUN_TEST(test_shouldForwardAcrossRcb_sameRcbIdenticalContent_isStillForwarded);
    RUN_TEST(test_shouldForwardAcrossRcb_differentRcbIdenticalContent_isSuppressed);
    RUN_TEST(test_shouldForwardAcrossRcb_differentRcbDifferentContent_isForwarded);
    RUN_TEST(test_shouldForwardAcrossRcb_suppressionDoesNotDisturbEstablishedBaseline);
    RUN_TEST(test_shouldForwardAcrossRcb_isNoOp_whenCacheIsNull);
    RUN_TEST(test_destroyCrossRcbDedupCache_doesNotCrash_onNull);

    RUN_TEST(test_buildWireMemberReferences_convertsDollarJoinedToDotBracketForm);
    RUN_TEST(test_buildWireMemberReferences_joinsNestedSegmentsWithDots);
    RUN_TEST(test_buildWireMemberReferences_skipsMalformedReference_tooFewSegments);
    RUN_TEST(test_buildWireMemberReferences_skipsNullEntry);
    RUN_TEST(test_buildWireMemberReferences_empty_whenCountIsZeroOrNegative);
    RUN_TEST(test_buildWireMemberReferences_empty_whenArrayIsNull);

    RUN_TEST(test_computeNextBackoffDelay_returnsInitial_whenCurrentIsZero);
    RUN_TEST(test_computeNextBackoffDelay_doublesUntilCap);
    RUN_TEST(test_computeNextBackoffDelay_doesNotOverflow_whenCurrentNearUint32Max);

    return UNITY_END();
}
