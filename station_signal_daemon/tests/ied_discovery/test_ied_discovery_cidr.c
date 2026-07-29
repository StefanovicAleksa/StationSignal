#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "linked_list.h"
#include "features/ied_discovery/domain/ied_discovery_cidr.h"

void
setUp(void) {}

void
tearDown(void) {}

/* 192.168.1.10 as a plain uint32_t, host byte order. */
#define ADDR_192_168_1_10 ((192u << 24) | (168u << 16) | (1u << 8) | 10u)
#define NETMASK_24 0xFFFFFF00u
#define NETMASK_30 0xFFFFFFFCu
#define NETMASK_31 0xFFFFFFFEu
#define NETMASK_32 0xFFFFFFFFu

static bool
listContains(LinkedList list, const char* value) {
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        if (strcmp((char*) LinkedList_getData(element), value) == 0) return true;
        element = LinkedList_getNext(element);
    }
    return false;
}

/* ---- networkAddress / broadcastAddress / hostCount ---- */

void
test_networkAddress_masksToSubnetFloor(void) {
    uint32_t network = IedDiscoveryCidr_networkAddress(ADDR_192_168_1_10, NETMASK_24);
    TEST_ASSERT_EQUAL_UINT32((192u << 24) | (168u << 16) | (1u << 8) | 0u, network);
}

void
test_broadcastAddress_isSubnetCeiling(void) {
    uint32_t broadcast = IedDiscoveryCidr_broadcastAddress(ADDR_192_168_1_10, NETMASK_24);
    TEST_ASSERT_EQUAL_UINT32((192u << 24) | (168u << 16) | (1u << 8) | 255u, broadcast);
}

void
test_hostCount_slash24_is254(void) {
    TEST_ASSERT_EQUAL_UINT32(254, IedDiscoveryCidr_hostCount(NETMASK_24));
}

void
test_hostCount_slash30_is2(void) {
    TEST_ASSERT_EQUAL_UINT32(2, IedDiscoveryCidr_hostCount(NETMASK_30));
}

void
test_hostCount_slash31_and_slash32_areZero(void) {
    TEST_ASSERT_EQUAL_UINT32(0, IedDiscoveryCidr_hostCount(NETMASK_31));
    TEST_ASSERT_EQUAL_UINT32(0, IedDiscoveryCidr_hostCount(NETMASK_32));
}

/* ---- buildCandidateList ---- */

void
test_buildCandidateList_slash24_excludesNetworkBroadcastAndOwnAddress(void) {
    LinkedList candidates = IedDiscoveryCidr_buildCandidateList(ADDR_192_168_1_10, NETMASK_24,
            ADDR_192_168_1_10, 1024);

    TEST_ASSERT_NOT_NULL(candidates);
    TEST_ASSERT_EQUAL_INT(253, LinkedList_size(candidates)); /* 254 usable hosts minus own address */
    TEST_ASSERT_FALSE(listContains(candidates, "192.168.1.0"));
    TEST_ASSERT_FALSE(listContains(candidates, "192.168.1.255"));
    TEST_ASSERT_FALSE(listContains(candidates, "192.168.1.10"));
    TEST_ASSERT_TRUE(listContains(candidates, "192.168.1.1"));
    TEST_ASSERT_TRUE(listContains(candidates, "192.168.1.254"));

    LinkedList_destroyDeep(candidates, free);
}

void
test_buildCandidateList_slash30_hasTwoHosts(void) {
    /* 192.168.1.8/30: network .8, broadcast .11, usable hosts .9/.10 */
    uint32_t base = (192u << 24) | (168u << 16) | (1u << 8) | 8u;
    LinkedList candidates = IedDiscoveryCidr_buildCandidateList(base, NETMASK_30, 0xFFFFFFFFu, 1024);

    TEST_ASSERT_NOT_NULL(candidates);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(candidates));
    TEST_ASSERT_TRUE(listContains(candidates, "192.168.1.9"));
    TEST_ASSERT_TRUE(listContains(candidates, "192.168.1.10"));

    LinkedList_destroyDeep(candidates, free);
}

