#ifndef IED_MODEL_ONLINE_LOADER_CONNECTION_H_
#define IED_MODEL_ONLINE_LOADER_CONNECTION_H_

#include "iec61850_client.h"
#include "iec61850_model.h"
#include "linked_list.h"
#include "features/ied_model_online_loader/domain/ied_model_online_loader_types.h"

/*
 * Walks a live, already-connected IedConnection's MMS ACSI directory/
 * model-discovery services and builds an equivalent dynamic IedModel - the
 * data-layer counterpart to ied_model_scl_loader.c's mxml-tree-driven
 * IedModelSclLoader_load, sourcing the same shape of dynamic-model
 * construction calls (IedModel_create/LogicalDevice_create/.../
 * ReportControlBlock_create/GSEControlBlock_create) from live MMS requests
 * instead of a parsed SCL file. `conn` must already be connected
 * (IedConnection_connect already succeeded) - this function only ever reads,
 * never calls connect/destroy on it. Returns NULL + *outError on failure;
 * caller owns the returned IedModel* (IedModel_destroy, or indirectly via
 * IedModel_wrapDynamicModel + IedModel_release).
 *
 * *outLnCategories is set to NULL immediately, and (on success) reassigned to
 * a newly-created LinkedList of heap-boxed IedModelLnCategoryEntry* right
 * before returning - one entry per LN built, classified via
 * IedModel_categorizeWireInstanceName's reverse-match against the raw wire
 * instance name (no lnClass attribute is ever available over MMS ACSI
 * directory services, unlike the SCL path - see that function's own doc
 * comment for the matching algorithm). Caller owns the list and its elements
 * (LinkedList_destroyDeep(list, free)) - ied_model_online_loader_api.c's
 * IedModelOnlineLoader_build passes this straight into
 * IedModel_wrapDynamicModel, mirroring how ied_model_api.c's own
 * IedModel_loadFromFile adopts IedModelSclLoader_load's outLnCategories.
 */
IedModel*
IedModelOnlineLoaderConnection_build(IedConnection conn, const char* iedName,
        const IedModelOnlineLoaderConfig* config, LinkedList* outLnCategories,
        IedModelOnlineLoaderError* outError);

/*
 * Owns the entire one-shot connection lifecycle: creates its own IedConnection
 * (IedConnection_createEx), configures ACSE password auth if given, connects
 * to host:port, calls IedModelOnlineLoaderConnection_build, then always closes
 * and destroys the connection before returning (discovery is a single
 * request/response burst, not a long-lived association like
 * mms_report_client's - there is nothing to keep the connection open for
 * afterward). Same "owns its own MMS session end-to-end" shape as
 * scl_bootstrap's data/scl_bootstrap_mms_session.c, for the same reason:
 * orchestration's own documented invariant is zero direct third-party
 * includes, so the connection lifecycle must live inside this feature, not
 * in the caller. Returns NULL + *outError (CONNECT_FAILED if the connect
 * itself failed) on failure.
 *
 * *outLnCategories has the same contract as IedModelOnlineLoaderConnection_build's
 * own out-param of the same name (this function is a thin connection-lifecycle
 * wrapper around it).
 */
IedModel*
IedModelOnlineLoaderConnection_connectAndBuild(const char* host, int port, const char* iedName,
        const char* acseAuthPassword, const IedModelOnlineLoaderConfig* config,
        LinkedList* outLnCategories, IedModelOnlineLoaderError* outError);

#endif /* IED_MODEL_ONLINE_LOADER_CONNECTION_H_ */
