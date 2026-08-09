#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "stdbool_compat.h"
#include "features/control_dispatcher/data/control_dispatcher_json_parser.h"
#include "orchestration/utils/orchestration_utils.h"

static void
freeRequestFields(ControlRequest* request) {
    free(request->requestId);
    free(request->host);
    free(request->iedName);
    free(request->interfaceId);
    free(request->sclFilePath);
    free(request->acseAuthPassword);
}

static bool
isNonEmptyString(const cJSON* item) {
    return item && cJSON_IsString(item) && item->valuestring && item->valuestring[0];
}

/* NULL if absent/wrong type - a present-but-null JSON value (sclFilePath/
 * acseAuthPassword's own documented "optional" case) is deliberately
 * indistinguishable from "field omitted entirely" here, matching this
 * feature's own envelope rules (both mean "not given"). */
static char*
dupOptionalString(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!isNonEmptyString(item)) return NULL;
    return OrchestrationUtils_safeStringDup(item->valuestring);
}

static bool
tryGetInt(const cJSON* object, const char* key, int* outValue) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item || !cJSON_IsNumber(item)) return false;
    *outValue = (int) item->valuedouble;
    return true;
}

static bool
parseAccessMode(const cJSON* object, AccessMode* outMode) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, "accessMode");
    if (!item) {
        *outMode = IED_MODEL_ACCESS_REPORT_ONLY;
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) return false;

    if (strcmp(item->valuestring, "REPORT_ONLY") == 0) *outMode = IED_MODEL_ACCESS_REPORT_ONLY;
    else if (strcmp(item->valuestring, "READ_ONLY") == 0) *outMode = IED_MODEL_ACCESS_READ_ONLY;
    else if (strcmp(item->valuestring, "READ_AND_WRITE") == 0) *outMode = IED_MODEL_ACCESS_READ_AND_WRITE;
    else return false;

    return true;
}

/*
 * Absent "lnCategories" -> IED_MODEL_LN_CATEGORY_ALL (today's unfiltered
 * behavior, fully backward compatible). Present: must be a non-empty array
 * of recognized category name strings ("CONTROL"/"MEASUREMENT"/"PROTECTION"/
 * "OTHER"), OR'd together into the returned mask. An empty array is
 * REJECTED as INVALID_PARAMS rather than treated as "no filter" - a caller
 * wanting no filter should omit the field entirely; an explicit empty array
 * is far more likely a client-side bug than genuine "subscribe to nothing"
 * intent (explicit product decision, not an oversight). Any non-string
 * element or unrecognized category name also fails closed, same as
 * parseAccessMode's own unrecognized-string handling. */
static bool
parseLnCategoryFilter(const cJSON* object, LnCategoryMask* outMask) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, "lnCategories");
    if (!item) {
        *outMask = IED_MODEL_LN_CATEGORY_ALL;
        return true;
    }
    if (!cJSON_IsArray(item)) return false;

    int size = cJSON_GetArraySize(item);
    if (size == 0) return false;

    LnCategoryMask mask = 0;
    for (int i = 0; i < size; i++) {
        const cJSON* element = cJSON_GetArrayItem(item, i);
        if (!cJSON_IsString(element) || !element->valuestring) return false;

        if (strcmp(element->valuestring, "CONTROL") == 0) mask |= IED_MODEL_LN_CATEGORY_CONTROL;
        else if (strcmp(element->valuestring, "MEASUREMENT") == 0) mask |= IED_MODEL_LN_CATEGORY_MEASUREMENT;
        else if (strcmp(element->valuestring, "PROTECTION") == 0) mask |= IED_MODEL_LN_CATEGORY_PROTECTION;
        else if (strcmp(element->valuestring, "OTHER") == 0) mask |= IED_MODEL_LN_CATEGORY_OTHER;
        else return false;
    }

    *outMask = mask;
    return true;
}

