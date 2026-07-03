#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "linked_list.h"
#include "features/scl_bootstrap/domain/scl_bootstrap_usecases.h"

void
setUp(void) {}

void
tearDown(void) {}

/* ---- isDirectoryEntry ---- */

void
test_isDirectoryEntry_true_whenTrailingSlash(void) {
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isDirectoryEntry("subdir/"));
}

void
test_isDirectoryEntry_false_whenPlainFile(void) {
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isDirectoryEntry("reporter1.cid"));
}

void
test_isDirectoryEntry_false_whenNullOrEmpty(void) {
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isDirectoryEntry(NULL));
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isDirectoryEntry(""));
}

/* ---- isSclExtension / extensionPriority ---- */

void
test_isSclExtension_true_forEachRecognizedExtension(void) {
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isSclExtension("device.cid"));
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isSclExtension("device.icd"));
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isSclExtension("device.scd"));
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isSclExtension("device.ssd"));
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isSclExtension("device.sed"));
}

void
test_isSclExtension_caseInsensitive(void) {
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isSclExtension("DEVICE.CID"));
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isSclExtension("Device.Icd"));
}

void
test_isSclExtension_false_forUnrecognizedOrNoExtension(void) {
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isSclExtension("readme.txt"));
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isSclExtension("noextension"));
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isSclExtension(NULL));
}

void
test_isSclExtension_false_forDirectoryEntry(void) {
    /* A directory literally named "foo.cid/" must not be mistaken for a file. */
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isSclExtension("foo.cid/"));
}

void
test_extensionPriority_ordersCidBeforeIcdBeforeScdBeforeSsdBeforeSed(void) {
    int cid = SclBootstrapUseCases_extensionPriority("a.cid");
    int icd = SclBootstrapUseCases_extensionPriority("a.icd");
    int scd = SclBootstrapUseCases_extensionPriority("a.scd");
    int ssd = SclBootstrapUseCases_extensionPriority("a.ssd");
    int sed = SclBootstrapUseCases_extensionPriority("a.sed");

    TEST_ASSERT_TRUE(cid < icd);
    TEST_ASSERT_TRUE(icd < scd);
    TEST_ASSERT_TRUE(scd < ssd);
    TEST_ASSERT_TRUE(ssd < sed);
}

void
test_extensionPriority_negativeOne_whenNoMatch(void) {
    TEST_ASSERT_EQUAL_INT(-1, SclBootstrapUseCases_extensionPriority("readme.txt"));
}

/* ---- pickBestSclFile ---- */

void
test_pickBestSclFile_null_whenListNullOrEmpty(void) {
    TEST_ASSERT_NULL(SclBootstrapUseCases_pickBestSclFile(NULL));

    LinkedList empty = LinkedList_create();
    TEST_ASSERT_NULL(SclBootstrapUseCases_pickBestSclFile(empty));
    LinkedList_destroyStatic(empty);
}

void
test_pickBestSclFile_prefersCidOverIcd(void) {
    LinkedList candidates = LinkedList_create();
    LinkedList_add(candidates, (void*) "device.icd");
    LinkedList_add(candidates, (void*) "device.cid");

    const char* best = SclBootstrapUseCases_pickBestSclFile(candidates);
    TEST_ASSERT_NOT_NULL(best);
    TEST_ASSERT_EQUAL_STRING("device.cid", best);

    LinkedList_destroyStatic(candidates);
}

void
test_pickBestSclFile_breaksTiesLexicographically(void) {
    LinkedList candidates = LinkedList_create();
    LinkedList_add(candidates, (void*) "zzz.cid");
    LinkedList_add(candidates, (void*) "aaa.cid");

    const char* best = SclBootstrapUseCases_pickBestSclFile(candidates);
    TEST_ASSERT_NOT_NULL(best);
    TEST_ASSERT_EQUAL_STRING("aaa.cid", best);

    LinkedList_destroyStatic(candidates);
}

/* ---- isHostListValid ---- */

void
test_isHostListValid_false_whenNullOrEmpty(void) {
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isHostListValid(NULL));

    LinkedList empty = LinkedList_create();
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isHostListValid(empty));
    LinkedList_destroyStatic(empty);
}

void
test_isHostListValid_false_whenContainsNullOrEmptyElement(void) {
    LinkedList withNull = LinkedList_create();
    LinkedList_add(withNull, (void*) "127.0.0.1");
    LinkedList_add(withNull, NULL);
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isHostListValid(withNull));
    LinkedList_destroyStatic(withNull);

    LinkedList withEmpty = LinkedList_create();
    LinkedList_add(withEmpty, (void*) "");
    TEST_ASSERT_FALSE(SclBootstrapUseCases_isHostListValid(withEmpty));
    LinkedList_destroyStatic(withEmpty);
}

void
test_isHostListValid_true_whenAllElementsNonEmpty(void) {
    LinkedList hosts = LinkedList_create();
    LinkedList_add(hosts, (void*) "127.0.0.1");
    LinkedList_add(hosts, (void*) "192.168.1.10");
    TEST_ASSERT_TRUE(SclBootstrapUseCases_isHostListValid(hosts));
    LinkedList_destroyStatic(hosts);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_isDirectoryEntry_true_whenTrailingSlash);
    RUN_TEST(test_isDirectoryEntry_false_whenPlainFile);
    RUN_TEST(test_isDirectoryEntry_false_whenNullOrEmpty);

    RUN_TEST(test_isSclExtension_true_forEachRecognizedExtension);
    RUN_TEST(test_isSclExtension_caseInsensitive);
    RUN_TEST(test_isSclExtension_false_forUnrecognizedOrNoExtension);
    RUN_TEST(test_isSclExtension_false_forDirectoryEntry);

    RUN_TEST(test_extensionPriority_ordersCidBeforeIcdBeforeScdBeforeSsdBeforeSed);
    RUN_TEST(test_extensionPriority_negativeOne_whenNoMatch);

    RUN_TEST(test_pickBestSclFile_null_whenListNullOrEmpty);
    RUN_TEST(test_pickBestSclFile_prefersCidOverIcd);
    RUN_TEST(test_pickBestSclFile_breaksTiesLexicographically);

    RUN_TEST(test_isHostListValid_false_whenNullOrEmpty);
    RUN_TEST(test_isHostListValid_false_whenContainsNullOrEmptyElement);
    RUN_TEST(test_isHostListValid_true_whenAllElementsNonEmpty);

    return UNITY_END();
}
