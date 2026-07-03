#ifndef ORCHESTRATION_TYPES_H_
#define ORCHESTRATION_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "linked_list.h"
#include "features/scl_bootstrap/service/scl_bootstrap_api.h"
#include "features/ied_model/service/ied_model_api.h"
#include "features/mms_report_client/service/mms_report_client_api.h"
#include "features/goose_subscriber/service/goose_subscriber_api.h"
#include "features/ipc_dispatcher/service/ipc_dispatcher_api.h"

/*
 * Domain vocabulary for this feature is entirely our own sibling features'
 * public APIs (scl_bootstrap, ied_model, mms_report_client, goose_subscriber,
 * ipc_dispatcher) - never their domain/data headers directly, same rule every
 * feature's own domain layer states about itself. Unlike ied_model/
 * mms_report_client/goose_subscriber, this layer has zero direct third-party
 * includes of its own (only transitively, via the sibling _api.h headers) -
 * orchestration's job is sequencing already-implemented features, not
 * talking to libiec61850/libwebsockets/cJSON itself.
 */

typedef enum {
    ORCHESTRATION_OK = 0,
    ORCHESTRATION_ERR_INVALID_ARGUMENT,
    ORCHESTRATION_ERR_OUT_OF_MEMORY,
    ORCHESTRATION_ERR_IPC_DISPATCHER_FAILED,
    ORCHESTRATION_ERR_BOOTSTRAP_FAILED,
    ORCHESTRATION_ERR_STAGING_FAILED,
    ORCHESTRATION_ERR_MODEL_LOAD_FAILED,
    ORCHESTRATION_ERR_REPORT_CLIENT_FAILED,
    ORCHESTRATION_ERR_GOOSE_SUBSCRIBER_FAILED
} OrchestrationError;

typedef enum {
    ORCHESTRATION_STAGE_NONE = 0,   /* success, or failure before any stage began (bad args) */
    ORCHESTRATION_STAGE_IPC_DISPATCHER_START, /* started first, deliberately - a websocket bind
                                                  failure should fail fast, before touching the
                                                  network-facing MMS/GOOSE side at all */
    ORCHESTRATION_STAGE_BOOTSTRAP,
    ORCHESTRATION_STAGE_STAGING,
    ORCHESTRATION_STAGE_MODEL_LOAD,
    ORCHESTRATION_STAGE_REPORT_CLIENT_START,
    ORCHESTRATION_STAGE_GOOSE_SUBSCRIBER_START
} OrchestrationStage;

/*
 * Which stage failed, plus the underlying feature's own error code for that
 * stage - mirrors SclBootstrapResult's own status+lastMmsError two-part
 * pattern rather than inventing new error vocabulary. Only the field(s)
 * matching .stage are meaningful; the rest are zero-valued.
 */
typedef struct {
    OrchestrationStage stage;
    IpcDispatcherError ipcDispatcherError;              /* stage==IPC_DISPATCHER_START */
    SclBootstrapError bootstrapArgError;              /* stage==BOOTSTRAP, scanAndFetch itself
                                                           returned NULL (bad args/OOM) */
    SclBootstrapCandidateStatus lastCandidateStatus;   /* stage==BOOTSTRAP, scanAndFetch succeeded
                                                           but no candidate reached FILE_RETRIEVED -
                                                           status of the last candidate scanned,
                                                           diagnostics only */
    int stagingErrno;                                  /* stage==STAGING */
    IedModelLoadError modelLoadError;                  /* stage==MODEL_LOAD */
    MmsReportClientError reportClientError;            /* stage==REPORT_CLIENT_START */
    GooseSubscriberError gooseSubscriberError;         /* stage==GOOSE_SUBSCRIBER_START */
} OrchestrationErrorDetail;

typedef struct {
    IpcDispatcherConfig ipcDispatcherConfig;    /* port defaults to 8765 via
                                                    IpcDispatcherConfig_defaults - override
                                                    before Orchestration_create if needed */
    SclBootstrapConfig bootstrapConfig;         /* .acseAuthPassword stays borrowed - same
                                                    convention as SclBootstrapConfig itself,
                                                    caller must keep it alive for the
                                                    OrchestrationHandle's whole lifetime */
    MmsReportClientConfig reportClientConfig;
    GooseSubscriberConfig gooseSubscriberConfig;
} OrchestrationConfig;

/*
 * Internal representation. Defined here (rather than hidden behind an
 * additional internal-only header) because every file within this feature
 * needs field access, mirroring every sibling feature's struct s*Handle
 * convention: opacity is enforced by which header is exposed
 * (service/orchestration_api.h is the only public one), not by hiding the
 * struct.
 */
struct sOrchestrationHandle {
    OrchestrationConfig config;

    IpcDispatcherHandle ipcDispatcher;       /* owned for the handle's whole lifetime - created
                                                 (alloc only, no I/O) in Orchestration_create,
                                                 started as stage 0 of Orchestration_run, stopped
                                                 in Orchestration_stop, destroyed in
                                                 Orchestration_destroy. Unlike iedModel/
                                                 reportClient/gooseSubscriber, this is never NULL
                                                 after a successful _create - main.c (or any
                                                 caller) has no other way to reach ipc_dispatcher,
                                                 by design: report/GOOSE data records are always
                                                 relayed there, not caller-configurable. */
    IedModelHandle iedModel;                 /* owned once _run succeeds, else NULL */
    MmsReportClientHandle reportClient;      /* owned once _run succeeds, else NULL */
    GooseSubscriberHandle gooseSubscriber;   /* owned once _run succeeds, else NULL */

    /* Report/GOOSE DATA record callbacks are deliberately not caller-settable
     * here (unlike the diagnostics below) - Orchestration_run always wires
     * IpcDispatcher_onMmsReport/_onGooseRecord onto reportClient/
     * gooseSubscriber directly, since exactly one consumer may ever own+
     * destroy a given record and ipc_dispatcher is that consumer. */
    MmsReportClientConnStateCallback connStateCallback;
    void* connStateCallbackParam;
    MmsReportClientRcbStatusCallback rcbStatusCallback;
    void* rcbStatusCallbackParam;
    GooseSubscriberStatusCallback gooseStatusCallback;
    void* gooseStatusCallbackParam;
    SclBootstrapProgressCallback bootstrapProgressCallback;
    void* bootstrapProgressCallbackParam;

    volatile bool running;
};

typedef struct sOrchestrationHandle* OrchestrationHandle;

#endif /* ORCHESTRATION_TYPES_H_ */