static ControlParseError
parseStartReportingParams(const cJSON* params, ControlRequest* request) {
    const cJSON* hostItem = cJSON_GetObjectItemCaseSensitive(params, "host");
    if (!isNonEmptyString(hostItem)) return CONTROL_PARSE_ERR_INVALID_PARAMS;
    request->host = OrchestrationUtils_safeStringDup(hostItem->valuestring);

    const cJSON* interfaceItem = cJSON_GetObjectItemCaseSensitive(params, "interfaceId");
    if (!isNonEmptyString(interfaceItem)) return CONTROL_PARSE_ERR_INVALID_PARAMS;
    request->interfaceId = OrchestrationUtils_safeStringDup(interfaceItem->valuestring);

    request->mmsPort = 102;
    tryGetInt(params, "mmsPort", &request->mmsPort);

    request->iedName = dupOptionalString(params, "iedName");
    request->sclFilePath = dupOptionalString(params, "sclFilePath");
    request->acseAuthPassword = dupOptionalString(params, "acseAuthPassword");

    /* Mandatory whenever sclFilePath is given - device_manager itself also
     * validates this, but failing fast here gives a clearer parse-time error
     * (INVALID_PARAMS) instead of reaching device_manager only to be
     * rejected there too. */
    if (request->sclFilePath && !request->iedName) return CONTROL_PARSE_ERR_INVALID_PARAMS;

    if (!parseAccessMode(params, &request->accessMode)) return CONTROL_PARSE_ERR_INVALID_PARAMS;
    if (!parseLnCategoryFilter(params, &request->lnCategoryFilter)) return CONTROL_PARSE_ERR_INVALID_PARAMS;

    if (!request->host || !request->interfaceId) return CONTROL_PARSE_ERR_INVALID_PARAMS; /* OOM */

    return CONTROL_PARSE_OK;
}

static ControlParseError
parseStopReportingParams(const cJSON* params, ControlRequest* request) {
    const cJSON* deviceIdItem = cJSON_GetObjectItemCaseSensitive(params, "deviceId");
    if (deviceIdItem) {
        if (!cJSON_IsNumber(deviceIdItem) || deviceIdItem->valuedouble < 0) {
            return CONTROL_PARSE_ERR_INVALID_PARAMS;
        }
        request->deviceId = (uint64_t) deviceIdItem->valuedouble;
        return CONTROL_PARSE_OK;
    }

    /* No deviceId - fall back to the (host, mmsPort) form, for a caller that
     * never obtained or has lost track of a deviceId (see
     * DeviceManager_stopReportingByAddress's own doc comment for why this
     * exists). control_dispatcher_usecases.c discriminates on request->host
     * being non-NULL - the two forms are mutually exclusive by construction,
     * this branch never also sets deviceId. */
    const cJSON* hostItem = cJSON_GetObjectItemCaseSensitive(params, "host");
    if (!isNonEmptyString(hostItem)) return CONTROL_PARSE_ERR_INVALID_PARAMS;
    request->host = OrchestrationUtils_safeStringDup(hostItem->valuestring);

    request->mmsPort = 102;
    tryGetInt(params, "mmsPort", &request->mmsPort);

    if (!request->host) return CONTROL_PARSE_ERR_INVALID_PARAMS; /* OOM */

    return CONTROL_PARSE_OK;
}

static ControlParseError
parseStartScanParams(const cJSON* params, ControlRequest* request) {
    const cJSON* interfaceItem = cJSON_GetObjectItemCaseSensitive(params, "interfaceId");
    if (!isNonEmptyString(interfaceItem)) return CONTROL_PARSE_ERR_INVALID_PARAMS;
    request->interfaceId = OrchestrationUtils_safeStringDup(interfaceItem->valuestring);

    request->mmsPort = 102;
    tryGetInt(params, "mmsPort", &request->mmsPort);

    int sweepIntervalMs = 0;
    tryGetInt(params, "sweepIntervalMs", &sweepIntervalMs);
    request->sweepIntervalMs = (sweepIntervalMs > 0) ? (uint32_t) sweepIntervalMs : 0;

    if (!request->interfaceId) return CONTROL_PARSE_ERR_INVALID_PARAMS; /* OOM */

    return CONTROL_PARSE_OK;
}

static ControlParseError
parseStopScanParams(const cJSON* params, ControlRequest* request) {
    const cJSON* scanIdItem = cJSON_GetObjectItemCaseSensitive(params, "scanId");
    if (!scanIdItem || !cJSON_IsNumber(scanIdItem) || scanIdItem->valuedouble < 0) {
        return CONTROL_PARSE_ERR_INVALID_PARAMS;
    }
    request->scanId = (uint64_t) scanIdItem->valuedouble;

    return CONTROL_PARSE_OK;
}

