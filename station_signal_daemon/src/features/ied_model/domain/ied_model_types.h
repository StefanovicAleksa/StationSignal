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
 * App-level bucketing of IEC 61850-7-4 LN classes by their group letter (the
 * first character of the LN class name, e.g. 'X' from "XCBR") - NOT a
 * standard IEC 61850 concept itself, a curated grouping layered on top of it
 * (see IedModelLnCategory_forGroupLetter's implementation for the exact
 * letter->category table). One LN always classifies to exactly one of
 * these - never a mask/OR of multiple, unlike LnCategoryMask below, which is
 * why this is a plain enum instead of also being bit-valued... except it
 * IS bit-valued (single set bit per value) purely so a single
 * "categoryFilter & category" test (LnCategoryMask & LnCategory) works
 * without a separate switch/lookup at every filter-check call site.
 */
typedef enum {
    IED_MODEL_LN_CATEGORY_CONTROL     = 1 << 0, /* group C, X, A - supervisory/automatic control, switchgear */
    IED_MODEL_LN_CATEGORY_MEASUREMENT = 1 << 1, /* group M, T - metering/measurement, instrument transformers */
    IED_MODEL_LN_CATEGORY_PROTECTION  = 1 << 2, /* group P, R - protection, protection-related */
    IED_MODEL_LN_CATEGORY_OTHER       = 1 << 3  /* group G, I, L, S, Y, Z, or unrecognized/non-standard */
} LnCategory;

/* OR of LnCategory bits - a caller's subscription filter selection, distinct
 * from LnCategory (one LN's own single classification) even though they
 * share a representation - named separately so call sites read as intent
 * ("this is a filter mask", not "this is one LN's category"). */
typedef uint8_t LnCategoryMask;

/* Matches every LnCategory - the default filter, equivalent to today's
 * unfiltered behavior. Every IedModelHandle construction site passes this
 * (or a narrower mask) explicitly - never silently defaulted deep inside the
 * handle, same posture as AccessMode. */
#define IED_MODEL_LN_CATEGORY_ALL \
    (IED_MODEL_LN_CATEGORY_CONTROL | IED_MODEL_LN_CATEGORY_MEASUREMENT | \
     IED_MODEL_LN_CATEGORY_PROTECTION | IED_MODEL_LN_CATEGORY_OTHER)

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
 * One {LogicalNode*, category} pair, mirroring IedModelDaSemanticEntry's own
 * shape/ownership (`ln` BORROWED, owned by the wrapped IedModel). Collected
 * while parsing (ied_model_scl_loader.c, SCL's own lnClass="..." attribute -
 * see IedModelSclLoader_load's outLnCategories parameter) or while walking a
 * live device's MMS ACSI directory (ied_model_online_loader, reverse-matched
 * from the raw wire LN instance name via
 * IedModelLnCategory_forWireInstanceName - see that function's own doc
 * comment) into a LinkedList, then copied by value into the flat array on
 * sIedModelHandle below. Unlike daSemantics, this is NEVER empty for a
 * IedModel_wrapDynamicModel-built handle - every LN online discovery
 * encounters gets classified (best-effort OTHER on no match), since leaving
 * it empty would make the whole category filter inert on that path.
 */
typedef struct {
    LogicalNode* ln;
    LnCategory category;
} IedModelLnCategoryEntry;

/*
 * One {DataAttribute*, desc} pair - the DA's own SCL desc="..." human-readable
 * label, captured at both the <DA>/<BDA> template level and the <DAI>
 * instance-override level (instance wins when both are present, same
 * precedence already used for Val overrides in
 * applyDoiDaiOverrides/applyOverridesUnderDataObject). `da` is BORROWED, same
 * as IedModelDaSemanticEntry; `desc` is an OWNED copy (a real per-file
 * string, not a fixed enum-backed literal like LnCategory). Always empty for
 * a IedModel_wrapDynamicModel-built handle - MMS ACSI directory services
 * carry no SCL desc equivalent, nothing to recover on that path (mirrors
 * daSemantics's own "no SCL bType ever available over the wire" limitation).
 */
typedef struct {
    DataAttribute* da;
    char* desc;
} IedModelDaDescEntry;

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

    /* SCL's <Services><DynDataSet max="N" maxAttributes="M"/> for this IED,
     * used by mms_report_client to budget/chunk dynamic dataset creation
     * against the device's own declared caps. -1 = not declared (no
     * <Services>, no <DynDataSet> child, or a model built via
     * IedModel_wrapDynamicModel - no <Services> is ever available on that
     * path). 0 is a real, distinct value (device declares zero capacity) -
     * never conflated with -1. */
    int dynDataSetMax;
    int dynDataSetMaxAttributes;

    /* Same shape as dynDataSetMax/dynDataSetMaxAttributes above, but for
     * <Services><ConfDataSet max="N" maxAttributes="M"/> - the domain-scoped
     * ("Conf") dataset pool, distinct from DynDataSet's association-scoped
     * ("Dyn") one. mms_report_client budgets/sizes buffered-target self-created
     * datasets (always Conf-class) and its own domain-scoped fallback for
     * unbuffered targets (also Conf-class, when association-specific creation
     * is rejected) against this pool instead of DynDataSet's. */
    int confDataSetMax;
    int confDataSetMaxAttributes;

    /* Flat array of {LogicalNode*, category} pairs - see
     * IedModelLnCategoryEntry's own doc comment. Unlike daSemantics, always
     * populated regardless of construction path (SCL or online-discovery). */
    IedModelLnCategoryEntry* lnCategories;
    int lnCategoryCount;

    /* Flat array of {DataAttribute*, desc} pairs - see IedModelDaDescEntry's
     * own doc comment. Empty for a IedModel_wrapDynamicModel-built handle. */
    IedModelDaDescEntry* daDescriptions;
    int daDescriptionCount;

    /* Active category filter. Deliberately does NOT gate
     * IedModel_get{Goose,Report}SubscriptionTargets - every RCB/GoCB is
     * always visible regardless of category (see those functions' own doc
     * comments in ied_model_usecases.c for why: real SCL very commonly
     * parents every RCB/GoCB on LLN0, which would make a parent-LN gate
     * useless in practice on such hardware). It DOES gate
     * _getReportableAttributeReferencesForWholeDevice (which leaves a
     * self-created dynamic dataset captures) and is exposed via
     * IedModel_getCategoryFilter so mms_report_client/goose_subscriber can
     * cache it once per connection and filter individual data points by
     * their OWN LN's category before forwarding. Default
     * IED_MODEL_LN_CATEGORY_ALL preserves pre-filter unfiltered behavior -
     * every construction site (IedModel_loadFromFile/_wrapDynamicModel)
     * passes this explicitly, mirroring accessMode above. */
    LnCategoryMask categoryFilter;
};

typedef struct sIedModelHandle* IedModelHandle;

#endif /* IED_MODEL_TYPES_H_ */
