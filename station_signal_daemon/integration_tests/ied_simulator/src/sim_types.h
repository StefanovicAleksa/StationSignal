#ifndef SIM_TYPES_H_
#define SIM_TYPES_H_

#include <stdbool.h>
#include "iec61850_model.h"
#include "iec61850_server.h"

/*
 * A minimal, self-contained fake IED ("Reporter1") built directly via
 * libiec61850's dynamic model API - NOT via genmodel.jar codegen (that tool
 * exists in the sibling libiec61850 checkout and produces real, useful
 * output, but is heavyweight/verbose for the tiny single-LD fixture E2E
 * tests need here; scripts/generate_model.sh is left as a placeholder for a
 * future genmodel.jar-based workflow if a larger simulated device is ever
 * needed).
 *
 * Deliberately has ZERO includes from src/ - fully decoupled from the
 * daemon's own production code, exactly like a real external IED would be.
 * The model shape mirrors integration_tests/mms_report_client/fixtures/reporter1.cid
 * (the same shape described as SCL for the client side to parse) - kept in
 * sync by hand, cross-referenced in both places.
 *
 * Model shape: IED "Reporter1", LDevice "LD1", LLN0 with one buffered RCB
 * ("brcbMain", trgOps=dchg+qchg+gi) AND one GSEControlBlock ("gcbInd",
 * appID="1000") over the same dataset ("ds1") containing GGIO1.Ind1.stVal
 * (SPS - the read/reported/published indication point flipped by
 * SimServer_setIndication) followed by GGIO1.Ind1.q (its sibling Quality
 * attribute) - one flip drives both the mms_report_client and
 * goose_subscriber E2E tests. gcbInd's <Communication>/<GSE><Address> (VLAN
 * 10, priority 4, APPID 0x1000, multicast dst MAC 01-0c-cd-01-00-01) is
 * mirrored into integration_tests/mms_report_client/fixtures/reporter1.cid
 * (this file's fixture cross-reference). GOOSE is published on "lo" (see
 * SimServer_start) - fine for these in-process/loopback E2E tests, not meant
 * to be a real-interface-configurable simulator. GGIO1.SPCSO1 (SPC, with a
 * controllable Oper.ctlVal) exists alongside it purely to give the simulated
 * device a realistic read+write-capable shape; it is not exercised by the
 * report-focused E2E test.
 *
 * Opaque handle - defined directly here (opacity by convention, matching this
 * project's ied_model/mms_report_client handle style), since every function
 * in sim_server.c needs field access.
 */
struct sSimServer {
    IedModel* model;
    IedServer server;
    DataAttribute* indicationStVal; /* GGIO1.Ind1.stVal */
    bool indicationValue;
    DataAttribute* spcso1StVal; /* GGIO1.SPCSO1.stVal - see SimServer_setSpcso1Indication's own doc comment */
    bool spcso1Value;
    const char* expectedPassword; /* borrowed, NULL = no authentication required */
};

typedef struct sSimServer* SimServer;

/* Builds the model and creates (but does not start) the IedServer. */
SimServer
SimServer_create(void);

/*
 * Same model as SimServer_create, but the IedServer is created via
 * IedServer_createWithConfig with an explicit dataset-size/count cap
 * (IedServerConfig_setMaxDataSetEntries / _setMaxAssociationSpecificDataSets)
 * instead of library defaults (100 entries / 10 association-specific
 * datasets respectively) - added specifically so an E2E test can
 * deterministically reproduce mms_report_client's dynamic-dataset chunking
 * (maxAttributes) and count-budget-exhaustion (DynDataSet max) behavior
 * without needing a real capacity-limited device. Both config values are
 * copied out of the IedServerConfig object synchronously during
 * IedServer_createWithConfig (confirmed against the vendored source,
 * ied_server.c), so the config object is destroyed before this function
 * returns - it does not need to outlive the call. */
SimServer
SimServer_createWithDatasetLimits(int maxDataSetEntries, int maxAssociationSpecificDataSets);

