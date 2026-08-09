#include <stdlib.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "linked_list.h"
#include "orchestration/service/orchestration_api.h"

/*
 * Argument-validation-only wiring tests - never calls Orchestration_run
 * against a real reachable host, no real connection/socket opened (mirrors
 * test_goose_subscriber_api/test_mms_report_client_api's own convention).
 * Real end-to-end behavior against a live IED is covered by
 * integration_tests/orchestration/.
 */

void
setUp(void) {}

void
tearDown(void) {}

static LinkedList
makeHostList(const char* host) {
    LinkedList list = LinkedList_create();
    LinkedList_add(list, (void*) host);
    return list;
}

/* ---- Orchestration_create / Orchestration_destroy ---- */

void
test_create_succeeds_withNullConfig(void) {
    OrchestrationError err;
    OrchestrationHandle handle = Orchestration_create(NULL, &err);

    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, err);

    Orchestration_destroy(handle);
}

void
test_create_succeeds_withExplicitConfig(void) {
    OrchestrationConfig config;
    OrchestrationConfig_defaults(&config);

    OrchestrationError err;
    OrchestrationHandle handle = Orchestration_create(&config, &err);

    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(ORCHESTRATION_OK, err);

    Orchestration_destroy(handle);
}

/* ---- Orchestration_run argument validation ---- */

void
test_run_rejectsNullHandle(void) {
    LinkedList hosts = makeHostList("127.0.0.1");

    OrchestrationError err = Orchestration_run(NULL, hosts, 102, "Reporter1", "lo",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT, err);
    LinkedList_destroyStatic(hosts);
}

void
test_run_rejectsNullOrEmptyHostList(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);
    LinkedList empty = LinkedList_create();

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_run(handle, NULL, 102, "Reporter1", "lo", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL,
                    NULL, NULL));
    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_run(handle, empty, 102, "Reporter1", "lo", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL,
                    NULL, NULL));

    LinkedList_destroy(empty);
    Orchestration_destroy(handle);
}

void
test_run_rejectsNonPositivePort(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);
    LinkedList hosts = makeHostList("127.0.0.1");

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_run(handle, hosts, 0, "Reporter1", "lo", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL,
                    NULL, NULL));
    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_run(handle, hosts, -1, "Reporter1", "lo", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL,
                    NULL, NULL));

    LinkedList_destroyStatic(hosts);
    Orchestration_destroy(handle);
}

void
test_run_doesNotRejectNullOrEmptyIedName_atValidation(void) {
    /* NULL/empty iedName now means "auto-detect from the staged SCL" (see
     * orchestration_api.h), no longer an argument-validation rejection.
     * Nothing real listens on 127.0.0.1:102 in this hermetic test, so the
     * run fails one stage later, at BOOTSTRAP, instead - proving the
     * validation gate itself no longer rejects an empty iedName. */
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);
    LinkedList hosts = makeHostList("127.0.0.1");
    OrchestrationErrorDetail detail;

    OrchestrationError err = Orchestration_run(handle, hosts, 102, NULL, "lo",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,&detail, NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT, err);
    TEST_ASSERT_EQUAL(ORCHESTRATION_STAGE_BOOTSTRAP, detail.stage);

    err = Orchestration_run(handle, hosts, 102, "", "lo", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,&detail, NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT, err);
    TEST_ASSERT_EQUAL(ORCHESTRATION_STAGE_BOOTSTRAP, detail.stage);

    LinkedList_destroyStatic(hosts);
    Orchestration_destroy(handle);
}

void
test_run_rejectsNullOrEmptyInterfaceId(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);
    LinkedList hosts = makeHostList("127.0.0.1");

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_run(handle, hosts, 102, "Reporter1", NULL, IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL,
                    NULL, NULL));
    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_run(handle, hosts, 102, "Reporter1", "", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL,
                    NULL, NULL));

    LinkedList_destroyStatic(hosts);
    Orchestration_destroy(handle);
}

void
test_run_rejectsReentry_whenAlreadyRunning(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);
    handle->running = true; /* struct is visible by convention - see orchestration_types.h */

    LinkedList hosts = makeHostList("127.0.0.1");
    OrchestrationError err = Orchestration_run(handle, hosts, 102, "Reporter1", "lo",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT, err);

    LinkedList_destroyStatic(hosts);
    handle->running = false; /* avoid Orchestration_destroy trying to tear down non-existent workers */
    Orchestration_destroy(handle);
}

/* ---- Orchestration_runFromLocalFile argument validation ---- */

void
test_runFromLocalFile_rejectsNullHandle(void) {
    OrchestrationError err = Orchestration_runFromLocalFile(NULL, "/tmp/some.icd", "127.0.0.1", 102,
            "Reporter1", "lo", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT, err);
}

void
test_runFromLocalFile_rejectsNullOrEmptySclFilePath(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_runFromLocalFile(handle, NULL, "127.0.0.1", 102, "Reporter1", "lo",
                    IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_runFromLocalFile(handle, "", "127.0.0.1", 102, "Reporter1", "lo",
                    IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL));

    Orchestration_destroy(handle);
}

