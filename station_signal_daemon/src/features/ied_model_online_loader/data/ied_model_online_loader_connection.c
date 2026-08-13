#define SS_LOG_FEATURE "ied_model_online_loader"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "features/ied_model_online_loader/data/ied_model_online_loader_connection.h"
#include "features/ied_model_online_loader/data/ied_model_online_loader_auth.h"
#include "features/ied_model_online_loader/domain/ied_model_online_loader_usecases.h"
#include "features/ied_model/service/ied_model_api.h"
#include "iec61850_dynamic_model.h"
#include "iec61850_common.h"
#include "mms_type_spec.h"
#include "mms_value.h"
#include "hal_time.h"
#include "log.h"

/* ---- small string helpers ---- */

static char*
joinWithSeparator(const char* a, const char* sep, const char* b) {
    size_t len = strlen(a) + strlen(sep) + strlen(b) + 1;
    char* out = malloc(len);
    if (out) snprintf(out, len, "%s%s%s", a, sep, b);
    return out;
}

static char*
buildLnRef(const char* ldName, const char* lnName) {
    return joinWithSeparator(ldName, "/", lnName);
}

static char*
buildRcbDotRef(const char* lnRef, bool buffered, const char* rcbName) {
    return joinWithSeparator(lnRef, buffered ? ".BR." : ".RP.", rcbName);
}

static char*
buildAppIdHex(uint16_t appId) {
    char* out = malloc(8);
    if (out) snprintf(out, 8, "%04X", appId);
    return out;
}

/* ---- dataset resolution (shared by RCB and GoCB construction) ---- */

/*
 * Both ReportControlBlock_create's dataSetName and GSEControlBlock_create's
 * dataSet parameter want the BARE local dataset name (confirmed against
 * ied_model_scl_loader.c's own buildReportControls/buildGseControls, which
 * pass SCL's raw datSet="..." attribute value straight through - always bare,
 * e.g. "ds1", per SCL semantics - and empirically confirmed the hard way:
 * passing getRCBValues/getGoCBValues' own FULLY QUALIFIED reference
 * ("LD/LN$ds1") here instead produces a double-qualified, unresolvable
 * dataset reference once IedModelUseCases_getReportSubscriptionTargets/
 * _getGooseSubscriptionTargets re-prepend their own lnRef+"$" on top of it).
 * Returns a borrowed pointer into `qualifiedRef` - never allocates, caller
 * must not free the result separately from `qualifiedRef` itself.
 */
static const char*
bareDatasetName(const char* qualifiedRef) {
    const char* dollar = strrchr(qualifiedRef, '$');
    return dollar ? dollar + 1 : qualifiedRef;
}

static bool
datasetAlreadyBuilt(LinkedList builtDatasets, const char* datasetRef) {
    LinkedList element = LinkedList_getNext(builtDatasets);
    while (element) {
        if (strcmp((char*) LinkedList_getData(element), datasetRef) == 0) return true;
        element = LinkedList_getNext(element);
    }
    return false;
}

/*
 * Resolves one RCB/GoCB's non-empty dataset reference ("LD/LN$dsName", this
 * codebase's own convention - see ied_model_usecases.c) into a real DataSet/
 * DataSetEntry chain, live, via IedConnection_getDataSetDirectory - the wire
 * equivalent of IedModel_getDataSetMemberReferences's own SCL-derived output.
 * `ln` must be the RCB/GoCB's own parent LN (dataset and control block are
 * always co-located under the same LN in this codebase's convention - see
 * ied_model_scl_loader.c's buildDataSets, which relies on the identical
 * assumption). De-dupes via builtDatasets (owned char* list) so two control
 * blocks sharing one dataset don't create it twice - DataSet_create has no
 * "already exists" tolerance of its own.
 */
