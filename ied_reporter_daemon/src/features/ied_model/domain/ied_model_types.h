#ifndef IED_MODEL_TYPES_H_
#define IED_MODEL_TYPES_H_

#include <stdint.h>

#include "iec61850_model.h"

/*
 * Deliberate deviation from framework-free domain layers: this feature's
 * domain vocabulary IS libiec61850's IEC 61850 model vocabulary (IedModel,
 * FunctionalConstraint, DataAttributeType, ...). These aren't swappable
 * infrastructure for this project - they're the domain itself. See the
 * plan/CLAUDE.md for the reasoning.
 */

/*
 * Capability tiers exposed by the service layer. Always a hierarchy:
 * REPORT_ONLY subset of READ_ONLY subset of READ_AND_WRITE. The loader
 * always builds the complete model regardless of mode - only the service
 * layer's accessors gate on it.
 */
typedef enum {
    IED_MODEL_ACCESS_REPORT_ONLY,     /* GOOSE + report-control subscription targets only */
    IED_MODEL_ACCESS_READ_ONLY,       /* + on-demand MMS read targets */
    IED_MODEL_ACCESS_READ_AND_WRITE   /* + IEC 61850 Control Service targets (Select/Operate) */
} AccessMode;

typedef enum {
    IED_MODEL_OK = 0,
    IED_MODEL_ERR_FILE_NOT_FOUND,
    IED_MODEL_ERR_XML_PARSE,
    IED_MODEL_ERR_IED_NOT_FOUND,
    IED_MODEL_ERR_UNRESOLVED_TYPE,
    IED_MODEL_ERR_OUT_OF_MEMORY
} IedModelLoadError;

/*
 * A DataAttribute's real SCL-authored semantic meaning, beyond what
 * DataAttributeType alone can express - specifically, IedModelUtils_mapBType
 * collapses both "Dbpos" and "Tcmd" SCL bTypes into one generic
 * IEC61850_CODEDENUM DataAttributeType (same 2-bit wire representation, per
 * IEC 61850-7-3, but different meaning), discarding which one it actually
 * was. This is captured separately (see IedModelDaSemanticEntry) so a
 * consumer (ipc_dispatcher, via mms_report_client/goose_subscriber) can
 * label a genuine Dbpos value (0=intermediate-state, 1=off, 2=on,
 * 3=bad-state) without guessing from the bitstring alone. Room to extend
 * with IED_MODEL_DA_SEMANTIC_TCMD later - not implemented now, no evidence
 * yet that reporting output needs it.
 */
typedef enum {
    IED_MODEL_DA_SEMANTIC_NONE = 0,
    IED_MODEL_DA_SEMANTIC_DBPOS
} IedModelDaSemantic;

/*
 * One {DataAttribute*, semantic} pair. `da` is a BORROWED pointer, owned by
 * the IedModel this entry's handle wraps - stable for the handle's whole
 * lifetime, never freed independently of the model itself. Used both as the
 * boxed heap item ied_model_scl_loader.c collects into a LinkedList while
 * parsing (see IedModelSclLoader_load's outDaSemantics parameter) and as the
 * flat array element type on sIedModelHandle below (copied by value out of
 * that LinkedList once, at load time).
 */
typedef struct {
    DataAttribute* da;
    IedModelDaSemantic semantic;
} IedModelDaSemanticEntry;

/*
 * A single Report Control Block (buffered=BRCB or unbuffered=URCB) discovered
 * from SCL, enriched enough for a consumer (mms_report_client) to enable
 * reporting on it without re-deriving anything from the raw
 * ReportControlBlock model node itself. Heap-allocated; caller owns via
 * LinkedList_destroyDeep(list, IedModel_destroyReportControlBlockTarget).
 */
typedef struct {
    char* objectReference;  /* e.g. "Breaker1CB1/LLN0.BR.brcbMain" - owned copy.
                                ".RP." for unbuffered, ".BR." for buffered, per
                                IedConnection_getRCBValues's documented
                                convention (iec61850_client.h). */
    bool buffered;
    char* datasetReference; /* e.g. "Breaker1CB1/LLN0$ds1" - owned copy, or NULL
                                if the RCB's dataSetName was empty. */
    char* lnReference;      /* e.g. "Breaker1CB1/LLN0" - owned copy, the RCB's
                                own parent LN's object reference. Always
                                present (every RCB has a parent LN). Used by
                                mms_report_client to derive a dynamic dataset's
                                member list (via
                                IedModel_getReportableAttributeReferencesForLogicalNode)
                                when datasetReference is NULL - see that
                                function's own doc comment. */
} ReportControlBlockTarget;

/*
 * A single GOOSE Control Block (GoCB) discovered from SCL, enriched with its
 * publisher-side communication addressing (VLAN/APPID/dst-MAC) when SCL
 * provided a matching <Communication>/<SubNetwork>/<ConnectedAP>/<GSE><Address>
 * block - the same PhyComAddress data ied_model_scl_loader.c already attaches
 * to the internal GSEControlBlock via GSEControlBlock_addPhyComAddress, now
 * surfaced publicly instead of requiring a caller to reach into
 * handle->model->gseCBs directly. Heap-allocated; caller owns via
 * LinkedList_destroyDeep(list, IedModel_destroyGooseSubscriptionTarget).
 */
typedef struct {
    char* objectReference;  /* e.g. "Breaker1CB1/LLN0$GO$gcbStatus" - owned copy */
    char* datasetReference; /* e.g. "Breaker1CB1/LLN0$ds1" - owned copy, or NULL
                                if the GCB's dataSetName was empty. Mirrors
                                ReportControlBlockTarget.datasetReference. */
    bool hasAddress;         /* true only if SCL had a matching <GSE><Address> */
    uint16_t vlanId;         /* valid only if hasAddress */
    uint8_t vlanPriority;    /* valid only if hasAddress */
    uint16_t appId;          /* valid only if hasAddress */
    uint8_t dstMac[6];       /* valid only if hasAddress */
} GooseSubscriptionTarget;

/*
 * Internal representation. Defined here (rather than hidden behind an
 * additional internal-only header) because every file within this feature
 * needs field access and only service/ied_model_api.h is the public
 * boundary - external callers only ever hold the pointer, never dereference
 * it, so opacity is enforced by convention/API surface, not by hiding the
 * struct.
 */
struct sIedModelHandle {
    IedModel* model;
    AccessMode accessMode;
    char* iedName; /* owned copy */

    /* Flat array of {DataAttribute*, semantic} pairs, only entries with a
     * non-NONE semantic (small N - a linear scan at cache-build time, once
     * per RCB/target enable, is not a hot path). Owned array (see
     * IedModel_release), but each entry's `da` pointer is borrowed - see
     * IedModelDaSemanticEntry's own doc comment. Empty
     * (daSemanticCount == 0, daSemantics == NULL) for models built via
     * IedModel_wrapDynamicModel - no SCL bType is ever available over the
     * wire on that path, an already-accepted limitation, not a regression. */
    IedModelDaSemanticEntry* daSemantics;
    int daSemanticCount;
};

typedef struct sIedModelHandle* IedModelHandle;

#endif /* IED_MODEL_TYPES_H_ */