void
test_runFromLocalFile_rejectsNullOrEmptyHost(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_runFromLocalFile(handle, "/tmp/some.icd", NULL, 102, "Reporter1", "lo",
                    IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_runFromLocalFile(handle, "/tmp/some.icd", "", 102, "Reporter1", "lo",
                    IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL));

    Orchestration_destroy(handle);
}

void
test_runFromLocalFile_rejectsNonPositivePort(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_runFromLocalFile(handle, "/tmp/some.icd", "127.0.0.1", 0, "Reporter1", "lo",
                    IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_runFromLocalFile(handle, "/tmp/some.icd", "127.0.0.1", -1, "Reporter1", "lo",
                    IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL));

    Orchestration_destroy(handle);
}

void
test_runFromLocalFile_doesNotRejectNullOrEmptyIedName_atValidation(void) {
    /* Same contract as Orchestration_run's own iedName auto-detect - NULL/
     * empty isn't rejected at validation. A nonexistent sclFilePath means
     * this fails one stage later instead, at IED_NAME_RESOLUTION (the
     * listing itself hits IED_MODEL_ERR_FILE_NOT_FOUND) - hermetic, no real
     * IED needed, just a bad local path. */
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);
    OrchestrationErrorDetail detail;

    OrchestrationError err = Orchestration_runFromLocalFile(handle, "/nonexistent/path.icd", "127.0.0.1",
            102, NULL, "lo", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,&detail, NULL, NULL);

    TEST_ASSERT_NOT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT, err);
    TEST_ASSERT_EQUAL(ORCHESTRATION_STAGE_IED_NAME_RESOLUTION, detail.stage);
    TEST_ASSERT_EQUAL(IED_MODEL_ERR_FILE_NOT_FOUND, detail.iedNameListError);

    Orchestration_destroy(handle);
}

void
test_runFromLocalFile_rejectsNullOrEmptyInterfaceId(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_runFromLocalFile(handle, "/tmp/some.icd", "127.0.0.1", 102, "Reporter1", NULL,
                    IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT,
            Orchestration_runFromLocalFile(handle, "/tmp/some.icd", "127.0.0.1", 102, "Reporter1", "",
                    IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL));

    Orchestration_destroy(handle);
}

void
test_runFromLocalFile_rejectsReentry_whenAlreadyRunning(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);
    handle->running = true; /* struct is visible by convention - see orchestration_types.h */

    OrchestrationError err = Orchestration_runFromLocalFile(handle, "/tmp/some.icd", "127.0.0.1", 102,
            "Reporter1", "lo", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL,NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(ORCHESTRATION_ERR_INVALID_ARGUMENT, err);

    handle->running = false; /* avoid Orchestration_destroy trying to tear down non-existent workers */
    Orchestration_destroy(handle);
}

/* ---- Orchestration_stop / Orchestration_destroy on a never-run handle ---- */

void
test_stop_isNoOp_onNeverRunHandle(void) {
    OrchestrationHandle handle = Orchestration_create(NULL, NULL);

    Orchestration_stop(handle); /* must not crash */
    Orchestration_stop(handle); /* repeatable */

    Orchestration_destroy(handle);
}

void
test_stop_and_destroy_doNotCrash_onNullHandle(void) {
    Orchestration_stop(NULL);
    Orchestration_destroy(NULL);
    TEST_PASS();
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_succeeds_withNullConfig);
    RUN_TEST(test_create_succeeds_withExplicitConfig);

    RUN_TEST(test_run_rejectsNullHandle);
    RUN_TEST(test_run_rejectsNullOrEmptyHostList);
    RUN_TEST(test_run_rejectsNonPositivePort);
    RUN_TEST(test_run_doesNotRejectNullOrEmptyIedName_atValidation);
    RUN_TEST(test_run_rejectsNullOrEmptyInterfaceId);
    RUN_TEST(test_run_rejectsReentry_whenAlreadyRunning);

    RUN_TEST(test_runFromLocalFile_rejectsNullHandle);
    RUN_TEST(test_runFromLocalFile_rejectsNullOrEmptySclFilePath);
    RUN_TEST(test_runFromLocalFile_rejectsNullOrEmptyHost);
    RUN_TEST(test_runFromLocalFile_rejectsNonPositivePort);
    RUN_TEST(test_runFromLocalFile_doesNotRejectNullOrEmptyIedName_atValidation);
    RUN_TEST(test_runFromLocalFile_rejectsNullOrEmptyInterfaceId);
    RUN_TEST(test_runFromLocalFile_rejectsReentry_whenAlreadyRunning);

    RUN_TEST(test_stop_isNoOp_onNeverRunHandle);
    RUN_TEST(test_stop_and_destroy_doNotCrash_onNullHandle);

    return UNITY_END();
}
