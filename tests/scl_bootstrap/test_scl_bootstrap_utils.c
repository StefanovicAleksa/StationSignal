#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/scl_bootstrap/utils/scl_bootstrap_utils.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- safeStringDup ---- */

void
test_safeStringDup_null_whenInputNull(void) {
    TEST_ASSERT_NULL(SclBootstrapUtils_safeStringDup(NULL));
}

void
test_safeStringDup_producesIndependentCopy(void) {
    char original[] = "127.0.0.1";
    char* copy = SclBootstrapUtils_safeStringDup(original);

    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", copy);
    TEST_ASSERT_TRUE(copy != original);

    original[0] = 'X';
    TEST_ASSERT_EQUAL_STRING_MESSAGE("127.0.0.1", copy,
            "clone must be independent of the original buffer's later mutations");

    free(copy);
}

/* ---- joinPath ---- */

void
test_joinPath_returnsEntryName_whenParentNullOrEmpty(void) {
    char* joined1 = SclBootstrapUtils_joinPath(NULL, "reporter1.cid");
    TEST_ASSERT_EQUAL_STRING("reporter1.cid", joined1);
    free(joined1);

    char* joined2 = SclBootstrapUtils_joinPath("", "reporter1.cid");
    TEST_ASSERT_EQUAL_STRING("reporter1.cid", joined2);
    free(joined2);
}

void
test_joinPath_concatenatesWithoutExtraSeparator(void) {
    /* Directory entries already carry their own trailing '/' - joinPath must
     * not insert a second one. */
    char* joined = SclBootstrapUtils_joinPath("subdir/", "reporter1.cid");
    TEST_ASSERT_EQUAL_STRING("subdir/reporter1.cid", joined);
    free(joined);
}

void
test_joinPath_handlesNestedDirectories(void) {
    char* joined = SclBootstrapUtils_joinPath("a/b/", "c.cid");
    TEST_ASSERT_EQUAL_STRING("a/b/c.cid", joined);
    free(joined);
}

void
test_joinPath_null_whenEntryNameNull(void) {
    TEST_ASSERT_NULL(SclBootstrapUtils_joinPath("subdir/", NULL));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_safeStringDup_null_whenInputNull);
    RUN_TEST(test_safeStringDup_producesIndependentCopy);

    RUN_TEST(test_joinPath_returnsEntryName_whenParentNullOrEmpty);
    RUN_TEST(test_joinPath_concatenatesWithoutExtraSeparator);
    RUN_TEST(test_joinPath_handlesNestedDirectories);
    RUN_TEST(test_joinPath_null_whenEntryNameNull);

    return UNITY_END();
}
