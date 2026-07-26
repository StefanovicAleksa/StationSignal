#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "orchestration/data/orchestration_staging.h"

/*
 * One self-contained temp-file case, matching tests/ied_model/test_ied_model_api.c's
 * own precedent (CLAUDE.md's "Testing" note about permitted file I/O) - proves
 * OrchestrationStaging_writeTempFile/_cleanup's own wiring (mkstemp, write,
 * unlink), not filesystem behavior in general.
 */

void
setUp(void) {}

void
tearDown(void) {}

void
test_writeTempFile_writesExactBytes_andCleanupRemovesIt(void) {
    const uint8_t data[] = { 0x01, 0x02, 0x03, 0xFF, 0x00, 'h', 'i' };

    int outErrno = -1;
    char* path = OrchestrationStaging_writeTempFile(data, sizeof(data), &outErrno);

    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_EQUAL_INT(0, outErrno);

    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "expected the staged file to exist on disk");

    uint8_t readBack[sizeof(data)];
    size_t readCount = fread(readBack, 1, sizeof(readBack), f);
    fclose(f);

    TEST_ASSERT_EQUAL_UINT(sizeof(data), readCount);
    TEST_ASSERT_EQUAL_MEMORY(data, readBack, sizeof(data));

    OrchestrationStaging_cleanup(path);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, access(path, F_OK),
            "expected the staged file to no longer exist after cleanup");

    free(path);
}

void
test_writeTempFile_returnsNull_whenFileDataIsNull(void) {
    int outErrno = 0;
    char* path = OrchestrationStaging_writeTempFile(NULL, 10, &outErrno);

    TEST_ASSERT_NULL(path);
    TEST_ASSERT_NOT_EQUAL(0, outErrno);
}

void
test_cleanup_doesNotCrash_onNullPath(void) {
    OrchestrationStaging_cleanup(NULL); /* must not crash - this is the assertion */
    TEST_PASS();
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_writeTempFile_writesExactBytes_andCleanupRemovesIt);
    RUN_TEST(test_writeTempFile_returnsNull_whenFileDataIsNull);
    RUN_TEST(test_cleanup_doesNotCrash_onNullPath);

    return UNITY_END();
}
