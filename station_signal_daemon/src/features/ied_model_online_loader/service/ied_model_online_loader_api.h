#ifndef IED_MODEL_ONLINE_LOADER_API_H_
#define IED_MODEL_ONLINE_LOADER_API_H_

#include "features/ied_model_online_loader/domain/ied_model_online_loader_types.h"
#include "features/ied_model/service/ied_model_api.h"

/*
 * Public boundary of this feature. Other code (orchestration) should only
 * ever include this header - never reach into domain/data directly.
 *
 * This is the ONE narrow, deliberate exception to CLAUDE.md's "No
 * over-the-wire tree discovery" Hard Rule: a fallback for real, connectable
 * IEC 61850 devices (e.g. OMICRON IED Scout's "Simulate IED" mode) that never
 * serve an SCL file over MMS file services at all, so scl_bootstrap can never
 * succeed against them no matter how the directory is browsed. Walks the
 * live device's MMS ACSI directory/model-discovery services
 * (GetLogicalDeviceList/GetLogicalDeviceDirectory/GetLogicalNodeDirectory/
 * GetDataDirectory[ByFC]/GetDataSetDirectory/GetRCBValues/GetGoCBValues)
 * instead of parsing an SCL file, and produces an IedModelHandle
 * indistinguishable to every other feature from one built by
 * IedModel_loadFromFile - mms_report_client/goose_subscriber require zero
 * changes to consume it.
 *
 * Engaged only via orchestration's explicit Orchestration_runFromOnlineDiscovery
 * entry point (never a silent, automatic substitute inside Orchestration_run)
 * - see that function's own doc comment for the caller-driven "try SCL first,
 * fall back only on SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND" policy this is
 * meant to support.
 */

/*
 * Builds a complete IedModelHandle by walking host:port's live data model.
 * Owns its own one-shot IedConnection end-to-end (create, optional ACSE
 * password auth, connect, discover, close+destroy) - same "owns its own MMS
 * session" shape as SclBootstrap_scanAndFetch/MmsReportClient_create, never a
 * caller-supplied connection object (orchestration's own documented
 * invariant is zero direct third-party includes, so this feature - not
 * orchestration - is where that connection lifecycle has to live).
 *
 * `iedName` only labels the constructed model (there is no SCL <IED> list to
 * auto-detect from over a live connection) - NULL/empty defaults to
 * "OnlineDiscovered". `mode` is the same AccessMode every other ied_model
 * consumer uses; note this loader only builds FC=ST/MX structure (the
 * "reportable" FC pair, sufficient for report/GOOSE consumers and Dyn-dataset
 * synthesis) - IedModel_getReadTargets/_getControlTargets against a model
 * built here will see an incomplete/empty tree, an accepted v1 scope limit
 * since this entry point only ever drives report/GOOSE consumption.
 * `acseAuthPassword` NULL means unauthenticated, same convention as
 * scl_bootstrap/mms_report_client's own config fields.
 *
 * `categoryFilter` is the same contract as IedModel_loadFromFile's own
 * parameter of the same name - stored on the resulting handle. Unlike
 * daSemantics/dynDataSetMax, LN classification IS available on this path
 * (reverse-matched from the raw wire instance name per LN - see
 * IedModel_categorizeWireInstanceName), so a non-ALL filter is fully
 * effective here too, not degraded.
 *
 * Returns NULL and sets *outError on failure. Caller owns the returned handle
 * exactly like IedModel_loadFromFile's (IedModel_release when done).
 */
IedModelHandle
IedModelOnlineLoader_build(const char* host, int port, const char* iedName, AccessMode mode,
        LnCategoryMask categoryFilter, const char* acseAuthPassword, const IedModelOnlineLoaderConfig* config,
        IedModelOnlineLoaderError* outError);

#endif /* IED_MODEL_ONLINE_LOADER_API_H_ */
