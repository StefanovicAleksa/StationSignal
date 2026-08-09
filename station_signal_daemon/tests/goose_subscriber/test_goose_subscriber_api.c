#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/goose_subscriber/service/goose_subscriber_api.h"
#include "iec61850_dynamic_model.h"

/*
 * Mirrors test_mms_report_client_api.c's "wiring only" spirit: argument
 * validation and config defaults, without ever calling GooseSubscription_start()
 * against a real target list (no real raw socket - fast, hermetic, per this
 * repo's unit-test convention). Anything that requires a live GooseReceiver/
 * GooseSubscriber (frame reception, the liveness thread's polling behavior)
 * is deliberately left to the E2E test instead, mirroring ied_model's and
 * mms_report_client's convention - GooseSubscriber has no public constructor
 * usable without a real receiver to fake one here.
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

/* ---- GooseSubscription_create argument validation ---- */

void
test_create_error_whenIedModelNull(void) {
    GooseSubscriberError error;
    GooseSubscriberHandle handle = GooseSubscription_create(NULL, "eth0", NULL, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(GOOSE_SUBSCRIBER_ERR_INVALID_ARGUMENT, error);
}

void
test_create_error_whenInterfaceIdNull(void) {
    fixtureModel = buildBareModel();
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    GooseSubscriberError error;
    GooseSubscriberHandle handle = GooseSubscription_create(fixtureIedModelHandle, NULL, NULL, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(GOOSE_SUBSCRIBER_ERR_INVALID_ARGUMENT, error);
}

void
test_create_error_whenInterfaceIdEmpty(void) {
    fixtureModel = buildBareModel();
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    GooseSubscriberError error;
    GooseSubscriberHandle handle = GooseSubscription_create(fixtureIedModelHandle, "", NULL, &error);

    TEST_ASSERT_NULL(handle);
    TEST_ASSERT_EQUAL(GOOSE_SUBSCRIBER_ERR_INVALID_ARGUMENT, error);
}

void
test_create_success_withValidArguments(void) {
    fixtureModel = buildBareModel();
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    GooseSubscriberError error;
    GooseSubscriberHandle handle = GooseSubscription_create(fixtureIedModelHandle, "eth0", NULL, &error);

    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(GOOSE_SUBSCRIBER_OK, error);

    GooseSubscription_destroy(handle);
}

void
test_create_appliesDefaults_whenConfigIsNull(void) {
    fixtureModel = buildBareModel();
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    GooseSubscriberHandle handle = GooseSubscription_create(fixtureIedModelHandle, "eth0", NULL, NULL);
    TEST_ASSERT_NOT_NULL(handle);

    GooseSubscription_destroy(handle);
}

/* ---- GooseSubscription_start ---- */

void
test_start_returnsErrNoTargets_whenModelHasNoGooseControlBlocks(void) {
    fixtureModel = buildBareModel(); /* no GSEControlBlocks */
    fixtureIedHandle = (struct sIedModelHandle) { .model = fixtureModel,
        .accessMode = IED_MODEL_ACCESS_REPORT_ONLY, .categoryFilter = IED_MODEL_LN_CATEGORY_ALL,
        .iedName = "TestIED" };
    fixtureIedModelHandle = &fixtureIedHandle;

    GooseSubscriberHandle handle = GooseSubscription_create(fixtureIedModelHandle, "eth0", NULL, NULL);
    TEST_ASSERT_NOT_NULL(handle);

    GooseSubscriberError startError = GooseSubscription_start(handle);
    TEST_ASSERT_EQUAL(GOOSE_SUBSCRIBER_ERR_NO_TARGETS, startError);

    GooseSubscription_destroy(handle);
}

/* Deliberately not covered here (left to the E2E test, mirroring
 * mms_report_client's convention): GooseSubscription_start() against a model
 * WITH GSEControlBlocks would create a real GooseReceiver and open a raw
 * Ethernet socket - not hermetic/fast enough for a unit test, and typically
 * needs CAP_NET_RAW. */

void
test_stop_and_destroy_doNotCrash_onNullHandle(void) {
    GooseSubscription_stop(NULL);
    GooseSubscription_destroy(NULL);
}

void
test_setCallbacks_areNoOpSafe_onNullHandle(void) {
    GooseSubscription_setRecordCallback(NULL, NULL, NULL);
    GooseSubscription_setStatusCallback(NULL, NULL, NULL);
}

/* ---- GooseSubscriberConfig_defaults ---- */

void
test_configDefaults_matchDocumentedValues(void) {
    GooseSubscriberConfig config;
    GooseSubscriberConfig_defaults(&config);

    TEST_ASSERT_EQUAL_UINT32(0, config.livenessPollMs);
}

void
test_configDefaults_doesNotCrash_onNull(void) {
    GooseSubscriberConfig_defaults(NULL);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_error_whenIedModelNull);
    RUN_TEST(test_create_error_whenInterfaceIdNull);
    RUN_TEST(test_create_error_whenInterfaceIdEmpty);
    RUN_TEST(test_create_success_withValidArguments);
    RUN_TEST(test_create_appliesDefaults_whenConfigIsNull);

    RUN_TEST(test_start_returnsErrNoTargets_whenModelHasNoGooseControlBlocks);
    RUN_TEST(test_stop_and_destroy_doNotCrash_onNullHandle);
    RUN_TEST(test_setCallbacks_areNoOpSafe_onNullHandle);

    RUN_TEST(test_configDefaults_matchDocumentedValues);
    RUN_TEST(test_configDefaults_doesNotCrash_onNull);

    return UNITY_END();
}