static void
resolveAndBuildDataset(IedConnection conn, LogicalNode* ln, const char* datasetRef, LinkedList builtDatasets) {
    if (!datasetRef || !datasetRef[0] || datasetAlreadyBuilt(builtDatasets, datasetRef)) return;

    IedClientError err = IED_ERROR_OK;
    bool isDeletable = false;
    LinkedList members = IedConnection_getDataSetDirectory(conn, &err, datasetRef, &isDeletable);
    if (!members) {
        SS_LOG_WARN("[ied_model_online_loader] WARN: could not resolve dataset '%s' (error %d) - "
                "control block(s) referencing it will report/publish nothing\n", datasetRef, err);
        return;
    }

    /* This codebase's own dataset-reference convention is always exactly
     * "<lnRef>$<dsName>" (one "$", no nested structure) - see
     * IedModelUseCases_getReportSubscriptionTargets/_getGooseSubscriptionTargets. */
    const char* dollar = strrchr(datasetRef, '$');
    const char* dsBareName = dollar ? dollar + 1 : datasetRef;

    /* Bare local name only - DataSet_create prepends "<lnName>$" internally
     * (see CLAUDE.md's documented dynamic-model gotcha #2). */
    DataSet* dataSet = DataSet_create(dsBareName, ln);

    LinkedList element = LinkedList_getNext(members);
    while (element) {
        char* acsiMemberRef = (char*) LinkedList_getData(element);
        char* wireRef = IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(acsiMemberRef);
        if (wireRef) {
            /* wireRef is already LD-prefix-free (IedModelOnlineLoaderUseCases_
             * convertAcsiRefToWireRef strips it) - dynamic-model gotcha #1. */
            DataSetEntry_create(dataSet, wireRef, -1, NULL);
            free(wireRef);
        } else {
            SS_LOG_WARN("[ied_model_online_loader] WARN: could not convert dataset member '%s' "
                    "(dataset '%s') to wire form - member skipped\n", acsiMemberRef, datasetRef);
        }
        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(members, free);

    char* ownedRef = strdup(datasetRef);
    if (ownedRef) LinkedList_add(builtDatasets, ownedRef);
}

/* ---- FC=ST/MX data object / data attribute tree ----
 *
 * Deliberately restricted to FC=ST/MX only - the same "reportable" FC pair
 * this codebase's own IedModelUseCases_getReadTargets/
 * _getReportableAttributeReferencesForLogicalNode already treat as the whole
 * story for reporting/GOOSE/dynamic-dataset purposes. No current ied_model
 * accessor needs any other FC (dataset members come from
 * getDataSetDirectory directly as flat strings, not from this tree; RCB/GoCB
 * configuration comes from getRCBValues/getGoCBValues directly, not from this
 * tree either) - this tree exists solely to back
 * IedModel_getReportableAttributeReferencesForLogicalNode's dynamic-dataset
 * fallback for a discovered Dyn RCB/GoCB with no configured dataset. A
 * consequence: IedModel_getReadTargets/_getControlTargets (FC=CO and
 * generic-read paths) will see an incomplete/empty tree against a model
 * built by this loader - an accepted v1 scope limit, since this daemon's own
 * online-discovery entry point only ever drives report/GOOSE consumers.
 */

/*
 * Coarse MmsType -> DataAttributeType mapping. Deliberately approximate:
 * MmsVariableSpecification only carries the wire-level MMS basic type, which
 * cannot distinguish CDC-semantic types that share an MMS encoding (e.g.
 * IEC61850_QUALITY vs. a generic 13-bit IEC61850_GENERIC_BITSTRING;
 * IEC61850_CODEDENUM vs. a plain small bitstring) - guessing which one from
 * naming convention alone would violate this codebase's "don't guess IEC
 * 61850 semantics" rule (see CLAUDE.md's Hard Rules). Confirmed harmless for
 * every current ied_model accessor: none of them ever read DataAttributeType
 * back off a built node (only ModelNode_getType()/FC are consulted) - this
 * matters only if a future feature starts relying on real CDC semantics from
 * a model built by this loader. IEC61850_TIMESTAMP is the one unambiguous
 * exception (MMS_UTC_TIME has no other meaning in this codebase's own
 * datasets). Returns IEC61850_UNKNOWN_TYPE for anything not listed - callers
 * skip creating a DataAttribute rather than guess a placeholder type.
 */
static DataAttributeType
mapMmsTypeToDataAttributeType(MmsType type) {
    switch (type) {
        case MMS_BOOLEAN: return IEC61850_BOOLEAN;
        case MMS_INTEGER: return IEC61850_INT32;
        case MMS_UNSIGNED: return IEC61850_INT32U;
        case MMS_FLOAT: return IEC61850_FLOAT32;
        case MMS_BIT_STRING: return IEC61850_GENERIC_BITSTRING;
        case MMS_OCTET_STRING: return IEC61850_OCTET_STRING_64;
        case MMS_VISIBLE_STRING: return IEC61850_VISIBLE_STRING_255;
        case MMS_UTC_TIME: return IEC61850_TIMESTAMP;
        default: return IEC61850_UNKNOWN_TYPE;
    }
}

static void buildFcContainerChildren(IedConnection conn, const char* nodeRef, FunctionalConstraint fc,
        ModelNode* parentNode, LinkedList children);

/*
 * Builds a DataAttribute/DataObject subtree directly from a variable's own
 * recursive type description, bypassing IedConnection_getDataDirectoryByFC
 * entirely - the fallback buildFcContainer engages when that call finds zero
 * children. Root-caused against real hardware: getDataDirectoryByFC only
 * finds a DO's children by string-matching separately-named leaf entries in
 * the domain's flat variable-name list (confirmed by reading libiec61850's
 * own getDataDirectoryByFc/IedConnection_getDeviceModelFromServer source,
 * third_party's vendored .a has no source but the sibling
 * /home/aleksa/code/station_signal/libiec61850 checkout does) - but real IEC
 * 61850 servers commonly expose a structured DO (e.g. an SPS's
 * stVal/q/t) as ONE opaque MMS_STRUCTURE-typed named variable instead,
 * exactly matching what a report's own wire value looks like
 * (MmsReportEntry.value arriving as one undecomposed MMS_STRUCTURE) - the
 * domain's flat name list then never contains the "$"-suffixed leaf entries
 * getDataDirectoryByFC's matching depends on, so it silently finds nothing
 * for every such DO. One getVariableSpecification call on the DO itself
 * returns the FULL recursive component layout (names + types) regardless of
 * how the domain names its variables, so this needs no further wire round
 * trips per leaf.
 */
static void
buildDataAttributeTreeFromSpec(MmsVariableSpecification* spec, ModelNode* parentNode, FunctionalConstraint fc) {
    if (!spec || MmsVariableSpecification_getType(spec) != MMS_STRUCTURE) return;

    int size = MmsVariableSpecification_getSize(spec);
    for (int i = 0; i < size; i++) {
        /* Borrowed - owned by (and destroyed recursively along with) the
         * top-level spec buildFcContainer destroys; never destroyed here. */
        MmsVariableSpecification* child = MmsVariableSpecification_getChildSpecificationByIndex(spec, i);
        if (!child) continue;

        const char* childName = MmsVariableSpecification_getName(child);
        if (!childName) continue;

        MmsType childType = MmsVariableSpecification_getType(child);
        if (childType == MMS_STRUCTURE) {
            DataObject* childDo = DataObject_create(childName, parentNode, 0);
            buildDataAttributeTreeFromSpec(child, (ModelNode*) childDo, fc);
        } else {
            DataAttributeType type = mapMmsTypeToDataAttributeType(childType);
            if (type != IEC61850_UNKNOWN_TYPE) {
                DataAttribute_create(childName, parentNode, type, fc, 0, 0, 0);
            }
        }
    }
}

/*
 * One node's worth of FC=<fc> children: each child is either itself a
 * container (its own getDataDirectoryByFC call returns a non-empty list -
 * built as a DataObject, recursed into) or a genuine leaf (empty list - type
 * resolved via getVariableSpecification, built as a DataAttribute). This
 * uniform "container = DataObject, regardless of whether it's really an SDO
 * or a CONSTRUCTED DA" choice is safe because every existing tree-walking
 * accessor in ied_model_usecases.c (collectDataAttributesByFc,
 * collectLeafReferencesByFc, ...) already treats any non-DataAttribute node
 * as "recurse unconditionally, filter once a DataAttribute leaf is reached" -
 * it never distinguishes SDO from CONSTRUCTED DA containers itself.
 */
static void
buildFcContainer(IedConnection conn, const char* nodeRef, FunctionalConstraint fc, ModelNode* parentNode) {
    IedClientError err = IED_ERROR_OK;
    LinkedList children = IedConnection_getDataDirectoryByFC(conn, &err, nodeRef, fc);

    if (children && LinkedList_size(children) > 0) {
        buildFcContainerChildren(conn, nodeRef, fc, parentNode, children);
        LinkedList_destroyDeep(children, free);
        return;
    }
    if (children) LinkedList_destroyDeep(children, free);

    /* getDataDirectoryByFC found nothing - see buildDataAttributeTreeFromSpec's
     * own doc comment for why this is the expected, common case on real
     * hardware rather than a network failure. Fall back to the variable's own
     * type description. */
    IedClientError specErr = IED_ERROR_OK;
    MmsVariableSpecification* spec = IedConnection_getVariableSpecification(conn, &specErr, nodeRef, fc);
    if (spec) {
        buildDataAttributeTreeFromSpec(spec, parentNode, fc);
        MmsVariableSpecification_destroy(spec);
        return;
    }
}

static void
buildFcContainerChildren(IedConnection conn, const char* nodeRef, FunctionalConstraint fc,
        ModelNode* parentNode, LinkedList children) {
    LinkedList element = LinkedList_getNext(children);
    while (element) {
        char* childName = (char*) LinkedList_getData(element);
        char* childRef = joinWithSeparator(nodeRef, ".", childName);

        if (childRef) {
            IedClientError err = IED_ERROR_OK;
            LinkedList grandchildren = IedConnection_getDataDirectoryByFC(conn, &err, childRef, fc);

            if (grandchildren && LinkedList_size(grandchildren) > 0) {
                DataObject* childDo = DataObject_create(childName, parentNode, 0);
                buildFcContainerChildren(conn, childRef, fc, (ModelNode*) childDo, grandchildren);
            } else {
                IedClientError specErr = IED_ERROR_OK;
                MmsVariableSpecification* spec =
                        IedConnection_getVariableSpecification(conn, &specErr, childRef, fc);
                if (spec) {
                    DataAttributeType type = mapMmsTypeToDataAttributeType(MmsVariableSpecification_getType(spec));
                    MmsVariableSpecification_destroy(spec);
                    if (type != IEC61850_UNKNOWN_TYPE) {
                        DataAttribute_create(childName, parentNode, type, fc, 0, 0, 0);
                    }
                }
            }

            if (grandchildren) LinkedList_destroyDeep(grandchildren, free);
            free(childRef);
        }

        element = LinkedList_getNext(element);
    }
}

/*
 * Every FC=ST/MX-reportable DO under one LN. A DO name appearing in both the
 * ST and MX walk (uncommon but not impossible) would otherwise get created
 * twice - each DO name is only ever seen once per LN here since both walks
 * start from the same getLogicalNodeDirectory(ACSI_CLASS_DATA_OBJECT) name
 * list and build into the SAME DataObject node, not two separate ones.
 */
static void
buildLogicalNodeDataModel(IedConnection conn, const char* lnRef, LogicalNode* ln) {
    IedClientError err = IED_ERROR_OK;
    LinkedList doNames = IedConnection_getLogicalNodeDirectory(conn, &err, lnRef, ACSI_CLASS_DATA_OBJECT);
    if (!doNames) {
        return;
    }

    LinkedList element = LinkedList_getNext(doNames);
    while (element) {
        char* doName = (char*) LinkedList_getData(element);
        char* doRef = joinWithSeparator(lnRef, ".", doName);

        if (doRef) {
            DataObject* doNode = DataObject_create(doName, (ModelNode*) ln, 0);
            buildFcContainer(conn, doRef, IEC61850_FC_ST, (ModelNode*) doNode);
            buildFcContainer(conn, doRef, IEC61850_FC_MX, (ModelNode*) doNode);

            free(doRef);
        }

        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(doNames, free);
}

/* ---- control blocks ---- */

static void
buildReportControlBlocks(IedConnection conn, const char* lnRef, LogicalNode* ln, ACSIClass acsiClass,
        bool buffered, LinkedList builtDatasets) {
    IedClientError err = IED_ERROR_OK;
    LinkedList names = IedConnection_getLogicalNodeDirectory(conn, &err, lnRef, acsiClass);
    if (!names) return;

    LinkedList element = LinkedList_getNext(names);
    while (element) {
        char* rcbName = (char*) LinkedList_getData(element);
        char* rcbRef = buildRcbDotRef(lnRef, buffered, rcbName);

        char* dataSetRefCopy = NULL;
        uint32_t confRev = 0;

        if (rcbRef) {
            IedClientError rcbErr = IED_ERROR_OK;
            ClientReportControlBlock rcb = IedConnection_getRCBValues(conn, &rcbErr, rcbRef, NULL);
            if (rcb) {
                const char* ds = ClientReportControlBlock_getDataSetReference(rcb);
                if (ds && ds[0]) dataSetRefCopy = strdup(ds);
                confRev = ClientReportControlBlock_getConfRev(rcb);
                ClientReportControlBlock_destroy(rcb);
            } else {
                SS_LOG_WARN("[ied_model_online_loader] WARN: getRCBValues failed for '%s' (error %d) - "
                        "created with no dataset/confRev info\n", rcbRef, rcbErr);
            }
            free(rcbRef);
        }

        /* rptId NULL -> library default (object reference); trgOps/options/
         * bufTm/intgPd left at 0 - this daemon's mms_report_client never
         * writes these live either way (see CLAUDE.md's own note on why),
         * they only matter for a server's own behavior, not a client-side
         * discovered model. dataSetRefCopy NULL is exactly the "Dyn" case -
         * mms_dataset_manager's existing self-create tier handles it
         * unchanged, no special-casing needed here. */
        ReportControlBlock_create(rcbName, ln, NULL, buffered,
                dataSetRefCopy ? bareDatasetName(dataSetRefCopy) : NULL, confRev, 0, 0, 0, 0);

        if (dataSetRefCopy) {
            resolveAndBuildDataset(conn, ln, dataSetRefCopy, builtDatasets);
            free(dataSetRefCopy);
        }

        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(names, free);
}

/*
 * GoCB addressing: ClientGooseControlBlock_getDstAddress returns a
 * PhyComAddress by value (the non-deprecated accessor - the per-field
 * _getDstAddress_appid/_vid/_priority/_addr getters are DEPRECATED per the
 * vendored header). DstAddress is an OPTIONAL GoCB attribute per IEC
 * 61850-7-2 - there is no documented sentinel distinguishing "server never
 * set this" from "explicitly all-zero", so an all-zero appId+vlanId+MAC is
 * treated as "not populated" (flagged in CLAUDE.md/the design plan as an
 * unverified-against-a-real-device heuristic). When not populated,
 * hasAddress stays false and the resulting GooseSubscriptionTarget degrades
 * to unfiltered-by-address reception (goose_subscriber already handles
 * hasAddress==false with zero special-casing - see
 * goose_subscriber_connection.c), identical to today's "SCL with no
 * <GSE><Address>" case.
 */
static void
buildGooseControlBlocks(IedConnection conn, const char* lnRef, LogicalNode* ln, LinkedList builtDatasets) {
    IedClientError err = IED_ERROR_OK;
    LinkedList names = IedConnection_getLogicalNodeDirectory(conn, &err, lnRef, ACSI_CLASS_GoCB);
    if (!names) return;

    LinkedList element = LinkedList_getNext(names);
    while (element) {
        char* gcbName = (char*) LinkedList_getData(element);
        char* gcbDotRef = joinWithSeparator(lnRef, ".", gcbName);

        char* dataSetRefCopy = NULL;
        uint32_t confRev = 0;
        bool hasAddress = false;
        PhyComAddress addr = { 0, 0, 0, { 0, 0, 0, 0, 0, 0 } };
        char* appIdHex = NULL;

        if (gcbDotRef) {
            IedClientError gcbErr = IED_ERROR_OK;
            ClientGooseControlBlock gocb = IedConnection_getGoCBValues(conn, &gcbErr, gcbDotRef, NULL);
            if (gocb) {
                const char* ds = ClientGooseControlBlock_getDatSet(gocb);
                if (ds && ds[0]) dataSetRefCopy = strdup(ds);
                confRev = ClientGooseControlBlock_getConfRev(gocb);

                addr = ClientGooseControlBlock_getDstAddress(gocb);
                bool macNonZero = false;
                for (int i = 0; i < 6; i++) {
                    if (addr.dstAddress[i] != 0) { macNonZero = true; break; }
                }
                hasAddress = (addr.appId != 0 || addr.vlanId != 0 || macNonZero);

                ClientGooseControlBlock_destroy(gocb);
            } else {
                SS_LOG_WARN("[ied_model_online_loader] WARN: getGoCBValues failed for '%s' (error %d) - "
                        "created with no dataset/confRev/address info\n", gcbDotRef, gcbErr);
            }
            appIdHex = buildAppIdHex(addr.appId);
            free(gcbDotRef);
        }

        GSEControlBlock* gcb = GSEControlBlock_create(gcbName, ln, appIdHex ? appIdHex : "",
                dataSetRefCopy ? bareDatasetName(dataSetRefCopy) : NULL, confRev, false, -1, -1);
        free(appIdHex);

        if (hasAddress) {
            uint8_t macCopy[6];
            memcpy(macCopy, addr.dstAddress, 6);
            PhyComAddress* phyAddr = PhyComAddress_create(addr.vlanPriority, addr.vlanId, addr.appId, macCopy);
            GSEControlBlock_addPhyComAddress(gcb, phyAddr);
        }

        if (dataSetRefCopy) {
            resolveAndBuildDataset(conn, ln, dataSetRefCopy, builtDatasets);
            free(dataSetRefCopy);
        }

        element = LinkedList_getNext(element);
    }
    LinkedList_destroyDeep(names, free);
}

/* ---- entry point ---- */

IedModel*
IedModelOnlineLoaderConnection_build(IedConnection conn, const char* iedName,
        const IedModelOnlineLoaderConfig* config, LinkedList* outLnCategories,
        IedModelOnlineLoaderError* outError) {
    if (outError) *outError = IED_MODEL_ONLINE_LOADER_OK;
    if (outLnCategories) *outLnCategories = NULL;
    (void) config; /* no per-call tuning beyond the request timeout, which the caller already
                       applies directly to `conn` before invoking this function */

    if (!conn) {
        if (outError) *outError = IED_MODEL_ONLINE_LOADER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    IedClientError err = IED_ERROR_OK;
    uint64_t ldListStartMs = Hal_getTimeInMs();
    LinkedList ldNames = IedConnection_getLogicalDeviceList(conn, &err);
    SS_LOG_DEBUG("[ied_model_online_loader] LD list -> %s, %d LD(s) (%llums)\n",
            ldNames ? "ok" : "failed", ldNames ? LinkedList_size(ldNames) : 0,
            (unsigned long long) (Hal_getTimeInMs() - ldListStartMs));
    if (!ldNames || LinkedList_size(ldNames) == 0) {
        if (ldNames) LinkedList_destroyDeep(ldNames, free);
        if (outError) *outError = IED_MODEL_ONLINE_LOADER_ERR_NO_LOGICAL_DEVICES;
        return NULL;
    }

    /* IedModel_create's own name is deliberately NOT `iedName` here (that's
     * only used to label the resulting IedModelHandle, via
     * IedModel_wrapDynamicModel - independent of this) - LogicalDevice_create
     * implicitly PREPENDS its parent IedModel's own name to whatever name
     * it's given to form the LD's real wire name (confirmed empirically:
     * feeding IedConnection_getLogicalDeviceList's already-fully-qualified
     * names, e.g. "Reporter1LD1", through LogicalDevice_create(name, model)
     * with model named "Reporter1" produced a corrupted double-prefixed wire
     * name, "Reporter1Reporter1LD1" - a THIRD dynamic-model construction
     * gotcha alongside the two already documented for DataSet/DataSetEntry).
     * Using an empty model name here sidesteps ever needing to know/derive
     * the server's true IED name at all - getLogicalDeviceList's names are
     * used completely unmodified as each LogicalDevice's own name, matching
     * the live server's wire naming exactly with no prepending. */
    IedModel* model = IedModel_create("");
    if (!model) {
        LinkedList_destroyDeep(ldNames, free);
        if (outError) *outError = IED_MODEL_ONLINE_LOADER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    /* Owned char* list of "LD/LN$dsName" references already turned into a
     * real DataSet - de-dupes across every RCB/GoCB in this whole build, not
     * just within one LN (mirrors mms_report_client's own per-connect-cycle
     * dynamicDatasetCache, see that feature's own doc comment). */
    LinkedList builtDatasets = LinkedList_create();

    /* One heap-boxed IedModelLnCategoryEntry* per LN built - see this
     * function's own doc comment (ied_model_online_loader_connection.h). */
    LinkedList lnCategories = LinkedList_create();

    LinkedList ldElement = LinkedList_getNext(ldNames);
    while (ldElement) {
        char* ldName = (char*) LinkedList_getData(ldElement);
        LogicalDevice* ld = LogicalDevice_create(ldName, model);

        IedClientError lnErr = IED_ERROR_OK;
        uint64_t lnDirStartMs = Hal_getTimeInMs();
        LinkedList lnNames = IedConnection_getLogicalDeviceDirectory(conn, &lnErr, ldName);
        SS_LOG_DEBUG("[ied_model_online_loader] LD '%s' LN directory -> %s, %d LN(s) (%llums)\n",
                ldName, lnNames ? "ok" : "failed", lnNames ? LinkedList_size(lnNames) : 0,
                (unsigned long long) (Hal_getTimeInMs() - lnDirStartMs));
        if (lnNames) {
            LinkedList lnElement = LinkedList_getNext(lnNames);
            while (lnElement) {
                char* lnName = (char*) LinkedList_getData(lnElement);
                LogicalNode* ln = LogicalNode_create(lnName, ld);
                char* lnRef = buildLnRef(ldName, lnName);

                if (lnCategories) {
                    /* No lnClass attribute is ever available here (only the
                     * raw concatenated wire instance name) - reverse-match
                     * via the same dictionary the SCL path's own
                     * lnClass="..." classification ultimately derives from.
                     * Never left unclassified: an unmatched name still
                     * yields OTHER, so this LN is filterable like any other. */
                    IedModelLnCategoryEntry* entry = malloc(sizeof(IedModelLnCategoryEntry));
                    if (entry) {
                        entry->ln = ln;
                        entry->category = IedModel_categorizeWireInstanceName(lnName);
                        entry->alwaysInclude = IedModel_isAlwaysIncludedWireInstanceName(lnName);
                        LinkedList_add(lnCategories, entry);
                    }
                }

                if (lnRef) {
                    uint64_t lnStartMs = Hal_getTimeInMs();
                    buildLogicalNodeDataModel(conn, lnRef, ln);
                    buildReportControlBlocks(conn, lnRef, ln, ACSI_CLASS_BRCB, true, builtDatasets);
                    buildReportControlBlocks(conn, lnRef, ln, ACSI_CLASS_URCB, false, builtDatasets);
                    buildGooseControlBlocks(conn, lnRef, ln, builtDatasets);
                    SS_LOG_DEBUG("[ied_model_online_loader] LN '%s' built (%llums)\n", lnRef,
                            (unsigned long long) (Hal_getTimeInMs() - lnStartMs));
                    free(lnRef);
                }

                lnElement = LinkedList_getNext(lnElement);
            }
            LinkedList_destroyDeep(lnNames, free);
        } else {
            SS_LOG_WARN("[ied_model_online_loader] WARN: getLogicalDeviceDirectory failed for '%s' "
                    "(error %d) - this logical device will have no logical nodes\n", ldName, lnErr);
        }

        ldElement = LinkedList_getNext(ldElement);
    }

    LinkedList_destroyDeep(ldNames, free);
    LinkedList_destroyDeep(builtDatasets, free);

    if (outLnCategories) *outLnCategories = lnCategories;
    else LinkedList_destroyDeep(lnCategories, free); /* caller doesn't want it - don't leak */

    return model;
}

IedModel*
IedModelOnlineLoaderConnection_connectAndBuild(const char* host, int port, const char* iedName,
        const char* acseAuthPassword, const IedModelOnlineLoaderConfig* config,
        LinkedList* outLnCategories, IedModelOnlineLoaderError* outError) {
    if (outError) *outError = IED_MODEL_ONLINE_LOADER_OK;
    if (outLnCategories) *outLnCategories = NULL;

    if (!host || !host[0] || port <= 0) {
        if (outError) *outError = IED_MODEL_ONLINE_LOADER_ERR_INVALID_ARGUMENT;
        return NULL;
    }

    IedConnection conn = IedConnection_createEx(NULL, true);
    if (!conn) {
        if (outError) *outError = IED_MODEL_ONLINE_LOADER_ERR_OUT_OF_MEMORY;
        return NULL;
    }

    if (config && config->requestTimeoutMs > 0) {
        IedConnection_setRequestTimeout(conn, config->requestTimeoutMs);
    }
    IedModelOnlineLoaderAuth_configurePasswordAuth(conn, acseAuthPassword);

    IedClientError connectErr = IED_ERROR_OK;
    uint64_t connectStartMs = Hal_getTimeInMs();
    IedConnection_connect(conn, &connectErr, host, port);
    SS_LOG_DEBUG("[ied_model_online_loader] mms connect %s:%d -> %s (%llums)\n", host, port,
            connectErr == IED_ERROR_OK ? "ok" : "failed",
            (unsigned long long) (Hal_getTimeInMs() - connectStartMs));
    if (connectErr != IED_ERROR_OK) {
        IedConnection_destroy(conn);
        if (outError) *outError = IED_MODEL_ONLINE_LOADER_ERR_CONNECT_FAILED;
        return NULL;
    }

    uint64_t buildStartMs = Hal_getTimeInMs();
    IedModel* model = IedModelOnlineLoaderConnection_build(conn, iedName, config, outLnCategories, outError);
    SS_LOG_DEBUG("[ied_model_online_loader] %s:%d discovery build done, model=%s (%llums)\n", host,
            port, model ? "ok" : "failed", (unsigned long long) (Hal_getTimeInMs() - buildStartMs));

    /* One-shot: nothing to keep this association open for once discovery is
     * done (or has failed) - unlike mms_report_client's long-lived
     * IedConnection, reused across reconnects for the daemon's whole
     * lifetime. */
    IedConnection_close(conn);
    IedConnection_destroy(conn);

    return model;
}
