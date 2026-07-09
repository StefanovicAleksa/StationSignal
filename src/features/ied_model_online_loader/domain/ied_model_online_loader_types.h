#ifndef IED_MODEL_ONLINE_LOADER_TYPES_H_
#define IED_MODEL_ONLINE_LOADER_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"

/*
 * Domain vocabulary for this feature is a mix of libiec61850's MMS-client
 * vocabulary (IedClientError, reached only through the one opaque
 * IedConnection the caller hands in) and ied_model's own dynamic-model
 * vocabulary (IedModel) - this feature's whole job is bridging live MMS
 * directory/model-discovery services into a dynamic IedModel indistinguishable
 * from one built by ied_model_scl_loader. See CLAUDE.md's "No over-the-wire
 * tree discovery" Hard Rule for why this is a narrow, explicit exception
 * rather than a general capability.
 */

typedef enum {
    IED_MODEL_ONLINE_LOADER_OK = 0,
    IED_MODEL_ONLINE_LOADER_ERR_INVALID_ARGUMENT,
    IED_MODEL_ONLINE_LOADER_ERR_CONNECT_FAILED,     /* IedConnection_connect itself failed - this
                                                        feature owns its own one-shot connection
                                                        (connect -> discover -> disconnect), same
                                                        convention as scl_bootstrap's own MMS
                                                        session, never a caller-supplied connection */
    IED_MODEL_ONLINE_LOADER_ERR_NO_LOGICAL_DEVICES, /* GetLogicalDeviceList succeeded but returned
                                                        zero LDs - a genuinely empty/misbehaving
                                                        server, not this loader's fault to paper
                                                        over */
    IED_MODEL_ONLINE_LOADER_ERR_OUT_OF_MEMORY
} IedModelOnlineLoaderError;

typedef struct {
    uint32_t requestTimeoutMs; /* 0 = leave IedConnection's own default untouched. This loader
                                  issues many more sequential MMS requests than a single SCL file
                                  fetch would - see CLAUDE.md's documented tradeoff for this
                                  fallback path. */
} IedModelOnlineLoaderConfig;

#endif /* IED_MODEL_ONLINE_LOADER_TYPES_H_ */