void
test_buildCandidateList_slash31_isValidEmptyList(void) {
    LinkedList candidates = IedDiscoveryCidr_buildCandidateList(ADDR_192_168_1_10, NETMASK_31,
            0xFFFFFFFFu, 1024);

    TEST_ASSERT_NOT_NULL(candidates);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(candidates));

    LinkedList_destroyDeep(candidates, free);
}

void
test_buildCandidateList_returnsNull_whenHostCountExceedsMaxHosts(void) {
    /* A /24 has 254 usable hosts - a maxHosts ceiling of 100 must reject it. */
    LinkedList candidates = IedDiscoveryCidr_buildCandidateList(ADDR_192_168_1_10, NETMASK_24,
            ADDR_192_168_1_10, 100);

    TEST_ASSERT_NULL(candidates);
}

/*
 * The address-selection predicate behind the "scan finds nothing" bug: every box permanently
 * carries a 169.254.1.1/24 recovery address next to its real static IP, and the kernel lists the
 * link-local one first, so the sweep enumerated an empty range. See ied_discovery_netif.c.
 */
void
test_isLinkLocal_trueForThe169_254_Block(void) {
    TEST_ASSERT_TRUE(IedDiscoveryCidr_isLinkLocal(0xA9FE0101u));  /* 169.254.1.1, the recovery address */
    TEST_ASSERT_TRUE(IedDiscoveryCidr_isLinkLocal(0xA9FE0000u));  /* 169.254.0.0, first in block */
    TEST_ASSERT_TRUE(IedDiscoveryCidr_isLinkLocal(0xA9FEFFFFu));  /* 169.254.255.255, last in block */
}

void
test_isLinkLocal_falseOutsideTheBlockIncludingItsBoundaries(void) {
    TEST_ASSERT_FALSE(IedDiscoveryCidr_isLinkLocal(0xC0A80132u)); /* 192.168.1.50, the real address */
    TEST_ASSERT_FALSE(IedDiscoveryCidr_isLinkLocal(0x7F000001u)); /* 127.0.0.1 - loopback stays selectable */
    TEST_ASSERT_FALSE(IedDiscoveryCidr_isLinkLocal(0xA9FDFFFFu)); /* 169.253.255.255, just below */
    TEST_ASSERT_FALSE(IedDiscoveryCidr_isLinkLocal(0xA9FF0000u)); /* 169.255.0.0, just above */
    TEST_ASSERT_FALSE(IedDiscoveryCidr_isLinkLocal(0x00000000u));
}

void
test_prefixLength_countsSetBits(void) {
    TEST_ASSERT_EQUAL_UINT32(24, IedDiscoveryCidr_prefixLength(0xFFFFFF00u));
    TEST_ASSERT_EQUAL_UINT32(30, IedDiscoveryCidr_prefixLength(0xFFFFFFFCu));
    TEST_ASSERT_EQUAL_UINT32(8, IedDiscoveryCidr_prefixLength(0xFF000000u));
    TEST_ASSERT_EQUAL_UINT32(32, IedDiscoveryCidr_prefixLength(0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(0, IedDiscoveryCidr_prefixLength(0x00000000u));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_networkAddress_masksToSubnetFloor);
    RUN_TEST(test_broadcastAddress_isSubnetCeiling);
    RUN_TEST(test_hostCount_slash24_is254);
    RUN_TEST(test_hostCount_slash30_is2);
    RUN_TEST(test_hostCount_slash31_and_slash32_areZero);

    RUN_TEST(test_buildCandidateList_slash24_excludesNetworkBroadcastAndOwnAddress);
    RUN_TEST(test_buildCandidateList_slash30_hasTwoHosts);
    RUN_TEST(test_buildCandidateList_slash31_isValidEmptyList);
    RUN_TEST(test_buildCandidateList_returnsNull_whenHostCountExceedsMaxHosts);

    RUN_TEST(test_isLinkLocal_trueForThe169_254_Block);
    RUN_TEST(test_isLinkLocal_falseOutsideTheBlockIncludingItsBoundaries);
    RUN_TEST(test_prefixLength_countsSetBits);

    return UNITY_END();
}
