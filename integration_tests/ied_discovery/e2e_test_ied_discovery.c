#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ied_discovery/service/ied_discovery_api.h"
#include "hal_thread.h"
#include "sim_types.h"

/*
 * End-to-end test: runs a real "Reporter1" IED simulator (see
 * integration_tests/ied_simulator/, fully decoupled from src/) in the same
 * process and drives the real ied_discovery service API against it over
 * loopback - a real TCP probe plus a real MMS/ACSE association, no mocking.
 *
 * Deliberately does NOT exercise IedDiscovery_scanSubnet/getifaddrs - a real
 * LAN subnet scan is machine-topology-dependent, not something this
 * hermetic, sandboxed test environment can assert on (see
 * tests/ied_discovery/test_ied_discovery_api.c's own loopback-ceiling test
 * for the one deterministic getifaddrs assertion that IS covered). This
 * file proves the verification primitive itself (IedDiscovery_verifyHost)
 * against a real IED, same "real fixture, real feature code" philosophy as
 * every other integration_tests/<feature>/.
 *
 * No sudo needed - ied_discovery only ever uses plain TCP (hal_socket.h, via
 * scl_bootstrap's delegate) and MMS (IedConnection), same as
 * integration_tests/scl_bootstrap/.
 */

#define TEST_PORT 10401
/* Deliberately not bound by anything in this file - see
 * integration_tests/scl_bootstrap/e2e_test_scl_bootstrap.c's own DEAD_PORT
 * comment for why "nothing is listening" can't be modeled with a second
 * loopback address here. */
#define DEAD_PORT 10499

#define LIVE_HOST "127.0.0.1"

void
setUp(void) {}

void
tearDown(void) {}

void
test_verifyHost_confirmsRealDevice(void) {
    SimServer sim = SimServer_create();
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    IedDiscoveryHandle handle = IedDiscovery_create(NULL, NULL);
    TEST_ASSERT_NOT_NULL(handle);

    IedDiscoveryError err;
    IedDiscoveryHostStatus status = IedDiscovery_verifyHost(handle, LIVE_HOST, TEST_PORT, &err);

    TEST_ASSERT_EQUAL(IED_DISCOVERY_OK, err);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_HOST_CONFIRMED, status);

    IedDiscovery_destroy(handle);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_verifyHost_notTcpReachable_whenNothingListening(void) {
    IedDiscoveryHandle handle = IedDiscovery_create(NULL, NULL);

    IedDiscoveryError err;
    IedDiscoveryHostStatus status = IedDiscovery_verifyHost(handle, LIVE_HOST, DEAD_PORT, &err);

    TEST_ASSERT_EQUAL(IED_DISCOVERY_OK, err);
    TEST_ASSERT_EQUAL(IED_DISCOVERY_HOST_NOT_TCP_REACHABLE, status);

    IedDiscovery_destroy(handle);
}

void
test_verifyHost_authRequiredNoPasswordConfigured_accessDenied(void) {
    SimServer sim = SimServer_create();
    SimServer_requireAuthentication(sim, "secret123");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    IedDiscoveryHandle handle = IedDiscovery_create(NULL, NULL); /* no acseAuthPassword configured */

    IedDiscoveryError err;
    IedDiscoveryHostStatus status = IedDiscovery_verifyHost(handle, LIVE_HOST, TEST_PORT, &err);

    TEST_ASSERT_EQUAL(IED_DISCOVERY_HOST_ACCESS_DENIED, status);

    IedDiscovery_destroy(handle);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_verifyHost_authRequiredCorrectPassword_confirmed(void) {
    SimServer sim = SimServer_create();
    SimServer_requireAuthentication(sim, "secret123");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    IedDiscoveryConfig config;
    IedDiscoveryConfig_defaults(&config);
    config.acseAuthPassword = "secret123";
    IedDiscoveryHandle handle = IedDiscovery_create(&config, NULL);

    IedDiscoveryError err;
    IedDiscoveryHostStatus status = IedDiscovery_verifyHost(handle, LIVE_HOST, TEST_PORT, &err);

    TEST_ASSERT_EQUAL(IED_DISCOVERY_HOST_CONFIRMED, status);

    IedDiscovery_destroy(handle);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_verifyHost_authRequiredWrongPassword_accessDenied(void) {
    SimServer sim = SimServer_create();
    SimServer_requireAuthentication(sim, "secret123");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    IedDiscoveryConfig config;
    IedDiscoveryConfig_defaults(&config);
    config.acseAuthPassword = "wrong-password";
    IedDiscoveryHandle handle = IedDiscovery_create(&config, NULL);

    IedDiscoveryError err;
    IedDiscoveryHostStatus status = IedDiscovery_verifyHost(handle, LIVE_HOST, TEST_PORT, &err);

    TEST_ASSERT_EQUAL(IED_DISCOVERY_HOST_ACCESS_DENIED, status);

    IedDiscovery_destroy(handle);
    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_verifyHost_confirmsRealDevice);
    RUN_TEST(test_verifyHost_notTcpReachable_whenNothingListening);
    RUN_TEST(test_verifyHost_authRequiredNoPasswordConfigured_accessDenied);
    RUN_TEST(test_verifyHost_authRequiredCorrectPassword_confirmed);
    RUN_TEST(test_verifyHost_authRequiredWrongPassword_accessDenied);

    return UNITY_END();
}
