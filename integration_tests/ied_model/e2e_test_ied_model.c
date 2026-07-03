#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ied_model/service/ied_model_api.h"
#include "iec61850_model.h"

/*
 * End-to-end test: exercises the full ied_model pipeline (mxml parsing, the
 * DataTypeTemplates join-graph resolution, DataSet/FCDA/ReportControl/
 * GSEControl attachment, Communication addressing) against a real,
 * self-contained SCL fixture (fixtures/breaker1.cid) - not a synthetic tiny
 * fixture like the unit tests use. This is deliberately the only place that
 * fixture's shape gets verified; the unit tests in tests/ied_model/ cover
 * usecases/api/utils logic in isolation with in-memory/minimal fixtures and
 * don't duplicate this.
 *
 * Fixture shape (see fixtures/breaker1.cid): IED "Breaker1", LDevice "CB1",
 * LLN0 + XCBR1 (breaker, with a controllable "Pos" DO) + MMXU1 (measurement).
 * One buffered ReportControl, one GSEControl with full <Communication>
 * addressing (VLAN 10, APPID 2000, MAC 01-0c-cd-01-00-05).
 */

#define FIXTURE_PATH "fixtures/breaker1.cid"

void
setUp(void) {}

void
tearDown(void) {}

void
test_loadsFixtureSuccessfully(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_READ_AND_WRITE, &error);

    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected breaker1.cid to load successfully");
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

    IedModel_release(handle);
}

void
test_gooseSubscriptionTarget_resolvesCorrectReference(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    LinkedList goose = IedModel_getGooseSubscriptionTargets(handle);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(goose));

    GooseSubscriptionTarget* target =
        (GooseSubscriptionTarget*) LinkedList_getData(LinkedList_getNext(goose));
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/LLN0$GO$gcbStatus", target->objectReference);
    TEST_ASSERT_TRUE(target->hasAddress);
    TEST_ASSERT_EQUAL_UINT16(10, target->vlanId);
    TEST_ASSERT_EQUAL_UINT8(4, target->vlanPriority);
    TEST_ASSERT_EQUAL_UINT16(2000, target->appId);
    uint8_t expectedMac[6] = { 0x01, 0x0c, 0xcd, 0x01, 0x00, 0x05 };
    TEST_ASSERT_EQUAL_MEMORY(expectedMac, target->dstMac, 6);

    LinkedList_destroyDeep(goose, IedModel_destroyGooseSubscriptionTarget);
    IedModel_release(handle);
}

void
test_reportSubscriptionTarget_resolvesCorrectReference(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    LinkedList report = IedModel_getReportSubscriptionTargets(handle);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(report));

    ReportControlBlockTarget* target =
        (ReportControlBlockTarget*) LinkedList_getData(LinkedList_getNext(report));
    /* fixture's brcbMain is buffered="true" on dataset "ds1" - must use ".BR." */
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/LLN0.BR.brcbMain", target->objectReference);
    TEST_ASSERT_TRUE(target->buffered);
    TEST_ASSERT_EQUAL_STRING("Breaker1CB1/LLN0$ds1", target->datasetReference);

    LinkedList_destroyDeep(report, IedModel_destroyReportControlBlockTarget);
    IedModel_release(handle);
}

void
test_readTargets_matchExpectedCount_andExcludeConfigAndControlAttributes(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_READ_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    LinkedList read = IedModel_getReadTargets(handle);

    /* LLN0: Mod{stVal,q,t} + Beh{stVal,q,t} + Health{stVal,q,t} = 9 ST (NamPlt is DC, excluded)
     * XCBR1: Mod{stVal,q,t} + Pos{stVal,q,t} = 6 ST (Pos.Oper is CO, Pos.ctlModel is CF, excluded)
     * MMXU1: Mod{stVal,q,t} = 3 ST; TotW{mag,q,t} = 3 MX
     * Total = 9 + 6 + 3 ST from Mod's, + 3 MX = 21 */
    TEST_ASSERT_EQUAL_INT(21, LinkedList_size(read));

    bool foundCtlModel = false, foundOper = false;
    LinkedList element = LinkedList_getNext(read);
    while (element) {
        const char* ref = (const char*) LinkedList_getData(element);
        if (strstr(ref, "ctlModel")) foundCtlModel = true;
        if (strstr(ref, "Oper")) foundOper = true;
        element = LinkedList_getNext(element);
    }
    TEST_ASSERT_FALSE_MESSAGE(foundCtlModel, "ctlModel is FC=CF and must not be a read target");
    TEST_ASSERT_FALSE_MESSAGE(foundOper, "Oper is FC=CO and must not be a read target");

    LinkedList_destroyDeep(read, free);
    IedModel_release(handle);
}

void
test_controlTargets_includeOnlyThePosDataObject(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_READ_AND_WRITE, &error);
    TEST_ASSERT_NOT_NULL(handle);

    LinkedList control = IedModel_getControlTargets(handle);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(control));

    const char* ref = (const char*) LinkedList_getData(LinkedList_getNext(control));
    TEST_ASSERT_TRUE_MESSAGE(strstr(ref, "Pos") != NULL, "expected the controllable target to be XCBR1.Pos");

    LinkedList_destroyDeep(control, free);
    IedModel_release(handle);
}

void
test_gooseControlBlock_hasPhyComAddressAttached_fromCommunicationSection(void) {
    /* Digs into the handle's internal model directly (test code, not a public
     * API contract) to verify the <Communication>-derived GOOSE transport
     * addressing actually attached, not just that a reference string exists. */
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    GSEControlBlock* gcb = handle->model->gseCBs;
    TEST_ASSERT_NOT_NULL(gcb);
    TEST_ASSERT_NOT_NULL_MESSAGE(gcb->address, "expected PhyComAddress resolved from <Communication>");
    TEST_ASSERT_EQUAL_UINT16(10, gcb->address->vlanId);
    TEST_ASSERT_EQUAL_UINT16(2000, gcb->address->appId);
    TEST_ASSERT_EQUAL_UINT8(0x01, gcb->address->dstAddress[0]);
    TEST_ASSERT_EQUAL_UINT8(0x05, gcb->address->dstAddress[5]);

    IedModel_release(handle);
}

void
test_reportOnlyMode_deniesReadAndControlTargets(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    LinkedList read = IedModel_getReadTargets(handle);
    LinkedList control = IedModel_getControlTargets(handle);

    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(read));
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(control));

    LinkedList_destroyDeep(read, free);
    LinkedList_destroyDeep(control, free);
    IedModel_release(handle);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_loadsFixtureSuccessfully);
    RUN_TEST(test_gooseSubscriptionTarget_resolvesCorrectReference);
    RUN_TEST(test_reportSubscriptionTarget_resolvesCorrectReference);
    RUN_TEST(test_readTargets_matchExpectedCount_andExcludeConfigAndControlAttributes);
    RUN_TEST(test_controlTargets_includeOnlyThePosDataObject);
    RUN_TEST(test_gooseControlBlock_hasPhyComAddressAttached_fromCommunicationSection);
    RUN_TEST(test_reportOnlyMode_deniesReadAndControlTargets);

    return UNITY_END();
}
