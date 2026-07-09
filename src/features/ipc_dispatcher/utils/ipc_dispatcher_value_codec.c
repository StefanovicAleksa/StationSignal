#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "features/ipc_dispatcher/utils/ipc_dispatcher_value_codec.h"

static char*
dupString(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

static IpcScalarValue
rawPlaceholder(const char* text) {
    IpcScalarValue scalar;
    scalar.type = IPC_SCALAR_RAW;
    scalar.value.str = dupString(text);
    return scalar;
}

IpcScalarValue
IpcDispatcherValueCodec_convert(const MmsValue* value) {
    if (!value) return rawPlaceholder("<null>");

    MmsValue* mutableValue = (MmsValue*) value; /* MmsValue_toString/_getTypeString require non-const */
    IpcScalarValue scalar;

    switch (MmsValue_getType(value)) {
        case MMS_BOOLEAN:
            scalar.type = IPC_SCALAR_BOOL;
            scalar.value.b = MmsValue_getBoolean(value);
            return scalar;

        case MMS_INTEGER:
            scalar.type = IPC_SCALAR_INT64;
            scalar.value.i64 = MmsValue_toInt64(value);
            return scalar;

        case MMS_UNSIGNED:
            scalar.type = IPC_SCALAR_UINT64;
            scalar.value.u64 = (uint64_t) MmsValue_toUint32(value);
            return scalar;

        case MMS_FLOAT:
            scalar.type = IPC_SCALAR_DOUBLE;
            scalar.value.d = MmsValue_toDouble(value);
            return scalar;

        case MMS_VISIBLE_STRING:
        case MMS_STRING:
            scalar.type = IPC_SCALAR_STRING;
            scalar.value.str = dupString(MmsValue_toString(mutableValue));
            return scalar;

        case MMS_UTC_TIME:
            scalar.type = IPC_SCALAR_UINT64;
            scalar.value.u64 = MmsValue_getUtcTimeInMs(value);
            return scalar;

        case MMS_BIT_STRING:
            scalar.type = IPC_SCALAR_UINT64;
            scalar.value.u64 = (uint64_t) MmsValue_getBitStringAsInteger(value);
            return scalar;

        default: {
            char placeholder[64];
            snprintf(placeholder, sizeof(placeholder), "<unsupported:%s>", MmsValue_getTypeString(mutableValue));
            return rawPlaceholder(placeholder);
        }
    }
}

void
IpcDispatcherValueCodec_freeScalar(IpcScalarValue* scalar) {
    if (!scalar) return;
    if (scalar->type == IPC_SCALAR_STRING || scalar->type == IPC_SCALAR_RAW) {
        free(scalar->value.str);
        scalar->value.str = NULL;
    }
}

bool
IpcDispatcherValueCodec_decodeQuality(const MmsValue* value, IpcQuality* outQuality) {
    if (!outQuality) return false;
    memset(outQuality, 0, sizeof(*outQuality));

    if (!value || MmsValue_getType(value) != MMS_BIT_STRING) return false;

    Quality quality = Quality_fromMmsValue(value);

    switch (Quality_getValidity(&quality)) {
        case QUALITY_VALIDITY_GOOD: outQuality->validity = IPC_QUALITY_GOOD; break;
        case QUALITY_VALIDITY_INVALID: outQuality->validity = IPC_QUALITY_INVALID; break;
        case QUALITY_VALIDITY_RESERVED: outQuality->validity = IPC_QUALITY_RESERVED; break;
        case QUALITY_VALIDITY_QUESTIONABLE: outQuality->validity = IPC_QUALITY_QUESTIONABLE; break;
        default: outQuality->validity = IPC_QUALITY_INVALID; break;
    }
    outQuality->detailFlags = (uint16_t) quality;

    return true;
}
