#ifndef IED_MODEL_ONLINE_LOADER_USECASES_H_
#define IED_MODEL_ONLINE_LOADER_USECASES_H_

/*
 * Pure reference-format conversion logic - no third-party includes, no
 * IedConnection, no IedModel. This is the mirror image of mms_report_client's
 * own MmsReportClientUseCases_buildWireMemberReferences (which converts this
 * codebase's "$"-joined wire form INTO IedConnection_createDataSet's required
 * dot/bracket form); this feature needs the OPPOSITE direction, since
 * IedConnection_getDataSetDirectory hands back member references already in
 * dot/bracket ACSI form ("LDName/LNodeName.item(arrayIndex)component[FC]" per
 * that function's own doc comment in iec61850_client.h) and every other part
 * of this codebase (reference labeling, ipc_dispatcher's quality pairing)
 * expects the "$"-joined "LD/LN$FC$DO[$SDO...]$DA" form instead.
 */

/*
 * Converts one ACSI dot/bracket-form dataset member reference
 * ("LD/LN.DO[.SDO...].DA[FC]", the exact shape IedConnection_getDataSetDirectory
 * returns) into this codebase's "$"-joined wire form ("LD/LN$FC$DO[$SDO...]$DA").
 * Any "(arrayIndex)" annotation on a path segment (array-element FCDAs) is
 * stripped rather than preserved - this codebase does not model array
 * indices anywhere else (see ied_model's own documented, deliberately
 * deferred DAI/@ix limitation), so preserving it here would create a
 * reference shape nothing downstream can consume anyway.
 * Returns NULL (malformed input - no trailing "[FC]", no "." after the LD/LN
 * prefix, or allocation failure) rather than a best-effort partial string;
 * caller must free a non-NULL result and should skip NULL ones.
 */
char*
IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(const char* acsiRef);

#endif /* IED_MODEL_ONLINE_LOADER_USECASES_H_ */
