#include "unity.h"
#include "stdbool_compat.h"
#include "features/ied_discovery/data/ied_discovery_netif.h"

void
setUp(void) {}

void
tearDown(void) {}

void
test_getInterfaceIpv4_succeeds_forLoopback(void) {
    /* "lo" always exists with an IPv4 address in any environment this test
     * runs in (including sandboxed CI containers with no other interface) -
     * unlike a real LAN interface, this is safe to assert unconditionally. */
    uint32_t address;
    uint32_t netmask;
    bool ok = IedDiscoveryNetif_getInterfaceIpv4("lo", &address, &netmask);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(0x7F000001u, address); /* 127.0.0.1 */
}

void
test_getInterfaceIpv4_fails_forBogusInterfaceName(void) {
    uint32_t address;
    uint32_t netmask;
    bool ok = IedDiscoveryNetif_getInterfaceIpv4("bogus-nonexistent-if0", &address, &netmask);

    TEST_ASSERT_FALSE(ok);
}

void
test_getInterfaceIpv4_fails_onNullArguments(void) {
    uint32_t address;
    uint32_t netmask;

    TEST_ASSERT_FALSE(IedDiscoveryNetif_getInterfaceIpv4(NULL, &address, &netmask));
    TEST_ASSERT_FALSE(IedDiscoveryNetif_getInterfaceIpv4("lo", NULL, &netmask));
    TEST_ASSERT_FALSE(IedDiscoveryNetif_getInterfaceIpv4("lo", &address, NULL));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_getInterfaceIpv4_succeeds_forLoopback);
    RUN_TEST(test_getInterfaceIpv4_fails_forBogusInterfaceName);
    RUN_TEST(test_getInterfaceIpv4_fails_onNullArguments);

    return UNITY_END();
}
