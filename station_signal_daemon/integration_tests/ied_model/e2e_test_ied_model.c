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
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_READ_AND_WRITE, IED_MODEL_LN_CATEGORY_ALL, &error);

    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected breaker1.cid to load successfully");
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

    IedModel_release(handle);
}

void
test_gooseSubscriptionTarget_resolvesCorrectReference(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_READ_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_READ_AND_WRITE, IED_MODEL_LN_CATEGORY_ALL, &error);
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
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
    IedModelHandle handle = IedModel_loadFromFile(FIXTURE_PATH, "Breaker1", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
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
 * (see this feature's own Architecture bullet in CLAUDE.md) - proves the
 * escaped <ReportControl> payload is unescaped, parsed, and resolved to a
 * real report target (runtime name taken from rptID's suffix, same as a
 * literal SCL element would get via resolveRcbRuntimeName), not just
 * detected-and-ignored.
 */
void
test_privateOnly_parsesEscapedReportControl(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile("fixtures/private_only.icd", "PrivateOnlyIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);

    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected private_only.icd to still load successfully");
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

    LinkedList report = IedModel_getReportSubscriptionTargets(handle);
    LinkedList goose = IedModel_getGooseSubscriptionTargets(handle);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(report));
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(goose));

    ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(LinkedList_getNext(report));
    TEST_ASSERT_EQUAL_STRING("PrivateOnlyIEDED1/LLN0.RP.urcbA01", target->objectReference);
    TEST_ASSERT_FALSE(target->buffered);
    TEST_ASSERT_NULL(target->datasetReference);

    LinkedList_destroyDeep(report, IedModel_destroyReportControlBlockTarget);
    LinkedList_destroyDeep(goose, IedModel_destroyGooseSubscriptionTarget);
    IedModel_release(handle);
}

/*
 * ---- private_control_block_storage_malformed.icd: two genuinely broken
 * Private ControlBlockStorage payloads (no "<key>|<xml>" separator at all;
 * a separator followed by unparseable XML) under one LN - proves the loader
 * still loads successfully with zero report targets and doesn't crash,
 * preserving the same graceful-degradation guarantee the vendor pattern
 * always had, now that real payloads are actually parsed.
 */
void
test_privateControlBlockStorage_malformedPayloads_loadSuccessfullyWithNoTargets(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile("fixtures/private_control_block_storage_malformed.icd",
            "MalformedPrivateIED", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);

    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected malformed Private payloads to still load successfully");
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

    LinkedList report = IedModel_getReportSubscriptionTargets(handle);
    TEST_ASSERT_EQUAL_INT(0, LinkedList_size(report));

    LinkedList_destroyDeep(report, IedModel_destroyReportControlBlockTarget);
    IedModel_release(handle);
}

/*
 * ---- private_control_block_storage_multi.icd: mirrors the real Siemens
 * SIPROTEC station export shape - several predefined RCBs (urcbA/urcbB
 * unbuffered, brcbA buffered) all parented under one LN, each its own
 * escaped Private entry. Proves buildReportControls' loop discovers every
 * one, not just a single entry.
 */
void
test_privateControlBlockStorage_multipleEntries_allDiscovered(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile("fixtures/private_control_block_storage_multi.icd",
            "MultiPrivateIED", IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);

    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected private_control_block_storage_multi.icd to load successfully");
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

    LinkedList report = IedModel_getReportSubscriptionTargets(handle);
    TEST_ASSERT_EQUAL_INT(3, LinkedList_size(report));

    bool sawUrcbA = false, sawUrcbB = false, sawBrcbA = false;
    LinkedList element = LinkedList_getNext(report);
    while (element) {
        ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);
        if (strcmp(target->objectReference, "MultiPrivateIEDED1/LLN0.RP.urcbA01") == 0) {
            sawUrcbA = true;
            TEST_ASSERT_FALSE(target->buffered);
        } else if (strcmp(target->objectReference, "MultiPrivateIEDED1/LLN0.RP.urcbB01") == 0) {
            sawUrcbB = true;
            TEST_ASSERT_FALSE(target->buffered);
        } else if (strcmp(target->objectReference, "MultiPrivateIEDED1/LLN0.BR.brcbA01") == 0) {
            sawBrcbA = true;
            TEST_ASSERT_TRUE(target->buffered);
        }
        element = LinkedList_getNext(element);
    }
    TEST_ASSERT_TRUE_MESSAGE(sawUrcbA, "expected urcbA01 among discovered report targets");
    TEST_ASSERT_TRUE_MESSAGE(sawUrcbB, "expected urcbB01 among discovered report targets");
    TEST_ASSERT_TRUE_MESSAGE(sawBrcbA, "expected brcbA01 among discovered report targets");

    LinkedList_destroyDeep(report, IedModel_destroyReportControlBlockTarget);
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
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected a leading top-level comment to not break SCL root resolution");
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);

    IedModel_release(handle);
}

