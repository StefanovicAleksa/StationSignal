#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_ring_buffer.h"

static IpcDispatcherRingBuffer fixtureBuffer;

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
        IpcDispatcherRingBuffer_destroy(fixtureBuffer);
        fixtureBuffer = NULL;
    }
}

void
test_create_returnsNull_onInvalidCapacity(void) {
    TEST_ASSERT_NULL(IpcDispatcherRingBuffer_create(0));
    TEST_ASSERT_NULL(IpcDispatcherRingBuffer_create(-1));
}

void
test_headSeq_startsAtZero(void) {
    fixtureBuffer = IpcDispatcherRingBuffer_create(4);
    TEST_ASSERT_EQUAL_UINT64(0, IpcDispatcherRingBuffer_headSeq(fixtureBuffer));
}

void
test_readNext_returnsNull_whenCursorAtHead(void) {
    fixtureBuffer = IpcDispatcherRingBuffer_create(4);
    uint64_t cursor = IpcDispatcherRingBuffer_headSeq(fixtureBuffer);

    char* result = IpcDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, NULL);

    TEST_ASSERT_NULL(result);
}

void
test_push_thenReadNext_returnsCopy_notAlias(void) {
    fixtureBuffer = IpcDispatcherRingBuffer_create(4);
    uint64_t cursor = IpcDispatcherRingBuffer_headSeq(fixtureBuffer);

    IpcDispatcherRingBuffer_push(fixtureBuffer, dupStr("{\"a\":1}"));

    uint64_t dropped = 999;
    char* result = IpcDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, &dropped);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", result);
    TEST_ASSERT_EQUAL_UINT64(0, dropped);
    TEST_ASSERT_EQUAL_UINT64(1, cursor);

    free(result);

    /* Nothing further available */
    TEST_ASSERT_NULL(IpcDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, NULL));
}

void
test_wraparound_dropsOldest(void) {
    fixtureBuffer = IpcDispatcherRingBuffer_create(2);

    IpcDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg0"));
    IpcDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg1"));
    IpcDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg2")); /* overwrites msg0's slot */

    uint64_t cursor = 0; /* reader that never saw anything, starting from the very beginning */
    uint64_t dropped = 0;
    char* result = IpcDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, &dropped);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_UINT64(1, dropped); /* msg0 was skipped */
    TEST_ASSERT_EQUAL_STRING("msg1", result); /* oldest still available */

    free(result);

    char* second = IpcDispatcherRingBuffer_readNext(fixtureBuffer, &cursor, &dropped);
    TEST_ASSERT_EQUAL_STRING("msg2", second);
    free(second);
}

void
test_twoIndependentCursors_laggingOneDoesNotAffectCaughtUpOne(void) {
    fixtureBuffer = IpcDispatcherRingBuffer_create(2);

    uint64_t caughtUpCursor = IpcDispatcherRingBuffer_headSeq(fixtureBuffer);
    uint64_t laggingCursor = caughtUpCursor;

    IpcDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg0"));

    /* caughtUp reads immediately */
    char* r1 = IpcDispatcherRingBuffer_readNext(fixtureBuffer, &caughtUpCursor, NULL);
    TEST_ASSERT_EQUAL_STRING("msg0", r1);
    free(r1);

    /* more pushes happen before lagging reader catches up, past capacity */
    IpcDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg1"));
    IpcDispatcherRingBuffer_push(fixtureBuffer, dupStr("msg2"));

    /* caughtUp reader sees the rest in order, no drops (never fell behind) */
    uint64_t droppedForCaughtUp = 999;
    char* r2 = IpcDispatcherRingBuffer_readNext(fixtureBuffer, &caughtUpCursor, &droppedForCaughtUp);
    TEST_ASSERT_EQUAL_STRING("msg1", r2);
    TEST_ASSERT_EQUAL_UINT64(0, droppedForCaughtUp);
    free(r2);

    /* lagging reader, still at the very start, has lost msg0 (overwritten) */
    uint64_t droppedForLagging = 0;
    char* r3 = IpcDispatcherRingBuffer_readNext(fixtureBuffer, &laggingCursor, &droppedForLagging);
    TEST_ASSERT_EQUAL_UINT64(1, droppedForLagging);
    TEST_ASSERT_EQUAL_STRING("msg1", r3);
    free(r3);
}

void
test_push_takesOwnership_evenOnNullBuffer(void) {
    /* Must not crash or leak-detect as a use-after-free; just documents the
     * "always takes ownership, even on a NULL buffer" contract. */
    IpcDispatcherRingBuffer_push(NULL, dupStr("discarded"));
}

void
test_destroy_doesNotCrash_onNull(void) {
    IpcDispatcherRingBuffer_destroy(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_returnsNull_onInvalidCapacity);
    RUN_TEST(test_headSeq_startsAtZero);
    RUN_TEST(test_readNext_returnsNull_whenCursorAtHead);
    RUN_TEST(test_push_thenReadNext_returnsCopy_notAlias);
    RUN_TEST(test_wraparound_dropsOldest);
    RUN_TEST(test_twoIndependentCursors_laggingOneDoesNotAffectCaughtUpOne);
    RUN_TEST(test_push_takesOwnership_evenOnNullBuffer);
    RUN_TEST(test_destroy_doesNotCrash_onNull);

    return UNITY_END();
}
