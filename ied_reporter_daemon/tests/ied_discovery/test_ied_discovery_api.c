#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ied_discovery/service/ied_discovery_api.h"

/*
 * Argument-validation and config-defaults wiring tests, plus the one
 * deterministic real-getifaddrs assertion that needs no network probe at
 * all (the maxHosts ceiling against loopback's own real, large netmask) -
 * mirrors test_goose_subscriber_api/test_mms_report_client_api's "wiring
 * only" spirit. A real scan against a real neighbor IED is covered by
 * integration_tests/ied_discovery/ instead.
 */

void
setUp(void) {}

void
tearDown(void) {}

/* ---- IedDiscoveryConfig_defaults ---- */

void
test_configDefaults_setsSaneValues(void) {
    IedDiscoveryConfig config;
    IedDiscoveryConfig_defaults(&config);

    TEST_ASSERT_EQUAL_UINT32(500, config.tcpProbeTimeoutMs);
    TEST_ASSERT_EQUAL_INT(64, config.maxConcurrentTcpProbes);
    TEST_ASSERT_EQUAL_UINT32(3000, config.mmsConnectTimeoutMs);
    TEST_ASSERT_EQUAL_UINT32(1024, config.maxHosts);
    TEST_ASSERT_NULL(config.acseAuthPassword);
}

/* ---- IedDiscovery_create / _destroy ---- */

void
test_create_succeeds_withNullConfig(void) {
    IedDiscoveryError error;
    IedDiscoveryHandle handle = IedDiscovery_create(NULL, &error);

    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_OK, error);

    IedDiscovery_destroy(handle);
}

void
test_destroy_doesNotCrash_onNullHandle(void) {
    IedDiscovery_destroy(NULL); /* must not crash - this is the assertion */
    TEST_PASS();
}

/* ---- IedDiscovery_verifyHost argument validation ---- */

void
test_verifyHost_rejectsNullHandle(void) {
    IedDiscoveryError error;
    IedDiscoveryHostStatus status = IedDiscovery_verifyHost(NULL, "127.0.0.1", 102, &error);

    TEST_ASSERT_EQUAL(IED_DISCOVERY_ERR_INVALID_ARGUMENT, error);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_HOST_NOT_TCP_REACHABLE, status);
}

void
test_verifyHost_rejectsNullOrEmptyHost(void) {
    IedDiscoveryHandle handle = IedDiscovery_create(NULL, NULL);
    IedDiscoveryError error;

    IedDiscovery_verifyHost(handle, NULL, 102, &error);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_ERR_INVALID_ARGUMENT, error);

    IedDiscovery_verifyHost(handle, "", 102, &error);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_ERR_INVALID_ARGUMENT, error);

    IedDiscovery_destroy(handle);
}

void
test_verifyHost_rejectsNonPositivePort(void) {
    IedDiscoveryHandle handle = IedDiscovery_create(NULL, NULL);
    IedDiscoveryError error;

    IedDiscovery_verifyHost(handle, "127.0.0.1", 0, &error);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_ERR_INVALID_ARGUMENT, error);

    IedDiscovery_destroy(handle);
}

/* ---- IedDiscovery_scanSubnet argument validation + the maxHosts ceiling ---- */

void
test_scanSubnet_rejectsNullOrEmptyInterfaceId(void) {
    IedDiscoveryHandle handle = IedDiscovery_create(NULL, NULL);
    IedDiscoveryError error;

    TEST_ASSERT_NULL(IedDiscovery_scanSubnet(handle, NULL, 102, &error));
    TEST_ASSERT_EQUAL(IED_DISCOVERY_ERR_INVALID_ARGUMENT, error);

    TEST_ASSERT_NULL(IedDiscovery_scanSubnet(handle, "", 102, &error));
    TEST_ASSERT_EQUAL(IED_DISCOVERY_ERR_INVALID_ARGUMENT, error);

    IedDiscovery_destroy(handle);
}

void
test_scanSubnet_errorInterfaceNotFound_forBogusInterface(void) {
    IedDiscoveryHandle handle = IedDiscovery_create(NULL, NULL);
    IedDiscoveryError error;

    LinkedList result = IedDiscovery_scanSubnet(handle, "bogus-nonexistent-if0", 102, &error);

    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND, error);

    IedDiscovery_destroy(handle);
}

void
test_scanSubnet_errorSubnetTooLarge_forLoopbackAtDefaultCeiling(void) {
    /* Deterministic, real getifaddrs() integration, but no network probe
     * ever happens: loopback's real netmask (typically /8) has far more
     * hosts than the default maxHosts=1024 ceiling, so this must fail fast
     * at that check - proving the safety valve without depending on
     * anything topology-specific beyond "lo" always existing. */
    IedDiscoveryHandle handle = IedDiscovery_create(NULL, NULL);
    IedDiscoveryError error;

    LinkedList result = IedDiscovery_scanSubnet(handle, "lo", 102, &error);

    TEST_ASSERT_NULL(result);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_ERR_SUBNET_TOO_LARGE, error);

    IedDiscovery_destroy(handle);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_configDefaults_setsSaneValues);

    RUN_TEST(test_create_succeeds_withNullConfig);
    RUN_TEST(test_destroy_doesNotCrash_onNullHandle);

    RUN_TEST(test_verifyHost_rejectsNullHandle);
    RUN_TEST(test_verifyHost_rejectsNullOrEmptyHost);
    RUN_TEST(test_verifyHost_rejectsNonPositivePort);

    RUN_TEST(test_scanSubnet_rejectsNullOrEmptyInterfaceId);
    RUN_TEST(test_scanSubnet_errorInterfaceNotFound_forBogusInterface);
    RUN_TEST(test_scanSubnet_errorSubnetTooLarge_forLoopbackAtDefaultCeiling);

    return UNITY_END();
}