/*
 * ---- services_dyn_dataset.icd: <Services><DynDataSet max="15" maxAttributes="60"/></Services>
 * as a direct child of <IED> (a sibling of <AccessPoint>, not nested under
 * Server) - the real shape confirmed against IEC61850-Station.scd (a real
 * Siemens SIPROTEC 6MD SCD). Proves the parser reads both attributes off a
 * genuinely present <DynDataSet> element.
 */
void
test_services_dynDataSet_parsesMaxAndMaxAttributes(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile("fixtures/services_dyn_dataset.icd", "ServicesDynDatasetIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);

    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);
    TEST_ASSERT_EQUAL_INT(15, IedModel_getDynDataSetMax(handle));
    TEST_ASSERT_EQUAL_INT(60, IedModel_getDynDataSetMaxAttributes(handle));

    IedModel_release(handle);
}

/*
 * ---- services_empty.icd: a self-closing <Services/> with no <DynDataSet>
 * child at all - a real, schema-legal shape (some IEDs in
 * IEC61850-Station.scd, e.g. "Clock", have exactly this). Both values must
 * report -1 ("not declared"), not 0 or garbage.
 */
void
test_services_selfClosingEmpty_reportsUnknown(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile("fixtures/services_empty.icd", "ServicesEmptyIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);

    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);
    TEST_ASSERT_EQUAL_INT(-1, IedModel_getDynDataSetMax(handle));
    TEST_ASSERT_EQUAL_INT(-1, IedModel_getDynDataSetMaxAttributes(handle));

    IedModel_release(handle);
}

/*
 * ---- private_only.icd has no <Services> element at all (predates this
 * feature) - reuses that existing fixture rather than adding a new one, to
 * prove the "absent <Services>" case (as opposed to "present but empty",
 * covered above) also reports -1/-1.
 */
void
test_services_absent_reportsUnknown(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile("fixtures/private_only.icd", "PrivateOnlyIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);

    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL(IED_MODEL_OK, error);
    TEST_ASSERT_EQUAL_INT(-1, IedModel_getDynDataSetMax(handle));
    TEST_ASSERT_EQUAL_INT(-1, IedModel_getDynDataSetMaxAttributes(handle));

    IedModel_release(handle);
}

/* ---- categoryFilter (real SCL lnClass/desc parsing - see fixtures/category_filter.cid) ----
 *
 * Fixture shape: IED "CatFilterIED", LDevice "CB1": LLN0 (OTHER, hosts
 * buffered RCB "brcbMain" + GoCB "gcbStatus", both on dataset "ds1" spanning
 * all four LNs) + XCBR1 (CONTROL, also hosts its OWN unbuffered RCB
 * "urcbXcbr" on a private dataset "dsXcbr") + MMXU1 (MEASUREMENT) + PTOC1
 * (PROTECTION). XCBR1's Pos.stVal carries desc="Circuit breaker position",
 * PTOC1's Str.general carries desc="Protection general start" - both at the
 * DA-template level, proving the real mxml attribute-reading path (not just
 * the dynamic-model unit tests, which never touch SCL parsing at all). */

#define CATEGORY_FIXTURE_PATH "fixtures/category_filter.cid"

static int
countLinkedListMatching(LinkedList list, bool (*predicate)(const char*)) {
    int count = 0;
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        if (predicate((const char*) LinkedList_getData(element))) count++;
        element = LinkedList_getNext(element);
    }
    return count;
}

static bool
alwaysTrue(const char* ref) {
    (void) ref;
    return true;
}

/* RCB/GoCB visibility is deliberately NOT gated by categoryFilter - the
 * daemon always needs every RCB/GoCB visible to know where every dataset
 * lives, regardless of the category filter in effect. Category filtering
 * happens per-point instead, downstream in mms_report_client/goose_subscriber
 * - see their own integration suites for that proof. */
