#include <stdlib.h>
#include "unity.h"
#include "device_manager/data/device_manager_registry.h"

/* Registry never dereferences the OrchestrationHandle it stores/hands back -
 * only stores/compares the pointer - so a fake, never-dereferenced pointer
 * value is fine for exercising finalize/removeIfRunning's own plumbing. */
#define FAKE_HANDLE_1 ((OrchestrationHandle) (void*) 0x1)
#define FAKE_HANDLE_2 ((OrchestrationHandle) (void*) 0x2)

static DeviceManagerRegistry fixtureRegistry;

void
setUp(void) {
    fixtureRegistry = DeviceManagerRegistry_create(9000, 9099);
}

void
tearDown(void) {
    if (fixtureRegistry) {
        DeviceManagerRegistry_destroy(fixtureRegistry);
        fixtureRegistry = NULL;
    }
}

/* ---- reserve ---- */

void
test_reserve_issuesMonotonicDeviceIdsAndDistinctPorts(void) {
    uint64_t id1, id2;
    uint16_t port1, port2;

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id1, &port1, NULL));
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.2", 102, NULL, &id2, &port2, NULL));

    TEST_ASSERT_EQUAL_UINT64(1, id1);
    TEST_ASSERT_EQUAL_UINT64(2, id2);
    TEST_ASSERT_NOT_EQUAL(port1, port2);
}

void
test_reserve_rejectsDuplicateHostAndPort(void) {
    uint64_t id1, id2;
    uint16_t port1, port2;

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id1, &port1, NULL));
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_HOST_ALREADY_RUNNING,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id2, &port2, NULL));
}

void
test_reserve_allowsSameHostOnDifferentPort(void) {
    uint64_t id1, id2;
    uint16_t port1, port2;

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id1, &port1, NULL));
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 103, NULL, &id2, &port2, NULL));
}

void
test_reserve_returnsBorrowedOwnedPasswordCopy_distinctFromCallersBuffer(void) {
    char password[] = "secret";
    uint64_t id;
    uint16_t port;
    const char* ownedPassword = NULL;

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, password, &id, &port, &ownedPassword));

    TEST_ASSERT_NOT_NULL(ownedPassword);
    TEST_ASSERT_TRUE(ownedPassword != password);
    TEST_ASSERT_EQUAL_STRING("secret", ownedPassword);
}

void
test_reserve_portExhaustion(void) {
    DeviceManagerRegistry_destroy(fixtureRegistry);
    fixtureRegistry = DeviceManagerRegistry_create(9000, 9000);

    uint64_t id1, id2;
    uint16_t port1, port2;

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id1, &port1, NULL));
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_PORT_EXHAUSTED,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.2", 102, NULL, &id2, &port2, NULL));
}

/* ---- finalize / removeIfRunning interplay ---- */

void
test_removeIfRunning_returnsStartInProgress_beforeFinalize(void) {
    uint64_t id;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));

    OrchestrationHandle handle = NULL;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_START_IN_PROGRESS,
            DeviceManagerRegistry_removeIfRunning(fixtureRegistry, id, &handle, NULL, NULL, NULL));
}

void
test_finalize_thenRemoveIfRunning_returnsHandleAndPort(void) {
    uint64_t id;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));

    DeviceManagerRegistry_finalize(fixtureRegistry, id, FAKE_HANDLE_1);

    OrchestrationHandle outHandle = NULL;
    uint16_t outPort = 0;
    char* outHost = NULL;
    char* outPassword = NULL;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_removeIfRunning(fixtureRegistry, id, &outHandle, &outPort, &outHost, &outPassword));

    TEST_ASSERT_EQUAL_PTR(FAKE_HANDLE_1, outHandle);
    TEST_ASSERT_EQUAL_UINT16(port, outPort);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", outHost);
    TEST_ASSERT_NULL(outPassword);

    free(outHost);
}

