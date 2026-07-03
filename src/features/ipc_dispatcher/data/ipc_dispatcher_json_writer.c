#include "cJSON.h"
#include "features/ipc_dispatcher/data/ipc_dispatcher_json_writer.h"

static const char*
sourceTypeTag(IpcSourceType type) {
    return (type == IPC_SOURCE_GOOSE) ? "GOOSE" : "MMS_REPORT";
}

static const char*
validityString(IpcQualityValidity validity) {
    switch (validity) {
        case IPC_QUALITY_GOOD: return "GOOD";
        case IPC_QUALITY_INVALID: return "INVALID";
        case IPC_QUALITY_RESERVED: return "RESERVED";
        case IPC_QUALITY_QUESTIONABLE: return "QUESTIONABLE";
        default: return "INVALID";
    }
}

static cJSON*
buildScalarValueJson(const IpcScalarValue* value) {
    switch (value->type) {
        case IPC_SCALAR_BOOL: return cJSON_CreateBool(value->value.b);
        case IPC_SCALAR_INT64: return cJSON_CreateNumber((double) value->value.i64);
        case IPC_SCALAR_UINT64: return cJSON_CreateNumber((double) value->value.u64);
        case IPC_SCALAR_DOUBLE: return cJSON_CreateNumber(value->value.d);
        case IPC_SCALAR_STRING:
        case IPC_SCALAR_RAW:
        default:
            return cJSON_CreateString(value->value.str ? value->value.str : "");
    }
}

static cJSON*
buildQualityJson(const IpcDataPoint* point) {
    if (!point->hasQuality) return cJSON_CreateNull();

    cJSON* quality = cJSON_CreateObject();
    if (!quality) return NULL;

    cJSON_AddStringToObject(quality, "validity", validityString(point->quality.validity));
    cJSON_AddNumberToObject(quality, "detailFlags", (double) point->quality.detailFlags);
    return quality;
}

static cJSON*
buildDataPointJson(const IpcDataPoint* point) {
    cJSON* obj = cJSON_CreateObject();
    if (!obj) return NULL;

    if (point->reference) cJSON_AddStringToObject(obj, "reference", point->reference);
    else cJSON_AddNullToObject(obj, "reference");

    cJSON* value = buildScalarValueJson(&point->value);
    cJSON_AddItemToObject(obj, "value", value ? value : cJSON_CreateNull());

    cJSON* quality = buildQualityJson(point);
    cJSON_AddItemToObject(obj, "quality", quality ? quality : cJSON_CreateNull());

    return obj;
}

static cJSON*
buildSourceJson(const IpcMessage* message) {
    cJSON* source = cJSON_CreateObject();
    if (!source) return NULL;

    if (message->sourceType == IPC_SOURCE_GOOSE) {
        if (message->sourceReference) cJSON_AddStringToObject(source, "goCbRef", message->sourceReference);
        else cJSON_AddNullToObject(source, "goCbRef");
    } else {
        if (message->sourceReference) cJSON_AddStringToObject(source, "rcbReference", message->sourceReference);
        else cJSON_AddNullToObject(source, "rcbReference");
        if (message->hasBuffered) cJSON_AddBoolToObject(source, "buffered", message->buffered);
    }

    return source;
}

char*
IpcDispatcherJsonWriter_write(const IpcMessage* message) {
    if (!message) return NULL;

    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "schemaVersion", 1);
    cJSON_AddStringToObject(root, "type", sourceTypeTag(message->sourceType));

    cJSON* source = buildSourceJson(message);
    cJSON_AddItemToObject(root, "source", source ? source : cJSON_CreateNull());

    cJSON_AddBoolToObject(root, "hasTimestamp", message->hasTimestamp);
    if (message->hasTimestamp) {
        cJSON_AddNumberToObject(root, "timestampMs", (double) message->timestampMs);
    }

    cJSON* dataPoints = cJSON_CreateArray();
    if (dataPoints) {
        for (int i = 0; i < message->dataPointCount; i++) {
            cJSON* point = buildDataPointJson(&message->dataPoints[i]);
            if (point) cJSON_AddItemToArray(dataPoints, point);
        }
        cJSON_AddItemToObject(root, "dataPoints", dataPoints);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return json;
}
