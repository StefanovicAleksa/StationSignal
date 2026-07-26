#include <stdlib.h>
#include "unity.h"
#include "device_manager/data/device_manager_port_allocator.h"

static DeviceManagerPortAllocator fixtureAllocator;

void
setUp(void) {
    fixtureAllocator = NULL;
}

void
tearDown(void) {
    if (fixtureAllocator) {
        DeviceManagerPortAllocator_destroy(fixtureAllocator);
        fixtureAllocator = NULL;
    }
}

void
test_create_rejectsInvertedRange(void) {
    TEST_ASSERT_NULL(DeviceManagerPortAllocator_create(9010, 9000));
}

void
test_create_acceptsSinglePortRange(void) {
    fixtureAllocator = DeviceManagerPortAllocator_create(9000, 9000);
    TEST_ASSERT_NOT_NULL(fixtureAllocator);
}

void
test_alloc_issuesSequentially(void) {
    fixtureAllocator = DeviceManagerPortAllocator_create(9000, 9002);
    TEST_ASSERT_NOT_NULL(fixtureAllocator);

    uint16_t port;
    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));
    TEST_ASSERT_EQUAL_UINT16(9000, port);
    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));
    TEST_ASSERT_EQUAL_UINT16(9001, port);
    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));
    TEST_ASSERT_EQUAL_UINT16(9002, port);
}

void
test_alloc_exhaustion_returnsFalse(void) {
    fixtureAllocator = DeviceManagerPortAllocator_create(9000, 9001);
    TEST_ASSERT_NOT_NULL(fixtureAllocator);

    uint16_t port;
    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));
    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));
    TEST_ASSERT_FALSE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));
}

void
test_free_thenAlloc_reusesFreedPort_beforeGrowingRange(void) {
    fixtureAllocator = DeviceManagerPortAllocator_create(9000, 9002);
    TEST_ASSERT_NOT_NULL(fixtureAllocator);

    uint16_t first, second, third, reused;
    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &first));   /* 9000 */
    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &second));  /* 9001 */

    DeviceManagerPortAllocator_free(fixtureAllocator, first);

    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &reused));
    TEST_ASSERT_EQUAL_UINT16(first, reused); /* freed port reused before issuing 9002 */

    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &third));
    TEST_ASSERT_EQUAL_UINT16(9002, third);
}

void
test_alloc_exhaustionThenFree_recovers(void) {
    fixtureAllocator = DeviceManagerPortAllocator_create(9000, 9000);
    TEST_ASSERT_NOT_NULL(fixtureAllocator);

    uint16_t port;
    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));
    TEST_ASSERT_FALSE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));

    DeviceManagerPortAllocator_free(fixtureAllocator, 9000);

    TEST_ASSERT_TRUE(DeviceManagerPortAllocator_alloc(fixtureAllocator, &port));
    TEST_ASSERT_EQUAL_UINT16(9000, port);
}

void
test_nullSafety_doesNotCrash(void) {
    uint16_t port;
    TEST_ASSERT_FALSE(DeviceManagerPortAllocator_alloc(NULL, &port));
    DeviceManagerPortAllocator_free(NULL, 9000);
    DeviceManagerPortAllocator_destroy(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_rejectsInvertedRange);
    RUN_TEST(test_create_acceptsSinglePortRange);

    RUN_TEST(test_alloc_issuesSequentially);
    RUN_TEST(test_alloc_exhaustion_returnsFalse);
    RUN_TEST(test_free_thenAlloc_reusesFreedPort_beforeGrowingRange);
    RUN_TEST(test_alloc_exhaustionThenFree_recovers);

    RUN_TEST(test_nullSafety_doesNotCrash);

    return UNITY_END();
}
