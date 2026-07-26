#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/scl_bootstrap/service/scl_bootstrap_api.h"
#include "features/ied_model_online_loader/service/ied_model_online_loader_api.h"
#include "hal_thread.h"
#include "sim_types.h"

/*
 * End-to-end test: runs a real "Reporter1" IED simulator (sim_types.h /
 * sim_server.c - see integration_tests/ied_simulator/, fully decoupled from
 * src/) in the same process, with its MMS file services pointed at a real,
 * empty fixture directory (fixtures/no_scl_files/, mirroring scl_bootstrap's
 * own identical fixture/precedent) - reproducing the exact real-world
 * scenario this feature exists for: a real, connectable MMS/GOOSE device
 * (confirmed in practice against a real OMICRON IED Scout "Simulate IED"
 * instance) that associates fine, browses fine, but has no SCL file to serve
 * at all (SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND) - NOT a device whose
 * file service is unimplemented/erroring outright (empirically confirmed to
 * be a DIFFERENT scl_bootstrap outcome, SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED,
 * during this test's own development - not what a real IED Scout instance
 * produces, so not what this fixture should simulate).
 *
 * Two things are proven here:
 *   1. The precondition this whole feature exists for is real, not assumed:
 *      scl_bootstrap genuinely cannot fetch an SCL from this server.
 *   2. ied_model_online_loader can still build a fully usable IedModelHandle
 *      directly from the same live server's MMS data model, and every
 *      accessor on it (getReportSubscriptionTargets, getGooseSubscriptionTargets,
 *      getDataSetMemberReferences) matches the exact shape sim_server.c's own
 *      SimServer_create hand-builds - proving parity with what an
 *      SCL-parsed model of the same device would have produced.
 *
 * Note sim_server.c's RCBs (brcbMain included) are all created with a NULL
 * dataSetName at the server-model level by design - normally it's the SCL
 * fixture (parsed client-side) that tells a real client which dataset to
 * explicitly assign at enable time (see mms_report_client's own comment on
 * why it always asserts DATSET itself rather than trusting a server
 * default). Online discovery has no such external SCL to consult - it only
 * ever sees what the live server's RCB actually has *right now*, which for
 * every RCB in this simulator is genuinely nothing, same as urcbDyn already
 * demonstrates for the SCL-parsing path. Only the GoCB (gcbInd) has a real,
 * statically-configured dataset (GOOSE publishing has no external "client
 * enables me" step), so dataset-member resolution is proven through it
 * instead of through an RCB.
 *
 * No sudo needed - MMS/TCP only, same as scl_bootstrap's own E2E test.
 */

#define TEST_PORT 10501
#define LIVE_HOST "127.0.0.1"

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

static ReportControlBlockTarget*
findReportTarget(LinkedList targets, const char* objectReferenceSuffix) {
    LinkedList element = LinkedList_getNext(targets);
    while (element) {
        ReportControlBlockTarget* target = (ReportControlBlockTarget*) LinkedList_getData(element);
        if (target->objectReference && strstr(target->objectReference, objectReferenceSuffix)) return target;
        element = LinkedList_getNext(element);
    }
    return NULL;
}

static GooseSubscriptionTarget*
findGooseTarget(LinkedList targets, const char* objectReferenceSuffix) {
    LinkedList element = LinkedList_getNext(targets);
    while (element) {
        GooseSubscriptionTarget* target = (GooseSubscriptionTarget*) LinkedList_getData(element);
        if (target->objectReference && strstr(target->objectReference, objectReferenceSuffix)) return target;
        element = LinkedList_getNext(element);
    }
    return NULL;
}

static bool
listContainsStringSuffix(LinkedList list, const char* suffix) {
    LinkedList element = LinkedList_getNext(list);
    while (element) {
        char* value = (char*) LinkedList_getData(element);
        if (value && strstr(value, suffix)) return true;
        element = LinkedList_getNext(element);
    }
    return false;
}

