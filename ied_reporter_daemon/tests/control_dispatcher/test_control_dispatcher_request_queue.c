#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/control_dispatcher/data/control_dispatcher_request_queue.h"

static ControlDispatcherRequestQueue fixtureQueue;

void
setUp(void) {
    fixtureQueue = NULL;
}

void
tearDown(void) {
    if (fixtureQueue) {
        ControlDispatcherRequestQueue_destroy(fixtureQueue);
        fixtureQueue = NULL;
    }
}

static ControlRequest*
makeStopRequest(const char* requestId, uint64_t deviceId) {
    ControlRequest* request = calloc(1, sizeof(ControlRequest));
    request->requestId = strdup(requestId);
    request->type = CONTROL_REQ_STOP_REPORTING;
    request->deviceId = deviceId;
    return request;
}

void
test_create_returnsNull_onInvalidCapacity(void) {
    TEST_ASSERT_NULL(ControlDispatcherRequestQueue_create(0));
    TEST_ASSERT_NULL(ControlDispatcherRequestQueue_create(-1));
}

void
test_pop_returnsNull_whenEmpty(void) {
    fixtureQueue = ControlDispatcherRequestQueue_create(4);
    TEST_ASSERT_NULL(ControlDispatcherRequestQueue_pop(fixtureQueue));
}

void
test_push_thenPop_returnsSameRequest_fifoOrder(void) {
    fixtureQueue = ControlDispatcherRequestQueue_create(4);

    ControlRequest* first = makeStopRequest("req-1", 1);
    ControlRequest* second = makeStopRequest("req-2", 2);

    TEST_ASSERT_TRUE(ControlDispatcherRequestQueue_push(fixtureQueue, first));
    TEST_ASSERT_TRUE(ControlDispatcherRequestQueue_push(fixtureQueue, second));

    ControlRequest* poppedFirst = ControlDispatcherRequestQueue_pop(fixtureQueue);
    TEST_ASSERT_EQUAL_PTR(first, poppedFirst);
    TEST_ASSERT_EQUAL_STRING("req-1", poppedFirst->requestId);
    ControlDispatcherRequest_destroy(poppedFirst);

    ControlRequest* poppedSecond = ControlDispatcherRequestQueue_pop(fixtureQueue);
    TEST_ASSERT_EQUAL_PTR(second, poppedSecond);
    ControlDispatcherRequest_destroy(poppedSecond);

    TEST_ASSERT_NULL(ControlDispatcherRequestQueue_pop(fixtureQueue));
}

void
test_push_returnsFalse_whenFull_callerStillOwnsRequest(void) {
    fixtureQueue = ControlDispatcherRequestQueue_create(1);

    ControlRequest* first = makeStopRequest("req-1", 1);
    ControlRequest* second = makeStopRequest("req-2", 2);

    TEST_ASSERT_TRUE(ControlDispatcherRequestQueue_push(fixtureQueue, first));
    TEST_ASSERT_FALSE(ControlDispatcherRequestQueue_push(fixtureQueue, second));

    /* Queue never took ownership of `second` - caller frees it. */
    ControlDispatcherRequest_destroy(second);

    ControlRequest* popped = ControlDispatcherRequestQueue_pop(fixtureQueue);
    ControlDispatcherRequest_destroy(popped);
}

void
test_wraparound_afterPopThenPush(void) {
    fixtureQueue = ControlDispatcherRequestQueue_create(2);

    TEST_ASSERT_TRUE(ControlDispatcherRequestQueue_push(fixtureQueue, makeStopRequest("a", 1)));
    TEST_ASSERT_TRUE(ControlDispatcherRequestQueue_push(fixtureQueue, makeStopRequest("b", 2)));

    ControlRequest* poppedA = ControlDispatcherRequestQueue_pop(fixtureQueue);
    ControlDispatcherRequest_destroy(poppedA);

    TEST_ASSERT_TRUE(ControlDispatcherRequestQueue_push(fixtureQueue, makeStopRequest("c", 3)));

    ControlRequest* poppedB = ControlDispatcherRequestQueue_pop(fixtureQueue);
    TEST_ASSERT_EQUAL_STRING("b", poppedB->requestId);
    ControlDispatcherRequest_destroy(poppedB);

    ControlRequest* poppedC = ControlDispatcherRequestQueue_pop(fixtureQueue);
    TEST_ASSERT_EQUAL_STRING("c", poppedC->requestId);
    ControlDispatcherRequest_destroy(poppedC);
}

void
test_destroy_freesStillQueuedRequests(void) {
    fixtureQueue = ControlDispatcherRequestQueue_create(4);

    TEST_ASSERT_TRUE(ControlDispatcherRequestQueue_push(fixtureQueue, makeStopRequest("a", 1)));
    TEST_ASSERT_TRUE(ControlDispatcherRequestQueue_push(fixtureQueue, makeStopRequest("b", 2)));

    /* Never popped - destroy must free them itself (verified only by the
     * absence of a leak-sanitizer complaint when this test is run under one,
     * not asserted directly here). */
    ControlDispatcherRequestQueue_destroy(fixtureQueue);
    fixtureQueue = NULL;
}

void
test_nullSafety_doesNotCrash(void) {
    TEST_ASSERT_FALSE(ControlDispatcherRequestQueue_push(NULL, NULL));
    TEST_ASSERT_NULL(ControlDispatcherRequestQueue_pop(NULL));
    ControlDispatcherRequestQueue_destroy(NULL);
    ControlDispatcherRequest_destroy(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_returnsNull_onInvalidCapacity);
    RUN_TEST(test_pop_returnsNull_whenEmpty);
    RUN_TEST(test_push_thenPop_returnsSameRequest_fifoOrder);
    RUN_TEST(test_push_returnsFalse_whenFull_callerStillOwnsRequest);
    RUN_TEST(test_wraparound_afterPopThenPush);
    RUN_TEST(test_destroy_freesStillQueuedRequests);
    RUN_TEST(test_nullSafety_doesNotCrash);

    return UNITY_END();
}
