#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "stdbool_compat.h"
#include "features/ipc_dispatcher/utils/ipc_dispatcher_value_codec.h"

static MmsValue* fixtureValue;

void
setUp(void) {
    fixtureValue = NULL;
}

void
tearDown(void) {
    if (fixtureValue) {
        MmsValue_delete(fixtureValue);
        fixtureValue = NULL;
    }
}

/* ---- convert ---- */

void
test_convert_boolean(void) {
    fixtureValue = MmsValue_newBoolean(true);

    IpcScalarValue scalar = IpcDispatcherValueCodec_convert(fixtureValue);

    TEST_ASSERT_EQUAL_INT(IPC_SCALAR_BOOL, scalar.type);
    TEST_ASSERT_TRUE(scalar.value.b);

    IpcDispatcherValueCodec_freeScalar(&scalar);
}

void
test_convert_integer(void) {
    fixtureValue = MmsValue_newIntegerFromInt32(-42);

    IpcScalarValue scalar = IpcDispatcherValueCodec_convert(fixtureValue);

    TEST_ASSERT_EQUAL_INT(IPC_SCALAR_INT64, scalar.type);
    TEST_ASSERT_EQUAL_INT64(-42, scalar.value.i64);

    IpcDispatcherValueCodec_freeScalar(&scalar);
}

void
test_convert_float(void) {
    fixtureValue = MmsValue_newFloat(3.5f);

    IpcScalarValue scalar = IpcDispatcherValueCodec_convert(fixtureValue);

    TEST_ASSERT_EQUAL_INT(IPC_SCALAR_DOUBLE, scalar.type);
    /* This vendored Unity build has double support excluded (no other test
     * in this repo uses TEST_ASSERT_EQUAL_DOUBLE/FLOAT) - plain C comparison
     * instead, exact here since 3.5f round-trips losslessly through double. */
    TEST_ASSERT_TRUE(scalar.value.d == 3.5);

    IpcDispatcherValueCodec_freeScalar(&scalar);
}

void
test_convert_visibleString_isDeepCopied(void) {
    fixtureValue = MmsValue_newVisibleString("hello");

    IpcScalarValue scalar = IpcDispatcherValueCodec_convert(fixtureValue);

    TEST_ASSERT_EQUAL_INT(IPC_SCALAR_STRING, scalar.type);
    TEST_ASSERT_EQUAL_STRING("hello", scalar.value.str);

    /* Mutate/free the source after conversion - the scalar must be unaffected. */
    MmsValue_delete(fixtureValue);
    fixtureValue = NULL;

    TEST_ASSERT_EQUAL_STRING_MESSAGE("hello", scalar.value.str, "string scalar must be a deep copy");

    IpcDispatcherValueCodec_freeScalar(&scalar);
}

void
test_convert_nullValue_producesRawNullPlaceholder(void) {
    IpcScalarValue scalar = IpcDispatcherValueCodec_convert(NULL);

    TEST_ASSERT_EQUAL_INT(IPC_SCALAR_RAW, scalar.type);
    TEST_ASSERT_EQUAL_STRING("<null>", scalar.value.str);

    IpcDispatcherValueCodec_freeScalar(&scalar);
}

void
test_convert_unsupportedType_producesRawPlaceholder(void) {
    /* MMS_OCTET_STRING is not decoded in v1 - falls back to IPC_SCALAR_RAW. */
    fixtureValue = MmsValue_newOctetString(4, 4);

    IpcScalarValue scalar = IpcDispatcherValueCodec_convert(fixtureValue);

    TEST_ASSERT_EQUAL_INT(IPC_SCALAR_RAW, scalar.type);
    TEST_ASSERT_NOT_NULL(scalar.value.str);

    IpcDispatcherValueCodec_freeScalar(&scalar);
}

void
test_convert_bitString_decodesAsRawUnsignedInteger(void) {
    /* Covers CODEDENUM-typed value DAs (Dbpos/Tcmd) that wire-encode as a
     * bitstring - deliberately a raw integer, not a named enum, since this
     * function can't know which specific CODEDENUM a given bitstring
     * represents (see this function's own doc comment). */
    fixtureValue = MmsValue_newBitString(2);
    MmsValue_setBitStringFromInteger(fixtureValue, 2); /* e.g. Dbpos "on" */

    IpcScalarValue scalar = IpcDispatcherValueCodec_convert(fixtureValue);

    TEST_ASSERT_EQUAL_INT(IPC_SCALAR_UINT64, scalar.type);
    TEST_ASSERT_EQUAL_UINT64(2, scalar.value.u64);

    IpcDispatcherValueCodec_freeScalar(&scalar);
}

void
test_convert_bitString_zeroValue(void) {
    fixtureValue = MmsValue_newBitString(2);
    MmsValue_setBitStringFromInteger(fixtureValue, 0); /* e.g. Dbpos "intermediate-state" */

    IpcScalarValue scalar = IpcDispatcherValueCodec_convert(fixtureValue);

    TEST_ASSERT_EQUAL_INT(IPC_SCALAR_UINT64, scalar.type);
    TEST_ASSERT_EQUAL_UINT64(0, scalar.value.u64);

    IpcDispatcherValueCodec_freeScalar(&scalar);
}

/* ---- decodeDbposLabel ---- */