void
test_fileServicesDisabled_scl_bootstrapReportsNoSclFileFound(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/no_scl_files/");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    LinkedList hosts = makeHostList(LIVE_HOST);

    SclBootstrapError err;
    SclBootstrapHandle handle = SclBootstrap_create(NULL, &err);
    TEST_ASSERT_NOT_NULL(handle);

    LinkedList results = SclBootstrap_scanAndFetch(handle, hosts, TEST_PORT, &err);
    TEST_ASSERT_NOT_NULL(results);
    TEST_ASSERT_EQUAL_INT(1, LinkedList_size(results));

    LinkedList element = LinkedList_getNext(results);
    SclBootstrapResult* result = (SclBootstrapResult*) LinkedList_getData(element);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL(SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND, result->status);

    LinkedList_destroyDeep(results, SclBootstrap_destroyResult);
    SclBootstrap_destroy(handle);
    LinkedList_destroyStatic(hosts);

    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_onlineDiscovery_buildsReportTargets_matchingSimServerShape(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/no_scl_files/");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    IedModelOnlineLoaderError err;
    IedModelHandle handle = IedModelOnlineLoader_build(LIVE_HOST, TEST_PORT, "Reporter1",
            IED_MODEL_ACCESS_REPORT_ONLY, NULL, NULL, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(handle, "expected online discovery to build a model against a live, "
            "associable server with no SCL file at all");

    LinkedList reportTargets = IedModel_getReportSubscriptionTargets(handle);
    TEST_ASSERT_NOT_NULL(reportTargets);

    /* brcbMain's dataSetName is NULL at the server-model level by design
     * (see this file's own top comment) - discovery must faithfully report
     * that as NULL, not fabricate a value, exactly like urcbDyn already
     * demonstrates for the SCL-parsing path. */
    ReportControlBlockTarget* brcbMain = findReportTarget(reportTargets, "LLN0.BR.brcbMain");
    TEST_ASSERT_NOT_NULL_MESSAGE(brcbMain, "expected discovered brcbMain (buffered RCB on LLN0)");
    TEST_ASSERT_TRUE(brcbMain->buffered);
    TEST_ASSERT_NULL_MESSAGE(brcbMain->datasetReference, "brcbMain has no live dataSetName on the server "
            "until a client explicitly assigns one - discovery must not fabricate one either");

    ReportControlBlockTarget* urcbDyn = findReportTarget(reportTargets, "GGIO1.RP.urcbDyn");
    TEST_ASSERT_NOT_NULL_MESSAGE(urcbDyn, "expected discovered urcbDyn (unbuffered RCB, parented under "
            "GGIO1 not LLN0)");
    TEST_ASSERT_FALSE(urcbDyn->buffered);
    TEST_ASSERT_NULL(urcbDyn->datasetReference);

    LinkedList_destroyDeep(reportTargets, IedModel_destroyReportControlBlockTarget);
    IedModel_release(handle);

    SimServer_stop(sim);
    SimServer_destroy(sim);
}

void
test_onlineDiscovery_buildsGooseTargets_matchingSimServerShape(void) {
    SimServer sim = SimServer_create();
    SimServer_setFilestoreBasepath(sim, "fixtures/no_scl_files/");
    SimServer_start(sim, TEST_PORT);
    Thread_sleep(200);

    IedModelOnlineLoaderError err;
    IedModelHandle handle = IedModelOnlineLoader_build(LIVE_HOST, TEST_PORT, "Reporter1",
            IED_MODEL_ACCESS_REPORT_ONLY, NULL, NULL, &err);
    TEST_ASSERT_NOT_NULL(handle);

    LinkedList gooseTargets = IedModel_getGooseSubscriptionTargets(handle);
    TEST_ASSERT_NOT_NULL(gooseTargets);

    /* Unlike brcbMain, gcbInd's dataset ("ds1") IS statically configured on
     * the live server at creation time (GOOSE publishing has no external
     * "client enables me and assigns a dataset" step the way reporting
     * does) - so this is where dataset-member resolution (live
     * GetDataSetDirectory -> IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef)
     * gets proven end-to-end. */
    GooseSubscriptionTarget* gcbInd = findGooseTarget(gooseTargets, "LLN0$GO$gcbInd");
    TEST_ASSERT_NOT_NULL_MESSAGE(gcbInd, "expected discovered gcbInd GoCB");
    TEST_ASSERT_NOT_NULL(gcbInd->datasetReference);
    TEST_ASSERT_TRUE(strstr(gcbInd->datasetReference, "LLN0$ds1") != NULL);

    /* sim_server.c's gcbInd has a real DstAddress (appId=0x1000, vlanId=10,
     * dstMac=01-0c-cd-01-00-01) - see that file's own comment. This is the
     * one flagged-as-unverified heuristic (all-zero == "not populated") -
     * proving it against this real, compliant libiec61850 server at least
     * confirms the mechanism works when a server DOES populate it, even
     * though a real vendor device's behavior here still needs separate
     * manual verification (see CLAUDE.md's own note on this). */
    TEST_ASSERT_TRUE_MESSAGE(gcbInd->hasAddress, "expected GoCB address to be discovered via getGoCBValues");
    TEST_ASSERT_EQUAL_HEX16(0x1000, gcbInd->appId);
    TEST_ASSERT_EQUAL_UINT16(10, gcbInd->vlanId);
    uint8_t expectedMac[6] = { 0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01 };
    TEST_ASSERT_EQUAL_MEMORY(expectedMac, gcbInd->dstMac, 6);

    LinkedList dsMembers = IedModel_getDataSetMemberReferences(handle, gcbInd->datasetReference);
    TEST_ASSERT_NOT_NULL(dsMembers);
    TEST_ASSERT_EQUAL_INT(2, LinkedList_size(dsMembers));
    TEST_ASSERT_TRUE_MESSAGE(listContainsStringSuffix(dsMembers, "GGIO1$ST$Ind1$stVal"),
            "expected ds1's first member (GGIO1.Ind1.stVal) resolved via live GetDataSetDirectory");
    TEST_ASSERT_TRUE_MESSAGE(listContainsStringSuffix(dsMembers, "GGIO1$ST$Ind1$q"),
            "expected ds1's second member (GGIO1.Ind1.q) resolved via live GetDataSetDirectory");

    LinkedList_destroyDeep(dsMembers, free);
    LinkedList_destroyDeep(gooseTargets, IedModel_destroyGooseSubscriptionTarget);
    IedModel_release(handle);

    SimServer_stop(sim);
    SimServer_destroy(sim);
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_fileServicesDisabled_scl_bootstrapReportsNoSclFileFound);
    RUN_TEST(test_onlineDiscovery_buildsReportTargets_matchingSimServerShape);
    RUN_TEST(test_onlineDiscovery_buildsGooseTargets_matchingSimServerShape);

    return UNITY_END();
}