void
test_categoryFilter_reportTargets_alwaysIncludedRegardlessOfParentLn(void) {
    IedModelLoadError error;

    /* Unfiltered: both brcbMain (parent LLN0) and urcbXcbr (parent XCBR1). */
    IedModelHandle allHandle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
    TEST_ASSERT_NOT_NULL(allHandle);
    LinkedList allTargets = IedModel_getReportSubscriptionTargets(allHandle);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(allTargets));
    LinkedList_destroyDeep(allTargets, IedModel_destroyReportControlBlockTarget);
    IedModel_release(allHandle);

    /* CONTROL-only: both RCBs still present, including brcbMain (parent LLN0,
     * category OTHER) - a narrow filter must never hide it, since the daemon
     * still needs to know its dataset exists regardless of the active filter. */
    IedModelHandle controlHandle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_CONTROL, &error);
    TEST_ASSERT_NOT_NULL(controlHandle);
    LinkedList controlTargets = IedModel_getReportSubscriptionTargets(controlHandle);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(controlTargets));
    LinkedList_destroyDeep(controlTargets, IedModel_destroyReportControlBlockTarget);
    IedModel_release(controlHandle);

    /* MEASUREMENT-only: still both RCBs, even though neither RCB's own
     * parent LN (LLN0/XCBR1) is MEASUREMENT. */
    IedModelHandle measurementHandle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_MEASUREMENT, &error);
    TEST_ASSERT_NOT_NULL(measurementHandle);
    LinkedList measurementTargets = IedModel_getReportSubscriptionTargets(measurementHandle);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(measurementTargets));
    LinkedList_destroyDeep(measurementTargets, IedModel_destroyReportControlBlockTarget);
    IedModel_release(measurementHandle);
}

void
test_categoryFilter_gooseTargets_alwaysIncludedRegardlessOfParentLn(void) {
    IedModelLoadError error;

    /* gcbStatus is parented on LLN0 (group L -> OTHER) - still present under
     * a CONTROL-only filter. */
    IedModelHandle controlHandle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_CONTROL, &error);
    TEST_ASSERT_NOT_NULL(controlHandle);
    LinkedList controlTargets = IedModel_getGooseSubscriptionTargets(controlHandle);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(controlTargets));
    LinkedList_destroyDeep(controlTargets, IedModel_destroyGooseSubscriptionTarget);
    IedModel_release(controlHandle);

    IedModelHandle otherHandle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_OTHER, &error);
    TEST_ASSERT_NOT_NULL(otherHandle);
    LinkedList otherTargets = IedModel_getGooseSubscriptionTargets(otherHandle);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(otherTargets));
    LinkedList_destroyDeep(otherTargets, IedModel_destroyGooseSubscriptionTarget);
    IedModel_release(otherHandle);
}

void
test_categoryFilter_wholeDeviceReportableAttributes_filteredByEachLeafsOwnLn(void) {
    IedModelLoadError error;
    IedModelHandle allHandle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
    TEST_ASSERT_NOT_NULL(allHandle);
    LinkedList allLeaves = IedModel_getReportableAttributeReferencesForWholeDevice(allHandle);
    int allCount = countLinkedListMatching(allLeaves, alwaysTrue);
    TEST_ASSERT_TRUE_MESSAGE(allCount > 0, "expected at least one reportable leaf across the whole device");
    LinkedList_destroyDeep(allLeaves, free);
    IedModel_release(allHandle);

    /* CONTROL-only must include XCBR1's own leaves and exclude every other LN's. */
    IedModelHandle controlHandle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_CONTROL, &error);
    TEST_ASSERT_NOT_NULL(controlHandle);
    LinkedList controlLeaves = IedModel_getReportableAttributeReferencesForWholeDevice(controlHandle);
    int controlCount = LinkedList_size(controlLeaves);
    TEST_ASSERT_TRUE_MESSAGE(controlCount > 0, "expected XCBR1's own leaves to survive a CONTROL-only filter");
    TEST_ASSERT_TRUE_MESSAGE(controlCount < allCount,
            "a CONTROL-only filter must exclude at least the MEASUREMENT/PROTECTION/OTHER leaves");

    bool foundNonXcbrLeaf = false;
    LinkedList element = LinkedList_getNext(controlLeaves);
    while (element) {
        const char* ref = (const char*) LinkedList_getData(element);
        if (!strstr(ref, "/XCBR1")) foundNonXcbrLeaf = true;
        element = LinkedList_getNext(element);
    }
    TEST_ASSERT_FALSE_MESSAGE(foundNonXcbrLeaf, "a CONTROL-only filter must exclude every non-XCBR1 leaf");
    LinkedList_destroyDeep(controlLeaves, free);
    IedModel_release(controlHandle);

    /* PROTECTION-only must include only PTOC1's own leaves. */
    IedModelHandle protectionHandle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_PROTECTION, &error);
    TEST_ASSERT_NOT_NULL(protectionHandle);
    LinkedList protectionLeaves = IedModel_getReportableAttributeReferencesForWholeDevice(protectionHandle);
    TEST_ASSERT_TRUE(LinkedList_size(protectionLeaves) > 0);
    bool foundNonPtocLeaf = false;
    element = LinkedList_getNext(protectionLeaves);
    while (element) {
        const char* ref = (const char*) LinkedList_getData(element);
        if (!strstr(ref, "/PTOC1")) foundNonPtocLeaf = true;
        element = LinkedList_getNext(element);
    }
    TEST_ASSERT_FALSE_MESSAGE(foundNonPtocLeaf, "a PROTECTION-only filter must exclude every non-PTOC1 leaf");
    LinkedList_destroyDeep(protectionLeaves, free);
    IedModel_release(protectionHandle);
}

