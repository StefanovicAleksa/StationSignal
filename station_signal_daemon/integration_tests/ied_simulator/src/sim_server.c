#include <stdlib.h>
#include <string.h>
#include "sim_types.h"
#include "iec61850_dynamic_model.h"

/* Shared model-building code for SimServer_create/_createWithDatasetLimits -
 * identical model either way, only how the IedServer itself is constructed
 * differs (plain IedServer_create vs IedServer_createWithConfig). Returns the
 * built IedModel* and hands back the two DataAttribute* the caller needs to
 * populate its own SimServer handle. */
static IedModel*
buildModel(DataAttribute** outIndicationStVal, DataAttribute** outSpcso1StVal, DataAttribute** outModStVal,
        DataAttribute** outBehStVal) {
    IedModel* model = IedModel_create("Reporter1");
    LogicalDevice* ld = LogicalDevice_create("LD1", model);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    LogicalNode* ggio1 = LogicalNode_create("GGIO1", ld);

    /* LLN0.Mod/Beh/Health (INC1/ENS1-shaped, matching fixtures/reporter1.cid's
     * own LLN0Type declaration) - previously declared in SCL but never
     * actually implemented on the live simulator (LLN0 had zero DataObjects
     * of its own). Harmless while every RCB only ever reported its own
     * parent LN's data, but a real gap once whole-device dynamic-dataset
     * clustering started pulling in LLN0's own leaves too, since a real
     * device is expected to be internally consistent between SCL and what it
     * actually implements - the fixture needs to be too. Mod.stVal/Beh.stVal
     * get TRG_OPT_DATA_CHANGED (unlike Health.stVal, never flipped by any
     * test) so SimServer_setModStVal/_setBehStVal below can drive a real
     * data-change report the same way SimServer_setIndication already does
     * for GGIO1.Ind1.stVal. */
    DataObject* mod = DataObject_create("Mod", (ModelNode*) ln0, 0);
    DataAttribute* modStVal = DataAttribute_create("stVal", (ModelNode*) mod, IEC61850_INT32, IEC61850_FC_ST,
            TRG_OPT_DATA_CHANGED, 0, 0);
    DataAttribute_create("q", (ModelNode*) mod, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("t", (ModelNode*) mod, IEC61850_TIMESTAMP, IEC61850_FC_ST, 0, 0, 0);

    DataObject* beh = DataObject_create("Beh", (ModelNode*) ln0, 0);
    DataAttribute* behStVal = DataAttribute_create("stVal", (ModelNode*) beh, IEC61850_ENUMERATED, IEC61850_FC_ST,
            TRG_OPT_DATA_CHANGED, 0, 0);
    DataAttribute_create("q", (ModelNode*) beh, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("t", (ModelNode*) beh, IEC61850_TIMESTAMP, IEC61850_FC_ST, 0, 0, 0);

    DataObject* health = DataObject_create("Health", (ModelNode*) ln0, 0);
    DataAttribute_create("stVal", (ModelNode*) health, IEC61850_ENUMERATED, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("q", (ModelNode*) health, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute_create("t", (ModelNode*) health, IEC61850_TIMESTAMP, IEC61850_FC_ST, 0, 0, 0);

    /* GGIO1.Ind1 (SPS) - the read/reported indication point flipped by
     * SimServer_setIndication. */
    DataObject* ind1 = DataObject_create("Ind1", (ModelNode*) ggio1, 0);
    DataAttribute* stVal = DataAttribute_create("stVal", (ModelNode*) ind1, IEC61850_BOOLEAN, IEC61850_FC_ST,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED, 0, 0);
    DataAttribute_create("q", (ModelNode*) ind1, IEC61850_QUALITY, IEC61850_FC_ST, TRG_OPT_QUALITY_CHANGED, 0, 0);
    DataAttribute_create("t", (ModelNode*) ind1, IEC61850_TIMESTAMP, IEC61850_FC_ST, 0, 0, 0);

    /* GGIO1.SPCSO1 (SPC) - a controllable point, present only to give this
     * simulated device a realistic read+write-capable shape; not exercised
     * by the report-focused E2E test. */
    DataObject* spcso1 = DataObject_create("SPCSO1", (ModelNode*) ggio1, 0);
    DataAttribute* spcso1StVal = DataAttribute_create("stVal", (ModelNode*) spcso1, IEC61850_BOOLEAN, IEC61850_FC_ST,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED, 0, 0);
    DataAttribute_create("q", (ModelNode*) spcso1, IEC61850_QUALITY, IEC61850_FC_ST, TRG_OPT_QUALITY_CHANGED, 0, 0);
    DataAttribute_create("t", (ModelNode*) spcso1, IEC61850_TIMESTAMP, IEC61850_FC_ST, 0, 0, 0);
    DataAttribute* oper = DataAttribute_create("Oper", (ModelNode*) spcso1, IEC61850_CONSTRUCTED, IEC61850_FC_CO, 0, 0, 0);
    DataAttribute_create("ctlVal", (ModelNode*) oper, IEC61850_BOOLEAN, IEC61850_FC_CO, 0, 0, 0);
    DataAttribute_create("ctlModel", (ModelNode*) spcso1, IEC61850_ENUMERATED, IEC61850_FC_CF, 0, 0, 0);

    /* DataSet_create internally prepends "<lnName>$" to this name
     * (StringUtils_createString(3, parent->name, "$", name) in
     * dynamic_model.c), so passing the bare local name here already produces
     * the externally-matchable "LLN0$ds1" - do not add the "LLN0$" prefix
     * ourselves, that would double it. */
    DataSet* dataSet = DataSet_create("ds1", ln0);
    /* Reference format is "<lnName>$<fc>$<doName>$<daName>" - NO LD-wire-name
     * prefix (confirmed against libiec61850's own server_example_dynamic.c,
     * which uses "TTMP1$MX$TmpSv$instMag$f"). Including one silently makes
     * this entry fail server-side resolution, which in turn fails the whole
     * dataset's access check and RptEna with DATA_ACCESS_ERROR_OBJECT_VALUE_INVALID. */
    DataSetEntry_create(dataSet, "GGIO1$ST$Ind1$stVal", -1, NULL);
    DataSetEntry_create(dataSet, "GGIO1$ST$Ind1$q", -1, NULL);

    /* A second, deliberately narrower dataset (stVal only, no q) - exists so
     * rcbMulti01 (below) can report the same GGIO1.Ind1 flip as brcbMain
     * without mms_report_client's cross-RCB duplicate-content suppression
     * (MmsReportClientUseCases_shouldForwardAcrossRcb) treating its report as
     * an exact-content duplicate of brcbMain's own (which carries both stVal
     * AND q) and silently dropping it - see
     * integration_tests/orchestration_local_file's own fixture and E2E test
     * comment for the full real-world story (a live ABB REC650's RptEnabled
     * max>1 RCB-suffix handling). */
    DataSet* dataSetStValOnly = DataSet_create("ds2", ln0);
    DataSetEntry_create(dataSetStValOnly, "GGIO1$ST$Ind1$stVal", -1, NULL);

    /* dataSetName left NULL (no server-side default dataset) - the client
     * (mms_report_client) always explicitly sets DatSet alongside RptEna on
     * enable, matching libiec61850's own reference client example
     * (client_example_no_thread.c), which is the validated, non-fragile path. */
    /* RPT_OPT_ENTRY_ID - required for mms_report_client's EntryID-resumption
     * E2E coverage (test_secondReconnectWithNoNewChanges_doesNotRedeliverBacklog
     * in e2e_test_mms_report_client.c): without it, the server never includes
     * an EntryID in the report at all, so the client has nothing to resume
     * from on reconnect (mirrors fixtures/reporter1.cid's own
     * OptFields entryID="true", which only governs the CLIENT side - this is
     * the SERVER side of the same setting). */
    ReportControlBlock_create("brcbMain", ln0, "brcbMain", true, NULL, 1,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_TIME_STAMP | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION
                    | RPT_OPT_ENTRY_ID,
            0, 60000);

    /* Reproduces a real-world vendor quirk found against a live ABB REC650
     * exposed via IED Scout: its SCD declares
     * <ReportControl name="rcb_A" ...><RptEnabled max="5">, but the live
     * server only ever answers on the wire to "rcb_A01".."rcb_A05" - never
     * the bare "rcb_A" name (RptEnabled max>1 means the server exposes N
     * separately-addressable, pre-instantiated RCB objects, each with its own
     * "NN" suffix baked into its real name - libiec61850's dynamic model API
     * has no separate "instance count" parameter for this, so the suffix is
     * simply part of the literal name passed here, matching the live
     * server's own actual object). Over ds2 (not ds1) so its report doesn't
     * get suppressed as an exact-content duplicate of brcbMain's own - see
     * ds2's own comment above. Only orchestration_local_file's own fixture
     * (reporter1_rcb_instance_mismatch.cid) declares a matching
     * <ReportControl name="rcbMulti" ...><RptEnabled max="5">, so no other
     * E2E test that links this same sim_server.c ever attempts to enable
     * it. */
    ReportControlBlock_create("rcbMulti01", ln0, "rcbMulti01", true, "ds2", 1,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_TIME_STAMP | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION,
            0, 60000);

    /* A second RCB over the exact same ds1 dataset/TrgOps/OptFields as
     * brcbMain - reproduces a real device pattern (redundant/reserved RCB
     * instances configured per-client on the same LN/dataset, e.g.
     * "urcbA01"/"urcbB01") where two independent RCBs report the identical
     * underlying event at nearly the same moment. Proves mms_report_client's
     * cross-RCB duplicate-content suppression end-to-end. Only
     * mms_report_client's own fixture SCL (reporter1.cid) declares a
     * matching <ReportControl>, so no other E2E test that links this same
     * sim_server.c ever attempts to enable it - same convention as urcbDyn
     * above. */
    ReportControlBlock_create("brcbDup", ln0, "brcbDup", true, NULL, 1,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_TIME_STAMP | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION
                    | RPT_OPT_ENTRY_ID,
            0, 60000);

    /* Parented under ggio1 itself (not ln0) with dataSetName=NULL, mirroring
     * a real device (E13_6MD/IEC 61850v2 JA4 station.scd) where every
     * ReportControl omits datSet and is parented under the LN it reports on -
     * proves mms_report_client's dynamic dataset creation
     * (IedConnection_createDataSet) against a real MMS association. Only
     * mms_report_client's own fixture SCL (reporter1.cid) declares a matching
     * <ReportControl> with no datSet for this RCB, so no other E2E test that
     * links this same sim_server.c ever attempts to enable it. */
    ReportControlBlock_create("urcbDyn", ggio1, "urcbDyn", false, NULL, 1,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION,
            0, 0);

    /* Buffered counterpart of urcbDyn - proves mms_report_client's dynamic
     * dataset creation for a BUFFERED RCB, which needs a domain-scoped (not
     * "@"-prefixed association-scoped) self-created dataset -
     * getOrCreateDynamicDataset's own doc comment
     * (mms_report_client_connection.c) and GAP3_DYNAMIC_DATASET_NOTES.md
     * explain why: an association-scoped dataset doesn't survive the
     * disconnect a buffered RCB exists to buffer through, and the reference
     * server (and a real SIPROTEC 6MD device) reject assigning one to a
     * buffered RCB outright. Only mms_report_client's own fixture SCL
     * (reporter1.cid) declares a matching <ReportControl>, so no other E2E
     * test that links this same sim_server.c ever attempts to enable it -
     * same convention as brcbDup/urcbDyn above. */
    ReportControlBlock_create("brcbDyn", ggio1, "brcbDyn", true, NULL, 1,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION | RPT_OPT_ENTRY_ID,
            0, 0);

    /* A second Dyn (no datSet) RCB on the same LN (GGIO1) as urcbDyn -
     * exists purely so an E2E test can prove mms_report_client's dynamic-
     * dataset chunking (buildChunkPlan/getOrCreateDynamicDataset,
     * mms_report_client_connection.c): with a small enough maxAttributes,
     * GGIO1's full 6-leaf set (Ind1.stVal/q/t + SPCSO1.stVal/q/t) splits into
     * two DO-atomic 3-member chunks, one per spare Dyn target on this LN -
     * urcbDyn gets one, urcbDyn2 gets the other. Only
     * fixtures/reporter1_chunking.cid/reporter1_budget.cid declare a matching
     * <ReportControl name="urcbDyn2">, so no other E2E test that links this
     * same sim_server.c ever attempts to enable it - same convention as
     * brcbDup/urcbDyn above. */
    ReportControlBlock_create("urcbDyn2", ggio1, "urcbDyn2", false, NULL, 1,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION,
            0, 0);

    /* A second BUFFERED Dyn RCB on the same LN (GGIO1) as brcbDyn - exists
     * purely so an E2E test can prove adoptUnclaimedDataset's preferred-
     * own-name pass (mms_report_client_connection.c): two buffered Dyn
     * targets sharing one LD, each with its own pre-existing leftover
     * dataset on the server (standing in for datasets left behind by an
     * earlier, ungracefully-terminated run), in an order that would cause
     * them to be cross-adopted by EACH OTHER without that preference. Only
     * fixtures/reporter1_sibling_buffered.cid declares a matching
     * <ReportControl name="brcbDyn2">, so no other E2E test that links this
     * same sim_server.c ever attempts to enable it - same convention as
     * urcbDyn2/brcbDup above. */
    ReportControlBlock_create("brcbDyn2", ggio1, "brcbDyn2", true, NULL, 1,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION | RPT_OPT_ENTRY_ID,
            0, 0);

    /* Reproduces the real-device finding behind mms_report_client's own
     * proactive TrgOps.dchg/qchg fix (mms_report_client_connection.c,
     * enableOneTarget): a real device (via OMICRON IED Scout's "Simulate
     * IED" feature) was found with an RCB's own TrgOps carrying ONLY General
     * Interrogation - dchg/qchg/dupd/integrity all false - meaning it would
     * NEVER generate a report on an actual data or quality change, only the
     * one-time GI snapshot on enable (itself invisible to the frontend via
     * this feature's own bootstrap-suppression). TrgOps here is
     * DELIBERATELY just TRG_OPT_GI - proves the daemon now proactively ORs
     * dchg/qchg in on enable rather than trusting the device's own (here,
     * deliberately insufficient) configuration.
     *
     * datSet="ds3" deliberately does NOT exist yet at server startup - unlike
     * ds1/ds2 (created once, always present, and therefore visible to EVERY
     * test's own tier-3 adoption scan under this same LD - a real collision
     * discovered when ds3 was first added directly here, silently stealing
     * OTHER tests' expected tier-4 self-create slots). Only
     * test_dynamicDataset_giOnlyRcb_reportsRealChangeAfterTrgOpsFix creates
     * ds3 itself, via its own side-channel connection, before ever starting
     * the daemon client - so ds3 exists ONLY within that one test's own
     * SimServer instance, never affecting any other test. Only
     * mms_report_client's own fixture SCL (reporter1.cid) declares a
     * matching <ReportControl>, so no other E2E test that links this same
     * sim_server.c ever attempts to enable this RCB either - same convention
     * as brcbDup/urcbDyn above. */
    ReportControlBlock_create("urcbGiOnly", ln0, "urcbGiOnly", false, "ds3", 1,
            TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION,
            0, 0);

    /* GSEControlBlock over the same ds1 dataset - lets goose_subscriber's E2E
     * test observe the same GGIO1.Ind1.stVal flip that mms_report_client's
     * E2E test observes via reporting. minTime=10/maxTime=5000 mirror
     * breaker1.cid's <GSE><MinTime>/<MaxTime> convention. Addressing
     * (priority 4, wire APPID 0x1000, multicast dst MAC 01-0c-cd-01-00-01)
     * is mirrored into fixtures/reporter1.cid's <Communication> section -
     * MUST stay numerically identical to what's declared there, since
     * goose_subscriber configures its APPID/dst-MAC filter from the SCL
     * fixture, not from this file.
     *
     * The wire APPID here is 0x1000, NOT decimal 1000 - ied_model_scl_loader.c
     * deliberately parses SCL's <P type="APPID"> as hex (IEC 61850-8-1
     * encodes it that way), so the fixture's "<P type=\"APPID\">1000</P>"
     * resolves to 0x1000 (4096 decimal) on the parsing side. This file used
     * to pass the plain decimal literal 1000 (0x3E8) here instead, a real
     * numeric mismatch that made goose_subscriber's GooseSubscriber_setAppId
     * filter silently reject every real frame (no parse error - the frame is
     * dropped before libiec61850 even attempts to report it as invalid),
     * which surfaced as every GOOSE-liveness E2E test (goose_subscriber,
     * orchestration, orchestration_local_file) hanging until its VALID-wait
     * timeout. Fixed by using 0x1000 here to match.
     *
     * VLAN-ID has the same hex/decimal parsing on the SCL side, but is
     * harmless: GooseSubscriberConnection_create only ever applies
     * setDstMac/setAppId as subscriber-side filters (see
     * data/goose_subscriber_connection.c) - vlanId/vlanPriority are parsed
     * and stored on GooseSubscriptionTarget but never used to filter
     * incoming frames, so no corresponding fixture/wire-value drift there
     * actually affects delivery. */
    GSEControlBlock* gcbInd = GSEControlBlock_create("gcbInd", ln0, "1000", "ds1", 1, false, 10, 5000);
    uint8_t gooseDstMac[6] = { 0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01 };
    GSEControlBlock_addPhyComAddress(gcbInd, PhyComAddress_create(4, 10, 0x1000, gooseDstMac));

    /* A second GoCB over the exact same ds1 dataset as gcbInd, with its own
     * distinct APPID (0x1001)/dst MAC (...-00-02) - reproduces the same
     * redundant-publisher pattern as mms_report_client's brcbDup, for
     * goose_subscriber's cross-target duplicate-content suppression E2E
     * test. Only goose_subscriber's own fixture SCL declares a matching
     * <GSE>/<GSEControl>, so no other E2E test that links this same
     * sim_server.c ever attempts to subscribe to it - same convention as
     * brcbDup/urcbDyn above. */
    GSEControlBlock* gcbDup = GSEControlBlock_create("gcbDup", ln0, "1001", "ds1", 1, false, 10, 5000);
    uint8_t gooseDupDstMac[6] = { 0x01, 0x0c, 0xcd, 0x01, 0x00, 0x02 };
    GSEControlBlock_addPhyComAddress(gcbDup, PhyComAddress_create(4, 10, 0x1001, gooseDupDstMac));

    *outIndicationStVal = stVal;
    *outSpcso1StVal = spcso1StVal;
    *outModStVal = modStVal;
    *outBehStVal = behStVal;
    return model;
}

SimServer
SimServer_create(void) {
    DataAttribute* indicationStVal;
    DataAttribute* spcso1StVal;
    DataAttribute* modStVal;
    DataAttribute* behStVal;
    IedModel* model = buildModel(&indicationStVal, &spcso1StVal, &modStVal, &behStVal);

    SimServer self = calloc(1, sizeof(struct sSimServer));
    self->model = model;
    self->server = IedServer_create(model);
    self->indicationStVal = indicationStVal;
    self->indicationValue = false;
    self->spcso1StVal = spcso1StVal;
    self->spcso1Value = false;
    self->modStVal = modStVal;
    self->behStVal = behStVal;

    return self;
}

SimServer
SimServer_createWithDatasetLimits(int maxDataSetEntries, int maxAssociationSpecificDataSets) {
    DataAttribute* indicationStVal;
    DataAttribute* spcso1StVal;
    DataAttribute* modStVal;
    DataAttribute* behStVal;
    IedModel* model = buildModel(&indicationStVal, &spcso1StVal, &modStVal, &behStVal);

    IedServerConfig config = IedServerConfig_create();
    IedServerConfig_setMaxDataSetEntries(config, maxDataSetEntries);
    IedServerConfig_setMaxAssociationSpecificDataSets(config, maxAssociationSpecificDataSets);

    SimServer self = calloc(1, sizeof(struct sSimServer));
    self->model = model;
    self->server = IedServer_createWithConfig(model, NULL, config);
    self->indicationStVal = indicationStVal;
    self->indicationValue = false;
    self->spcso1StVal = spcso1StVal;
    self->spcso1Value = false;
    self->modStVal = modStVal;
    self->behStVal = behStVal;

    /* Values are copied out synchronously during IedServer_createWithConfig
     * (confirmed against the vendored source) - safe to destroy immediately. */
    IedServerConfig_destroy(config);

    return self;
}

void
SimServer_start(SimServer self, int tcpPort) {
    IedServer_start(self->server, tcpPort);

    /* IedServer_startGoosePublishing is NOT used here - it shares IedServer's
     * single `running` flag with IedServer_start, and no-ops immediately if
     * that flag is already true (ied_server.c: "if (self->running) return;").
     * Since IedServer_start (above) already sets that flag and already spawns
     * a worker thread that drives GOOSE_processGooseEvents via
     * processPeriodicTasks (mms_mapping.c) for every enabled GSEControlBlock,
     * IedServer_enableGoosePublishing is all that's needed - it just arms the
     * control blocks (GoEna=true, creates the GoosePublisher). Confirmed via
     * a debug-instrumented libiec61850 build that MmsGooseControlBlock_enable
     * was never even being called while this used startGoosePublishing -
     * that guard was silently swallowing it. "lo" is fine here: this
     * simulator only ever runs in-process against goose_subscriber's E2E
     * test, never against a real network. */
    IedServer_setGooseInterfaceId(self->server, "lo");
    IedServer_useGooseVlanTag(self->server, NULL, "gcbInd", false);
    IedServer_enableGoosePublishing(self->server);
}

void
SimServer_setIndication(SimServer self, bool value) {
    self->indicationValue = value;
    IedServer_updateBooleanAttributeValue(self->server, self->indicationStVal, value);
}

void
SimServer_setSpcso1Indication(SimServer self, bool value) {
    self->spcso1Value = value;
    IedServer_updateBooleanAttributeValue(self->server, self->spcso1StVal, value);
}

void
SimServer_setModStVal(SimServer self, int32_t value) {
    IedServer_updateInt32AttributeValue(self->server, self->modStVal, value);
}

void
SimServer_setBehStVal(SimServer self, int32_t value) {
    IedServer_updateInt32AttributeValue(self->server, self->behStVal, value);
}

void
SimServer_setFilestoreBasepath(SimServer self, const char* basepath) {
    IedServer_setFilestoreBasepath(self->server, basepath);
}

/* AcseAuthenticator (iso_connection_parameters.h): accepts only
 * ACSE_AUTH_PASSWORD connections whose password matches self->expectedPassword
 * byte-for-byte via AcseAuthenticationParameter_getPassword/getPasswordLength
 * (the password is not guaranteed null-terminated, so length must be checked
 * before comparing bytes - a wrong-length password must never be treated as a
 * prefix match). securityToken/appReference are left untouched - this
 * simulator has no use for either. */
static bool
checkPassword(void* parameter, AcseAuthenticationParameter authParameter, void** securityToken,
        IsoApplicationReference* appReference) {
    (void) securityToken;
    (void) appReference;
    SimServer self = (SimServer) parameter;

    if (!self->expectedPassword) return true;
    if (AcseAuthenticationParameter_getAuthMechanism(authParameter) != ACSE_AUTH_PASSWORD) return false;

    int passwordLen = AcseAuthenticationParameter_getPasswordLength(authParameter);
    size_t expectedLen = strlen(self->expectedPassword);
    if ((size_t) passwordLen != expectedLen) return false;

    return memcmp(AcseAuthenticationParameter_getPassword(authParameter), self->expectedPassword, expectedLen) == 0;
}

void
SimServer_requireAuthentication(SimServer self, const char* expectedPassword) {
    self->expectedPassword = expectedPassword;
    IedServer_setAuthenticator(self->server, checkPassword, self);
}

void
SimServer_stop(SimServer self) {
    /* IedServer_stopGoosePublishing is NOT used here - it sets IedServer's
     * shared `running` flag back to false, which would incorrectly tear down
     * the whole server's periodic-tasks thread out from under IedServer_stop
     * below (see SimServer_start's comment). IedServer_disableGoosePublishing
     * only disables the control blocks (GoEna=false) and destroys the
     * publishers, without touching that shared flag. */
    IedServer_disableGoosePublishing(self->server);
    IedServer_stop(self->server);
}

void
SimServer_destroy(SimServer self) {
    if (!self) return;
    IedServer_destroy(self->server);
    IedModel_destroy(self->model);
    free(self);
}
