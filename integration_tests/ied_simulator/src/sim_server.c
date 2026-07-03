#include <stdlib.h>
#include <string.h>
#include "sim_types.h"
#include "iec61850_dynamic_model.h"

SimServer
SimServer_create(void) {
    IedModel* model = IedModel_create("Reporter1");
    LogicalDevice* ld = LogicalDevice_create("LD1", model);
    LogicalNode* ln0 = LogicalNode_create("LLN0", ld);
    LogicalNode* ggio1 = LogicalNode_create("GGIO1", ld);

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
    DataAttribute_create("stVal", (ModelNode*) spcso1, IEC61850_BOOLEAN, IEC61850_FC_ST, TRG_OPT_DATA_CHANGED, 0, 0);
    DataAttribute_create("q", (ModelNode*) spcso1, IEC61850_QUALITY, IEC61850_FC_ST, 0, 0, 0);
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

    /* dataSetName left NULL (no server-side default dataset) - the client
     * (mms_report_client) always explicitly sets DatSet alongside RptEna on
     * enable, matching libiec61850's own reference client example
     * (client_example_no_thread.c), which is the validated, non-fragile path. */
    ReportControlBlock_create("brcbMain", ln0, "brcbMain", true, NULL, 1,
            TRG_OPT_DATA_CHANGED | TRG_OPT_QUALITY_CHANGED | TRG_OPT_GI,
            RPT_OPT_SEQ_NUM | RPT_OPT_TIME_STAMP | RPT_OPT_DATA_SET | RPT_OPT_REASON_FOR_INCLUSION,
            0, 60000);

    /* GSEControlBlock over the same ds1 dataset - lets goose_subscriber's E2E
     * test observe the same GGIO1.Ind1.stVal flip that mms_report_client's
     * E2E test observes via reporting. minTime=10/maxTime=5000 mirror
     * breaker1.cid's <GSE><MinTime>/<MaxTime> convention. Addressing (VLAN
     * 10, priority 4, wire APPID 1000 decimal, multicast dst MAC
     * 01-0c-cd-01-00-01) is mirrored into fixtures/reporter1.cid's
     * <Communication> section - MUST stay numerically identical to what's
     * declared there, since goose_subscriber configures its APPID/dst-MAC
     * filter from the SCL fixture, not from this file. (The 3rd
     * GSEControlBlock_create argument below, "1000", is a separate thing -
     * GSEControl's textual "appID" SCL attribute, purely descriptive per the
     * standard, unrelated to the numeric wire APPID carried in PhyComAddress.) */
    GSEControlBlock* gcbInd = GSEControlBlock_create("gcbInd", ln0, "1000", "ds1", 1, false, 10, 5000);
    uint8_t gooseDstMac[6] = { 0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01 };
    GSEControlBlock_addPhyComAddress(gcbInd, PhyComAddress_create(4, 10, 1000, gooseDstMac));

    SimServer self = calloc(1, sizeof(struct sSimServer));
    self->model = model;
    self->server = IedServer_create(model);
    self->indicationStVal = stVal;
    self->indicationValue = false;

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
