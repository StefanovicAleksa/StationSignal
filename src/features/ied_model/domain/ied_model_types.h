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
};

typedef struct sIedModelHandle* IedModelHandle;

#endif /* IED_MODEL_TYPES_H_ */
