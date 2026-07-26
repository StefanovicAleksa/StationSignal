#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ied_model/service/ied_model_api.h"
#include "iec61850_model.h"
#include "mms_value.h"

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
 * addressing (VLAN-ID/APPID text "10"/"2000", both parsed as HEX per
 * IEC 61850-8-1 - i.e. 0x10=16 / 0x2000=8192, not decimal 10/2000 - see the
 * VLAN/APPID assertions below; MAC 01-0c-cd-01-00-05).
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
    /* Fixture text is "10"/"2000" - VLAN-ID/APPID are hex strings per
     * IEC 61850-8-1, so the correctly-parsed values are 0x10=16/0x2000=8192,
     * not decimal 10/2000 (see this file's own top comment). */
    TEST_ASSERT_EQUAL_UINT16(16, target->vlanId);
    TEST_ASSERT_EQUAL_UINT8(4, target->vlanPriority);
    TEST_ASSERT_EQUAL_UINT16(8192, target->appId);
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
    /* Fixture text is "10"/"2000" - see this file's own top comment on hex parsing. */
    TEST_ASSERT_EQUAL_UINT16(16, gcb->address->vlanId);
    TEST_ASSERT_EQUAL_UINT16(8192, gcb->address->appId);
    TEST_ASSERT_EQUAL_UINT8(0x01, gcb->address->dstAddress[0]);
    TEST_ASSERT_EQUAL_UINT8(0x05, gcb->address->dstAddress[5]);

    /* MinTime/MaxTime live under the same <GSE> as <Address> - fixture has
     * <MinTime>10</MinTime><MaxTime>5000</MaxTime>, previously silently
     * discarded (always -1,-1) regardless of file content. */
    TEST_ASSERT_EQUAL_INT(10, gcb->minTime);
    TEST_ASSERT_EQUAL_INT(5000, gcb->maxTime);

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

/*
 * ---- hardening_edge_cases.cid: real-world SCL variation this loader now
 * tolerates correctly (hex VLAN/APPID, <SDI>-wrapped overrides, non-numeric
 * enum overrides, LDevice/@ldName functional naming) - see fixtures/
 * hardening_edge_cases.cid's own comments for the exact shape.
 */
#define HARDENING_FIXTURE_PATH "fixtures/hardening_edge_cases.cid"

