#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/mms_report_client/service/mms_report_client_api.h"
#include "iec61850_dynamic_model.h"

/*
 * Mirrors test_ied_model_api.c's "wiring only" spirit: argument validation
 * and config defaults, without ever calling MmsReportClient_start() against
 * a real target (no real socket - fast, hermetic, per this repo's unit-test
 * convention). Anything that requires a live IedConnection (connect/enable/
 * reconnect behavior, the report adapter's ClientReport field extraction) is
 * deliberately left to the E2E test instead, mirroring ied_model's stated
 * convention - ClientReport has no public constructor to fake one here.
 */

static IedModel* fixtureModel;
static struct sIedModelHandle fixtureIedHandle;
static IedModelHandle fixtureIedModelHandle;

static IedModel*
buildBareModel(void) {
    return IedModel_create("TestIED");
}

void
setUp(void) {}

void
tearDown(void) {
    if (fixtureModel) {
        IedModel_destroy(fixtureModel);
        fixtureModel = NULL;
    }
}

/* ---- MmsReportClient_create argument validation ---- */

void
test_create_error_whenIedModelNull(void) {
    MmsReportClientError error;
    MmsReportClientHandle client = MmsReportClient_create(NULL, "127.0.0.1", 102, NULL, &error);

    TEST_ASSERT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_ERR_INVALID_ARGUMENT, error);
}

void
test_create_error_whenHostNull(void) {
    fixtureModel = buildBareModel();
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    MmsReportClientError error;
    MmsReportClientHandle client = MmsReportClient_create(fixtureIedModelHandle, NULL, 102, NULL, &error);

    TEST_ASSERT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_ERR_INVALID_ARGUMENT, error);
}

void
test_create_error_whenPortNotPositive(void) {
    fixtureModel = buildBareModel();
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    MmsReportClientError error;
    MmsReportClientHandle client = MmsReportClient_create(fixtureIedModelHandle, "127.0.0.1", 0, NULL, &error);

    TEST_ASSERT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_ERR_INVALID_ARGUMENT, error);
}

void
test_create_success_withValidArguments(void) {
    fixtureModel = buildBareModel();
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    MmsReportClientError error;
    MmsReportClientHandle client = MmsReportClient_create(fixtureIedModelHandle, "127.0.0.1", 102, NULL, &error);

    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_OK, error);

    MmsReportClient_destroy(client);
}

/* ---- MmsReportClient_start ---- */

void
test_start_error_whenNoReportTargets(void) {
    fixtureModel = buildBareModel(); /* no RCBs */
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    MmsReportClientHandle client = MmsReportClient_create(fixtureIedModelHandle, "127.0.0.1", 102, NULL, NULL);
    TEST_ASSERT_NOT_NULL(client);

    MmsReportClientError startError = MmsReportClient_start(client);
    TEST_ASSERT_EQUAL(MMS_REPORT_CLIENT_ERR_INVALID_ARGUMENT, startError);

    MmsReportClient_destroy(client);
}

/* Deliberately not covered here (left to the E2E test, mirroring ied_model's
 * convention): MmsReportClient_start() against a model WITH report targets
 * would create a real IedConnection and start the reconnect supervisor
 * thread attempting a real TCP connect - not hermetic/fast enough for a unit
 * test. */

void
test_stop_and_destroy_doNotCrash_onNullClient(void) {
    MmsReportClient_stop(NULL);
    MmsReportClient_destroy(NULL);
}

/* ---- MmsReportClientConfig_defaults ---- */

void
test_configDefaults_matchDocumentedValues(void) {
    MmsReportClientConfig config;
    MmsReportClientConfig_defaults(&config);

    TEST_ASSERT_TRUE(config.generalInterrogationOnEnable);
    TEST_ASSERT_EQUAL_UINT32(0, config.connectTimeoutMs);
    TEST_ASSERT_EQUAL_UINT32(0, config.requestTimeoutMs);
    TEST_ASSERT_EQUAL_UINT32(1000, config.reconnectInitialDelayMs);
    TEST_ASSERT_EQUAL_UINT32(30000, config.reconnectMaxDelayMs);
}

void
test_configDefaults_doesNotCrash_onNull(void) {
    MmsReportClientConfig_defaults(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_error_whenIedModelNull);
    RUN_TEST(test_create_error_whenHostNull);
    RUN_TEST(test_create_error_whenPortNotPositive);
    RUN_TEST(test_create_success_withValidArguments);

    RUN_TEST(test_start_error_whenNoReportTargets);
    RUN_TEST(test_stop_and_destroy_doNotCrash_onNullClient);

    RUN_TEST(test_configDefaults_matchDocumentedValues);
    RUN_TEST(test_configDefaults_doesNotCrash_onNull);

    return UNITY_END();
}