void
test_decodeDbposLabel_allFourOrdinals_produceCorrectLabels(void) {
    /* Built via Dbpos_toMmsValue (the library's own real Dbpos-encoding
     * function), not a raw MmsValue_setBitStringFromInteger - the bitstring's
     * on-wire bit order is an internal library detail, not something this
     * test should assume matches a naive integer-to-bits mapping. */
    Dbpos ordinals[4] = { DBPOS_INTERMEDIATE_STATE, DBPOS_OFF, DBPOS_ON, DBPOS_BAD_STATE };
    const char* expected[4] = { "intermediate-state", "off", "on", "bad-state" };

    for (int i = 0; i < 4; i++) {
        MmsValue* v = Dbpos_toMmsValue(NULL, ordinals[i]);

        const char* label = NULL;
        bool ok = IpcDispatcherValueCodec_decodeDbposLabel(IED_MODEL_DA_SEMANTIC_DBPOS, v, &label);

        TEST_ASSERT_TRUE_MESSAGE(ok, "expected a genuine Dbpos-typed bitstring to decode successfully");
        TEST_ASSERT_EQUAL_STRING(expected[i], label);

        MmsValue_delete(v);
    }
}

void
test_decodeDbposLabel_returnsFalse_whenSemanticIsNotDbpos(void) {
    /* Proves this never guesses from the bit pattern alone - even a
     * bit-for-bit identical bitstring must be refused without the
     * SCL-confirmed Dbpos semantic hint (e.g. a Tcmd sharing the same wire
     * representation - see IedModelUtils_mapBType's own doc comment). */
    fixtureValue = Dbpos_toMmsValue(NULL, DBPOS_ON);

    const char* label = NULL;
    bool ok = IpcDispatcherValueCodec_decodeDbposLabel(IED_MODEL_DA_SEMANTIC_NONE, fixtureValue, &label);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NULL(label);
}

void
test_decodeDbposLabel_returnsFalse_onNullValue(void) {
    const char* label = NULL;
    bool ok = IpcDispatcherValueCodec_decodeDbposLabel(IED_MODEL_DA_SEMANTIC_DBPOS, NULL, &label);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NULL(label);
}

void
test_decodeDbposLabel_returnsFalse_onNonBitstringValue(void) {
    fixtureValue = MmsValue_newBoolean(true);

    const char* label = NULL;
    bool ok = IpcDispatcherValueCodec_decodeDbposLabel(IED_MODEL_DA_SEMANTIC_DBPOS, fixtureValue, &label);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_NULL(label);
}

/* ---- decodeQuality ---- */

void
test_decodeQuality_good(void) {
    fixtureValue = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(fixtureValue, QUALITY_VALIDITY_GOOD);

    IpcQuality quality;
    bool ok = IpcDispatcherValueCodec_decodeQuality(fixtureValue, &quality);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(IPC_QUALITY_GOOD, quality.validity);
}

void
test_decodeQuality_questionableWithTestFlag(void) {
    fixtureValue = MmsValue_newBitString(13);
    MmsValue_setBitStringFromInteger(fixtureValue, QUALITY_VALIDITY_QUESTIONABLE | QUALITY_TEST);

    IpcQuality quality;
    bool ok = IpcDispatcherValueCodec_decodeQuality(fixtureValue, &quality);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_INT(IPC_QUALITY_QUESTIONABLE, quality.validity);
    TEST_ASSERT_EQUAL_UINT16(QUALITY_VALIDITY_QUESTIONABLE | QUALITY_TEST, quality.detailFlags);
}

void
test_decodeQuality_returnsFalse_onNull(void) {
    IpcQuality quality;
    TEST_ASSERT_FALSE(IpcDispatcherValueCodec_decodeQuality(NULL, &quality));
    TEST_ASSERT_EQUAL_INT(IPC_QUALITY_GOOD, quality.validity); /* zeroed */
}

void
test_decodeQuality_returnsFalse_onWrongType(void) {
    fixtureValue = MmsValue_newBoolean(true);

    IpcQuality quality;
    TEST_ASSERT_FALSE(IpcDispatcherValueCodec_decodeQuality(fixtureValue, &quality));
}

int
main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_convert_boolean);
    RUN_TEST(test_convert_integer);
    RUN_TEST(test_convert_float);
    RUN_TEST(test_convert_visibleString_isDeepCopied);
    RUN_TEST(test_convert_nullValue_producesRawNullPlaceholder);
    RUN_TEST(test_convert_unsupportedType_producesRawPlaceholder);
    RUN_TEST(test_convert_bitString_decodesAsRawUnsignedInteger);
    RUN_TEST(test_convert_bitString_zeroValue);

    RUN_TEST(test_decodeDbposLabel_allFourOrdinals_produceCorrectLabels);
    RUN_TEST(test_decodeDbposLabel_returnsFalse_whenSemanticIsNotDbpos);
    RUN_TEST(test_decodeDbposLabel_returnsFalse_onNullValue);
    RUN_TEST(test_decodeDbposLabel_returnsFalse_onNonBitstringValue);

    RUN_TEST(test_decodeQuality_good);
    RUN_TEST(test_decodeQuality_questionableWithTestFlag);
    RUN_TEST(test_decodeQuality_returnsFalse_onNull);
    RUN_TEST(test_decodeQuality_returnsFalse_onWrongType);

    return UNITY_END();
}