void
test_hardening_hexAppidVlanId_parsedCorrectly(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(HARDENING_FIXTURE_PATH, "HardeningIED",
            IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    GSEControlBlock* gcb = handle->model->gseCBs;
    TEST_ASSERT_NOT_NULL(gcb);
    TEST_ASSERT_NOT_NULL_MESSAGE(gcb->address, "expected PhyComAddress resolved from <Communication>");
    /* Fixture text is "0012"/"0040" - these specifically exercise the octal-
     * autodetection failure mode (old buggy code: strtoul(text, NULL, 0) on
     * a leading-zero string parses as octal) as well as the general
     * decimal-vs-hex issue - correct hex parsing gives 0x12=18 / 0x40=64. */
    TEST_ASSERT_EQUAL_UINT16(18, gcb->address->vlanId);
    TEST_ASSERT_EQUAL_UINT16(64, gcb->address->appId);

    IedModel_release(handle);
}

void
test_hardening_sdiWrappedOverride_appliesNestedValue(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(HARDENING_FIXTURE_PATH, "HardeningIED",
            IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    LogicalDevice* ld = IedModel_getDeviceByInst(handle->model, "ED1");
    TEST_ASSERT_NOT_NULL(ld);
    LogicalNode* ln0 = LogicalDevice_getLogicalNode(ld, "LLN0");
    TEST_ASSERT_NOT_NULL(ln0);

    ModelNode* phs = ModelNode_getChild((ModelNode*) ln0, "Phs");
    TEST_ASSERT_NOT_NULL_MESSAGE(phs, "expected Phs DO to exist");
    ModelNode* phsA = ModelNode_getChild(phs, "phsA");
    TEST_ASSERT_NOT_NULL_MESSAGE(phsA, "expected phsA SDO under Phs");
    ModelNode* db = ModelNode_getChild(phsA, "db");
    TEST_ASSERT_NOT_NULL_MESSAGE(db, "expected db DA under phsA");

    DataAttribute* dbAttr = (DataAttribute*) db;
    TEST_ASSERT_NOT_NULL_MESSAGE(dbAttr->mmsValue, "expected the <SDI><DAI><Val> override to have applied");
    TEST_ASSERT_EQUAL_INT32(2000, MmsValue_toInt32(dbAttr->mmsValue));

    IedModel_release(handle);
}

void
test_hardening_nonNumericEnumOverride_resolvesRealOrdinal(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(HARDENING_FIXTURE_PATH, "HardeningIED",
            IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    LogicalDevice* ld = IedModel_getDeviceByInst(handle->model, "ED1");
    LogicalNode* ln0 = LogicalDevice_getLogicalNode(ld, "LLN0");
    ModelNode* mod = ModelNode_getChild((ModelNode*) ln0, "Mod");
    TEST_ASSERT_NOT_NULL(mod);
    ModelNode* ctlModel = ModelNode_getChild(mod, "ctlModel");
    TEST_ASSERT_NOT_NULL(ctlModel);

    DataAttribute* ctlModelAttr = (DataAttribute*) ctlModel;
    TEST_ASSERT_NOT_NULL_MESSAGE(ctlModelAttr->mmsValue,
            "expected the non-numeric enum Val override to have applied");
    /* "sbo-with-enhanced-security" must resolve to its real EnumVal ord (4),
     * not atoi("sbo-with-enhanced-security") == 0 (the old, silently-wrong
     * behavior - 0 is itself a valid ordinal, so this was corruption, not a
     * harmless no-op). */
    TEST_ASSERT_EQUAL_INT32(4, MmsValue_toInt32(ctlModelAttr->mmsValue));

    IedModel_release(handle);
}

void
test_hardening_ldNameFunctionalNaming_resolvesFcdaAndRoundTrips(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(HARDENING_FIXTURE_PATH, "HardeningIED",
            IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL(handle);

    LogicalDevice* ld = IedModel_getDeviceByInst(handle->model, "ED1");
    TEST_ASSERT_NOT_NULL(ld);
    TEST_ASSERT_EQUAL_STRING("HardeningIEDFuncBay", ld->ldName);

    /* The fixture's <FCDA> references this LDevice via ldName
     * ("HardeningIEDFuncBay"), not its bare @inst ("ED1") or the
     * IED-prefixed wire form - only the third resolution convention can
     * succeed here. A non-empty member list proves it actually resolved
     * (an unresolved FCDA is skipped with a WARN, leaving the dataset
     * empty). Note object references are built from ldName when set (not
     * IEDName+inst), confirmed directly against this fixture. */
    LinkedList members = IedModel_getDataSetMemberReferences(handle, "HardeningIEDFuncBay/LLN0$ds1");
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(members));

    LinkedList_destroyDeep(members, free);
    IedModel_release(handle);
}

/*
 * ---- private_only.icd: a vendor Private-encoded-control-block situation
 * (see this feature's own Architecture bullet in CLAUDE.md) - proves this
 * is detected (via stderr diagnostic, not asserted here) without crashing
 * or erroring the whole load, and correctly yields empty target lists
 * rather than fabricating anything from the Private payload.
 */
void
test_privateOnly_loadsSuccessfully_withEmptyTargets(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile("fixtures/private_only.icd", "PrivateOnlyIED",
            IED_MODEL_ACCESS_REPORT_ONLY, &error);

    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected private_only.icd to still load successfully");
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

    LinkedList report = IedModel_getReportSubscriptionTargets(handle);
    LinkedList goose = IedModel_getGooseSubscriptionTargets(handle);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(report));
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(goose));

    LinkedList_destroyDeep(report, IedModel_destroyReportControlBlockTarget);
    LinkedList_destroyDeep(goose, IedModel_destroyGooseSubscriptionTarget);
    IedModel_release(handle);
}

/*
 * ---- leading_comment.icd: a top-level <!--comment--> before <SCL> (real,
 * confirmed against an ABB-exported SCD) used to be silently mistaken for
 * the SCL root itself, since this vendored Mini-XML represents comments as
 * MXML_ELEMENT nodes too (no distinct comment type) - see loadSclRoot's own
 * doc comment. Every element name/name-attribute here is otherwise
 * completely ordinary; the only thing under test is that the leading
 * comment doesn't derail root resolution.
 */
void
test_leadingComment_doesNotDerailSclRootResolution(void) {
    IedModelLoadError error;
    LinkedList names = IedModel_listIedNames("fixtures/leading_comment.icd", &error);
    TEST_ASSERT_NOT_NULL(names);
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(names));
    TEST_ASSERT_EQUAL_STRING("LeadingCommentIED", (char*) LinkedList_getData(LinkedList_getNext(names)));
    LinkedList_destroyDeep(names, free);

    IedModelHandle handle = IedModel_loadFromFile("fixtures/leading_comment.icd", "LeadingCommentIED",
            IED_MODEL_ACCESS_REPORT_ONLY, &error);
    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected a leading top-level comment to not break SCL root resolution");
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

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

    RUN_TEST(test_hardening_hexAppidVlanId_parsedCorrectly);
    RUN_TEST(test_hardening_sdiWrappedOverride_appliesNestedValue);
    RUN_TEST(test_hardening_nonNumericEnumOverride_resolvesRealOrdinal);
    RUN_TEST(test_hardening_ldNameFunctionalNaming_resolvesFcdaAndRoundTrips);
    RUN_TEST(test_privateOnly_loadsSuccessfully_withEmptyTargets);
    RUN_TEST(test_leadingComment_doesNotDerailSclRootResolution);

    return UNITY_END();
}
