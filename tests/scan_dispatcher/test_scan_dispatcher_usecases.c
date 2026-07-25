#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/scan_dispatcher/domain/scan_dispatcher_usecases.h"

static ScanDeviceFoundEvent* fixtureEvent;

void
setUp(void) {
    fixtureEvent = NULL;
}

void
tearDown(void) {
    if (fixtureEvent) {
        ScanDispatcherUseCases_freeEvent(fixtureEvent);
        fixtureEvent = NULL;
    }
}

void
test_assembleEvent_copiesFieldsAndDeepCopiesHost(void) {
    char hostBuf[] = "192.168.1.50";

    fixtureEvent = ScanDispatcherUseCases_assembleEvent(42, hostBuf, 102, 1700000000000ULL, false);

    TEST_ASSERT_NOT_NULL(fixtureEvent);
    TEST_ASSERT_EQUAL_UINT64(42, fixtureEvent->scanId);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", fixtureEvent->host);
    TEST_ASSERT_TRUE(fixtureEvent->host != hostBuf); /* deep copy, not aliased */
    TEST_ASSERT_EQUAL_INT(102, fixtureEvent->mmsPort);
    TEST_ASSERT_EQUAL_UINT64(1700000000000ULL, fixtureEvent->discoveredAtMs);
    TEST_ASSERT_FALSE(fixtureEvent->authRequired);
}

void
test_assembleEvent_copiesAuthRequiredTrue(void) {
    fixtureEvent = ScanDispatcherUseCases_assembleEvent(42, "192.168.1.50", 102, 1700000000000ULL, true);

    TEST_ASSERT_NOT_NULL(fixtureEvent);
    TEST_ASSERT_TRUE(fixtureEvent->authRequired);
}

void
test_assembleEvent_returnsNull_onNullHost(void) {
    TEST_ASSERT_NULL(ScanDispatcherUseCases_assembleEvent(1, NULL, 102, 0, false));
}

void
test_assembleEvent_returnsNull_onEmptyHost(void) {
    TEST_ASSERT_NULL(ScanDispatcherUseCases_assembleEvent(1, "", 102, 0, false));
}

void
test_freeEvent_doesNotCrash_onNull(void) {
    ScanDispatcherUseCases_freeEvent(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_assembleEvent_copiesFieldsAndDeepCopiesHost);
    RUN_TEST(test_assembleEvent_copiesAuthRequiredTrue);
    RUN_TEST(test_assembleEvent_returnsNull_onNullHost);
    RUN_TEST(test_assembleEvent_returnsNull_onEmptyHost);
    RUN_TEST(test_freeEvent_doesNotCrash_onNull);

    return UNITY_END();
}
