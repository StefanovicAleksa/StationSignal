#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/scan_dispatcher/data/scan_dispatcher_ring_buffer.h"

/*
 * Duplicate of test_ipc_dispatcher_ring_buffer.c against the duplicated
 * ring buffer implementation - see scan_dispatcher_types.h's own top comment
 * for why the ring buffer itself is duplicated rather than shared; the test
 * is duplicated for the same reason.
 */

static ScanDispatcherRingBuffer fixtureBuffer;

static char*
dupStr(const char* s) {
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    memcpy(copy, s, len);
    return copy;
}

void
setUp(void) {
    fixtureBuffer = NULL;
}

void
tearDown(void) {
    if (fixtureBuffer) {
        ScanDispatcherRingBuffer_destroy(fixtureBuffer);
        fixtureBuffer = NULL;
    }
}

void
test_create_returnsNull_onInvalidCapacity(void) {
    TEST_ASSERT_NULL(ScanDispatcherRingBuffer_create(0));
    TEST_ASSERT_NULL(ScanDispatcherRingBuffer_create(-1));
}

void
test_headSeq_startsAtZero(void) {
    fixtureBuffer = ScanDispatcherRingBuffer_create(4);
    TEST_ASSERT_EQUAL_UINT64(0, ScanDispatcherRingBuffer_headSeq(fixtureBuffer));
}

void
test_readNext_returnsNull_whenCursorAtHead(void) {
    fixtureBuffer = ScanDispatcherRingBuffer_create(4);
    uint64_t cursor = ScanDispatcherRingBuffer_headSeq(fixtureBuffer);

    char* result = ScanDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, NULL);

    TEST_ASSERT_NULL(result);
}

void
test_push_thenReadNext_returnsCopy_notAlias(void) {
    fixtureBuffer = ScanDispatcherRingBuffer_create(4);
    uint64_t cursor = ScanDispatcherRingBuffer_headSeq(fixtureBuffer);

    ScanDispatcherRingBuffer_push(fixtureBuffer, dupStr("{\"a\":1}"));

    uint64_t dropped = 999;
    char* result = ScanDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, &dropped);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", result);
    TEST_ASSERT_EQUAL_UINT64(0, dropped);
    TEST_ASSERT_EQUAL_UINT64(1, cursor);

    free(result);

    /* Nothing further available */
    TEST_ASSERT_NULL(ScanDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, NULL));
}

void
test_wraparound_dropsOldest(void) {
    fixtureBuffer = ScanDispatcherRingBuffer_create(2);

    ScanDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg0"));
    ScanDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg1"));
    ScanDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg2")); /* overwrites msg0's slot */

    uint64_t cursor = 0; /* reader that never saw anything, starting from the very beginning */
    uint64_t dropped = 0;
    char* result = ScanDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, &dropped);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_UINT64(1, dropped); /* msg0 was skipped */
    TEST_ASSERT_EQUAL_STRING("msg1", result); /* oldest still available */

    free(result);

    char* second = ScanDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, &dropped);
    TEST_ASSERT_EQUAL_STRING("msg2", second);
    free(second);
}

void
test_push_takesOwnership_evenOnNullBuffer(void) {
    /* Must not crash or leak-detect as a use-after-free; just documents the
     * "always takes ownership, even on a NULL buffer" contract. */
    ScanDispatcherRingBuffer_push(NULL, dupStr("discarded"));
}

void
test_destroy_doesNotCrash_onNull(void) {
    ScanDispatcherRingBuffer_destroy(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_returnsNull_onInvalidCapacity);
    RUN_TEST(test_headSeq_startsAtZero);
    RUN_TEST(test_readNext_returnsNull_whenCursorAtHead);
    RUN_TEST(test_push_thenReadNext_returnsCopy_notAlias);
    RUN_TEST(test_wraparound_dropsOldest);
    RUN_TEST(test_push_takesOwnership_evenOnNullBuffer);
    RUN_TEST(test_destroy_doesNotCrash_onNull);

    return UNITY_END();
}