void
test_categoryFilter_descCapturedFromRealScl_bothTemplateLevelLeaves(void) {
    IedModelLoadError error;
    IedModelHandle handle = IedModel_loadFromFile(CATEGORY_FIXTURE_PATH, "CatFilterIED",
            IED_MODEL_ACCESS_REPORT_ONLY, IED_MODEL_LN_CATEGORY_ALL, &error);
    TEST_ASSERT_NOT_NULL(handle);

    TEST_ASSERT_EQUAL_STRING("Circuit breaker position",
            IedModel_getDescriptionForMemberReference(handle, "CatFilterIEDCB1/XCBR1$ST$Pos$stVal"));
    TEST_ASSERT_EQUAL_STRING("Protection general start",
            IedModel_getDescriptionForMemberReference(handle, "CatFilterIEDCB1/PTOC1$ST$Str$general"));
    /* MMXU1's TotW$mag$f leaf carries no desc anywhere in the fixture. */
    TEST_ASSERT_NULL(IedModel_getDescriptionForMemberReference(handle, "CatFilterIEDCB1/MMXU1$MX$TotW$mag$f"));

    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_CONTROL,
            IedModel_getCategoryForMemberReference(handle, "CatFilterIEDCB1/XCBR1$ST$Pos$stVal"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_MEASUREMENT,
            IedModel_getCategoryForMemberReference(handle, "CatFilterIEDCB1/MMXU1$MX$TotW$mag$f"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_PROTECTION,
            IedModel_getCategoryForMemberReference(handle, "CatFilterIEDCB1/PTOC1$ST$Str$general"));
    TEST_ASSERT_EQUAL(IED_MODEL_LN_CATEGORY_OTHER,
            IedModel_getCategoryForMemberReference(handle, "CatFilterIEDCB1/LLN0$ST$Mod$stVal"));

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
    RUN_TEST(test_privateOnly_parsesEscapedReportControl);
    RUN_TEST(test_privateControlBlockStorage_malformedPayloads_loadSuccessfullyWithNoTargets);
    RUN_TEST(test_privateControlBlockStorage_multipleEntries_allDiscovered);
    RUN_TEST(test_leadingComment_doesNotDerailSclRootResolution);

    RUN_TEST(test_services_dynDataSet_parsesMaxAndMaxAttributes);
    RUN_TEST(test_services_selfClosingEmpty_reportsUnknown);
    RUN_TEST(test_services_absent_reportsUnknown);

    RUN_TEST(test_categoryFilter_reportTargets_alwaysIncludedRegardlessOfParentLn);
    RUN_TEST(test_categoryFilter_gooseTargets_alwaysIncludedRegardlessOfParentLn);
    RUN_TEST(test_categoryFilter_wholeDeviceReportableAttributes_filteredByEachLeafsOwnLn);
    RUN_TEST(test_categoryFilter_descCapturedFromRealScl_bothTemplateLevelLeaves);

    return UNITY_END();
}
