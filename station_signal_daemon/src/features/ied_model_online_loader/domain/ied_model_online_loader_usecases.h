#ifndef IED_MODEL_ONLINE_LOADER_USECASES_H_
#define IED_MODEL_ONLINE_LOADER_USECASES_H_

/*
 * Pure reference-format conversion logic - no third-party includes, no
 * IedConnection, no IedModel. This is the mirror image of mms_report_client's
 * own MmsDatasetManagerUseCases_buildWireMemberReferences (which converts this
 * codebase's "$"-joined wire form INTO IedConnection_createDataSet's required
 * dot/bracket form); this feature needs the OPPOSITE direction, since
 * IedConnection_getDataSetDirectory hands back member references already in
 * dot/bracket ACSI form ("LDName/LNodeName.item(arrayIndex)component[FC]" per
 * that function's own doc comment in iec61850_client.h) and this feature's
 * own caller (ied_model_online_loader_connection.c's resolveAndBuildDataset)
 * needs it in this codebase's "$"-joined LD-prefix-free "LN$FC$DO[$SDO...]$DA"
 * form to hand straight to DataSetEntry_create. The public-facing,
 * LD-prefixed "LD/LN$FC$DO$DA" reference form used elsewhere (reference
 * labeling, ipc_dispatcher's quality pairing) is reconstructed separately by
 * IedModel_getDataSetMemberReferences, which prepends the entry's own
 * logicalDeviceName - never produced directly by this function.
 */

/*
 * Converts one ACSI dot/bracket-form dataset member reference
 * ("LD/LN.DO[.SDO...].DA[FC]", the exact shape IedConnection_getDataSetDirectory
 * returns) into this codebase's "$"-joined, LD-prefix-free wire form
 * ("LN$FC$DO[$SDO...]$DA") - the exact shape DataSetEntry_create's own
 * variableName argument requires (this codebase's dynamic-model gotcha #1,
 * see CLAUDE.md: no LD-wire-name prefix, or resolution silently fails). The
 * LD portion of the ACSI reference's "LD/LN" prefix is deliberately dropped,
 * not carried in the output - the LD is conveyed separately/implicitly via
 * which LogicalNode the entry's parent dataset belongs to.
 * Any "(arrayIndex)" annotation on a path segment (array-element FCDAs) is
 * stripped rather than preserved - this codebase does not model array
 * indices anywhere else (see ied_model's own documented, deliberately
 * deferred DAI/@ix limitation), so preserving it here would create a
 * reference shape nothing downstream can consume anyway.
 * Returns NULL (malformed input - no trailing "[FC]", no "." after the LD/LN
 * prefix, no "/" within that prefix, or allocation failure) rather than a
 * best-effort partial string; caller must free a non-NULL result and should
 * skip NULL ones.
 */
char*
IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef(const char* acsiRef);

#endif /* IED_MODEL_ONLINE_LOADER_USECASES_H_ */