ControlParseError
ControlDispatcherJsonParser_parse(const char* rawJson, size_t len, char** outRecoveredRequestId,
        char** outRecoveredAction, ControlRequest** outRequest) {
    if (outRecoveredRequestId) *outRecoveredRequestId = NULL;
    if (outRecoveredAction) *outRecoveredAction = NULL;
    if (outRequest) *outRequest = NULL;

    cJSON* root = cJSON_ParseWithLength(rawJson, len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return CONTROL_PARSE_ERR_MALFORMED_JSON;
    }

    const cJSON* requestIdItem = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    if (isNonEmptyString(requestIdItem) && outRecoveredRequestId) {
        *outRecoveredRequestId = OrchestrationUtils_safeStringDup(requestIdItem->valuestring);
    }

    const cJSON* actionItem = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (cJSON_IsString(actionItem) && actionItem->valuestring && outRecoveredAction) {
        *outRecoveredAction = OrchestrationUtils_safeStringDup(actionItem->valuestring);
    }

    if (!isNonEmptyString(requestIdItem)) {
        cJSON_Delete(root);
        return CONTROL_PARSE_ERR_MISSING_REQUEST_ID;
    }

    ControlRequestType type;
    const char* actionStr = (cJSON_IsString(actionItem) && actionItem->valuestring) ? actionItem->valuestring : NULL;
    if (actionStr && strcmp(actionStr, "START_REPORTING") == 0) {
        type = CONTROL_REQ_START_REPORTING;
    } else if (actionStr && strcmp(actionStr, "STOP_REPORTING") == 0) {
        type = CONTROL_REQ_STOP_REPORTING;
    } else if (actionStr && strcmp(actionStr, "START_SCAN") == 0) {
        type = CONTROL_REQ_START_SCAN;
    } else if (actionStr && strcmp(actionStr, "STOP_SCAN") == 0) {
        type = CONTROL_REQ_STOP_SCAN;
    } else {
        cJSON_Delete(root);
        return CONTROL_PARSE_ERR_UNKNOWN_ACTION;
    }

    const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!params || !cJSON_IsObject(params)) {
        cJSON_Delete(root);
        return CONTROL_PARSE_ERR_INVALID_PARAMS;
    }

    ControlRequest* request = calloc(1, sizeof(ControlRequest));
    if (!request) {
        cJSON_Delete(root);
        return CONTROL_PARSE_ERR_INVALID_PARAMS; /* OOM - no dedicated code, extremely unlikely */
    }
    request->requestId = OrchestrationUtils_safeStringDup(requestIdItem->valuestring);
    request->type = type;

    ControlParseError paramsErr;
    switch (type) {
        case CONTROL_REQ_START_REPORTING: paramsErr = parseStartReportingParams(params, request); break;
        case CONTROL_REQ_STOP_REPORTING: paramsErr = parseStopReportingParams(params, request); break;
        case CONTROL_REQ_START_SCAN: paramsErr = parseStartScanParams(params, request); break;
        default: paramsErr = parseStopScanParams(params, request); break; /* CONTROL_REQ_STOP_SCAN */
    }

    cJSON_Delete(root);

    if (paramsErr != CONTROL_PARSE_OK || !request->requestId) {
        freeRequestFields(request);
        free(request);
        return paramsErr != CONTROL_PARSE_OK ? paramsErr : CONTROL_PARSE_ERR_INVALID_PARAMS;
    }

    /* Success: the request itself carries requestId; recovered copies made
     * above for the error-echo path are no longer needed. */
    if (outRecoveredRequestId) {
        free(*outRecoveredRequestId);
        *outRecoveredRequestId = NULL;
    }
    if (outRecoveredAction) {
        free(*outRecoveredAction);
        *outRecoveredAction = NULL;
    }

    if (outRequest) {
        *outRequest = request;
    } else {
        freeRequestFields(request);
        free(request);
    }

    return CONTROL_PARSE_OK;
}