void
test_removeIfRunning_isNotFound_onSecondCall_noDoubleTeardown(void) {
    uint64_t id;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));
    DeviceManagerRegistry_finalize(fixtureRegistry, id, FAKE_HANDLE_1);

    char* outHost = NULL;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_removeIfRunning(fixtureRegistry, id, NULL, NULL, &outHost, NULL));
    free(outHost);

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND,
            DeviceManagerRegistry_removeIfRunning(fixtureRegistry, id, NULL, NULL, NULL, NULL));
}

void
test_removeIfRunning_unknownDeviceId_returnsNotFound(void) {
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_DEVICE_NOT_FOUND,
            DeviceManagerRegistry_removeIfRunning(fixtureRegistry, 999, NULL, NULL, NULL, NULL));
}

/* ---- rollback frees the port for reuse ---- */

void
test_rollback_freesPortForReuse(void) {
    uint64_t id1, id2;
    uint16_t port1, port2;

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id1, &port1, NULL));

    DeviceManagerRegistry_rollback(fixtureRegistry, id1);

    /* Same host is reservable again post-rollback (dedupe entry was removed too). */
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id2, &port2, NULL));
    TEST_ASSERT_EQUAL_UINT16(port1, port2); /* freed port reused before growing the range */
}

/* ---- freePort + anyRunningDeviceId ---- */

void
test_anyRunningDeviceId_isZero_whenNothingFinalized(void) {
    uint64_t id;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));

    TEST_ASSERT_EQUAL_UINT64(0, DeviceManagerRegistry_anyRunningDeviceId(fixtureRegistry));
}

void
test_anyRunningDeviceId_returnsFinalizedDevice(void) {
    uint64_t id;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));
    DeviceManagerRegistry_finalize(fixtureRegistry, id, FAKE_HANDLE_2);

    TEST_ASSERT_EQUAL_UINT64(id, DeviceManagerRegistry_anyRunningDeviceId(fixtureRegistry));
}

void
test_freePort_thenReserve_reusesIt(void) {
    uint64_t id1, id2;
    uint16_t port1, port2;

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id1, &port1, NULL));
    DeviceManagerRegistry_finalize(fixtureRegistry, id1, FAKE_HANDLE_1);

    char* outHost = NULL;
    uint16_t removedPort = 0;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_removeIfRunning(fixtureRegistry, id1, NULL, &removedPort, &outHost, NULL));
    free(outHost);

    DeviceManagerRegistry_freePort(fixtureRegistry, removedPort);

    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.2", 102, NULL, &id2, &port2, NULL));
    TEST_ASSERT_EQUAL_UINT16(removedPort, port2);
}

/* ---- findDeviceIdByHost ---- */

void
test_findDeviceIdByHost_findsRunningEntry(void) {
    uint64_t id;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));
    DeviceManagerRegistry_finalize(fixtureRegistry, id, FAKE_HANDLE_1);

    uint64_t found = 0;
    TEST_ASSERT_TRUE(DeviceManagerRegistry_findDeviceIdByHost(fixtureRegistry, "10.0.0.1", 102, &found));
    TEST_ASSERT_EQUAL_UINT64(id, found);
}

void
test_findDeviceIdByHost_findsMidStartEntry(void) {
    /* Same match scope as hostAlreadyActive's own dedupe check - a reserved-
     * but-not-yet-finalized entry counts too, since resolving one is exactly
     * how DeviceManager_stopReportingByAddress recovers a deviceId for a
     * caller that never obtained one. */
    uint64_t id;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));

    uint64_t found = 0;
    TEST_ASSERT_TRUE(DeviceManagerRegistry_findDeviceIdByHost(fixtureRegistry, "10.0.0.1", 102, &found));
    TEST_ASSERT_EQUAL_UINT64(id, found);
}

