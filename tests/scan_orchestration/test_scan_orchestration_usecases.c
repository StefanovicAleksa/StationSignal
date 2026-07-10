#include "unity.h"
#include "stdbool_compat.h"
#include "scan_orchestration/domain/scan_orchestration_usecases.h"

void
setUp(void) {}

void
tearDown(void) {}

void
test_isHostNew_trueOnEmptySeenSet(void) {
    TEST_ASSERT_TRUE(ScanOrchestrationUseCases_isHostNew(NULL, 0, "192.168.1.1"));
}

void
test_isHostNew_falseWhenPresent(void) {
    const char* const seen[2] = { "192.168.1.1", "192.168.1.2" };
    TEST_ASSERT_FALSE(ScanOrchestrationUseCases_isHostNew(seen, 2, "192.168.1.2"));
}

void
test_isHostNew_trueWhenNotPresent(void) {
    const char* const seen[2] = { "192.168.1.1", "192.168.1.2" };
    TEST_ASSERT_TRUE(ScanOrchestrationUseCases_isHostNew(seen, 2, "192.168.1.3"));
}

void
test_isHostNew_exactStringMatch_noNormalization(void) {
    const char* const seen[1] = { "192.168.1.1" };
    TEST_ASSERT_TRUE(ScanOrchestrationUseCases_isHostNew(seen, 1, "192.168.001.001"));
}

void
test_isHostNew_trueOnNullOrEmptyHost(void) {
    const char* const seen[1] = { "192.168.1.1" };
    TEST_ASSERT_TRUE(ScanOrchestrationUseCases_isHostNew(seen, 1, NULL));
    TEST_ASSERT_TRUE(ScanOrchestrationUseCases_isHostNew(seen, 1, ""));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_isHostNew_trueOnEmptySeenSet);
    RUN_TEST(test_isHostNew_falseWhenPresent);
    RUN_TEST(test_isHostNew_trueWhenNotPresent);
    RUN_TEST(test_isHostNew_exactStringMatch_noNormalization);
    RUN_TEST(test_isHostNew_trueOnNullOrEmptyHost);

    return UNITY_END();
}