/* Starts listening for client connections on tcpPort (spawns libiec61850's
 * own internal connection-handling and event-worker threads - non-blocking),
 * and starts GOOSE publishing on "lo" (spawns IedServer's own GOOSE event
 * worker thread via IedServer_startGoosePublishing - also non-blocking). */
void
SimServer_start(SimServer self, int tcpPort);

/*
 * Flips GGIO1.Ind1.stVal to the given value. Safe to call from any thread
 * (IedServer_updateBooleanAttributeValue's own documented guarantee - no
 * explicit lock/unlock needed for a single attribute update). With the
 * model's RCB configured for a data-change trigger, this causes the server
 * to push a report to any client with an enabled RCB subscribed to the
 * containing dataset.
 */
void
SimServer_setIndication(SimServer self, bool value);

/*
 * Flips GGIO1.SPCSO1.stVal to the given value - the sibling counterpart of
 * SimServer_setIndication, over a DIFFERENT DO (SPCSO1, not Ind1). Added so
 * an E2E test can flip an attribute that is NOT already a member of
 * brcbMain's/brcbDup's own "ds1" dataset (both of which observe
 * GGIO1.Ind1.stVal/q), avoiding a spurious collision with
 * mms_report_client's cross-RCB duplicate-content suppression
 * (MmsReportClientUseCases_shouldForwardAcrossRcb) when a test wants to
 * observe a report from a DIFFERENT RCB/dataset covering the same underlying
 * kind of attribute without its content being suppressed as an apparent
 * duplicate of what brcbMain/brcbDup already forwarded. Same thread-safety
 * guarantee as SimServer_setIndication.
 */
void
SimServer_setSpcso1Indication(SimServer self, bool value);

/*
 * Points the server's MMS file services (GetFileDirectory/GetFile) at a real
 * filesystem directory - added specifically so scl_bootstrap's E2E test can
 * exercise real file browse/download against this simulator, mirroring how a
 * real IED exposes its own SCL export over MMS. Wraps
 * IedServer_setFilestoreBasepath (iec61850_server.h) - must be called before
 * SimServer_start.
 *
 * basepath MUST end with a trailing '/' (e.g. "fixtures/served_files/") -
 * confirmed empirically (not documented at the function itself): without the
 * trailing slash, every GetFileDirectory/GetFile request against it fails
 * with IED_ERROR_OBJECT_DOES_NOT_EXIST, matching the convention used by
 * libiec61850's own server_example_files.c reference example.
 *
 * Also confirmed empirically: IedConnection_getFileDirectory(conn, &err, NULL)
 * against this filestore-backed implementation returns a flat, fully
 * recursive listing of every file under basepath, with each entry already
 * being the full path relative to basepath (e.g. "subdir/nested.icd") - it
 * does NOT return one entry per subdirectory with a trailing-slash marker
 * requiring a separate recursive browse call. A stricter ACSI-spec-reading
 * server might behave differently (see scl_bootstrap's
 * SclBootstrapUseCases_isDirectoryEntry / browseDirectoryRecursive, which
 * defensively supports that case too), but it's not what this reference
 * implementation - or, presumably, most libiec61850-based real IEDs - do in
 * practice.
 */
void
SimServer_setFilestoreBasepath(SimServer self, const char* basepath);

/*
 * Gates MMS client connections behind ACSE password authentication - added
 * specifically so scl_bootstrap's E2E test can exercise its auth-retry path
 * against a real server. Wraps IedServer_setAuthenticator (iec61850_server.h)
 * with a callback that accepts only ACSE_AUTH_PASSWORD connections whose
 * password matches expectedPassword exactly; any other connection (no auth,
 * wrong password, certificate/TLS auth) is rejected. Must be called before
 * SimServer_start. expectedPassword is borrowed - must outlive the server.
 */
void
SimServer_requireAuthentication(SimServer self, const char* expectedPassword);

/* Stops the server (blocking - joins its connection-handling and event-worker
 * threads). Must not be called from within any server-side callback. */
void
SimServer_stop(SimServer self);

/* Destroys the IedServer and the IedModel. Call after _stop(). */
void
SimServer_destroy(SimServer self);

#endif /* SIM_TYPES_H_ */