void
test_findDeviceIdByHost_noMatch_returnsFalse(void) {
    uint64_t id, found = 0;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));

    TEST_ASSERT_FALSE(DeviceManagerRegistry_findDeviceIdByHost(fixtureRegistry, "10.0.0.1", 103, &found));
    TEST_ASSERT_FALSE(DeviceManagerRegistry_findDeviceIdByHost(fixtureRegistry, "10.0.0.2", 102, &found));
    TEST_ASSERT_EQUAL_UINT64(0, found); /* untouched on no-match */
}

void
test_findDeviceIdByHost_afterRemoval_returnsFalse(void) {
    uint64_t id;
    uint16_t port;
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_reserve(fixtureRegistry, "10.0.0.1", 102, NULL, &id, &port, NULL));
    DeviceManagerRegistry_finalize(fixtureRegistry, id, FAKE_HANDLE_1);
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_OK,
            DeviceManagerRegistry_removeIfRunning(fixtureRegistry, id, NULL, NULL, NULL, NULL));

    uint64_t found = 0;
    TEST_ASSERT_FALSE(DeviceManagerRegistry_findDeviceIdByHost(fixtureRegistry, "10.0.0.1", 102, &found));
}

/* ---- NULL-safety ---- */

void
test_nullSafety_doesNotCrash(void) {
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManagerRegistry_reserve(NULL, "10.0.0.1", 102, NULL, NULL, NULL, NULL));
    DeviceManagerRegistry_finalize(NULL, 1, FAKE_HANDLE_1);
    DeviceManagerRegistry_rollback(NULL, 1);
    TEST_ASSERT_EQUAL(DEVICE_MANAGER_ERR_INVALID_ARGUMENT,
            DeviceManagerRegistry_removeIfRunning(NULL, 1, NULL, NULL, NULL, NULL));
    DeviceManagerRegistry_freePort(NULL, 9000);
    TEST_ASSERT_EQUAL_UINT64(0, DeviceManagerRegistry_anyRunningDeviceId(NULL));
    uint64_t found = 0;
    TEST_ASSERT_FALSE(DeviceManagerRegistry_findDeviceIdByHost(NULL, "10.0.0.1", 102, &found));
    TEST_ASSERT_FALSE(DeviceManagerRegistry_findDeviceIdByHost(fixtureRegistry, NULL, 102, &found));
    TEST_ASSERT_FALSE(DeviceManagerRegistry_findDeviceIdByHost(fixtureRegistry, "10.0.0.1", 102, NULL));
    DeviceManagerRegistry_destroy(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_reserve_issuesMonotonicDeviceIdsAndDistinctPorts);
    RUN_TEST(test_reserve_rejectsDuplicateHostAndPort);
    RUN_TEST(test_reserve_allowsSameHostOnDifferentPort);
    RUN_TEST(test_reserve_returnsBorrowedOwnedPasswordCopy_distinctFromCallersBuffer);
    RUN_TEST(test_reserve_portExhaustion);

    RUN_TEST(test_removeIfRunning_returnsStartInProgress_beforeFinalize);
    RUN_TEST(test_finalize_thenRemoveIfRunning_returnsHandleAndPort);
    RUN_TEST(test_removeIfRunning_isNotFound_onSecondCall_noDoubleTeardown);
    RUN_TEST(test_removeIfRunning_unknownDeviceId_returnsNotFound);

    RUN_TEST(test_rollback_freesPortForReuse);

    RUN_TEST(test_anyRunningDeviceId_isZero_whenNothingFinalized);
    RUN_TEST(test_anyRunningDeviceId_returnsFinalizedDevice);
    RUN_TEST(test_freePort_thenReserve_reusesIt);

    RUN_TEST(test_findDeviceIdByHost_findsRunningEntry);
    RUN_TEST(test_findDeviceIdByHost_findsMidStartEntry);
    RUN_TEST(test_findDeviceIdByHost_noMatch_returnsFalse);
    RUN_TEST(test_findDeviceIdByHost_afterRemoval_returnsFalse);

    RUN_TEST(test_nullSafety_doesNotCrash);

    return UNITY_END();
}
