#ifndef IED_MODEL_LN_CATEGORY_H_
#define IED_MODEL_LN_CATEGORY_H_

#include "features/ied_model/domain/ied_model_types.h"

/*
 * Maps a single IEC 61850-7-4 LN group letter (the FIRST character of an
 * lnClass, e.g. 'X' from "XCBR") to its app-level LnCategory. This letter IS
 * the standard's own group indicator (formally defined, not this codebase's
 * invention) - what IS this codebase's own judgment call is which category
 * bucket each letter lands in; see the .c file's table for the exact mapping
 * and rationale. Unrecognized letters (including '\0') return
 * IED_MODEL_LN_CATEGORY_OTHER - never a guess into a real category.
 */
LnCategory
IedModelLnCategory_forGroupLetter(char groupLetter);

/*
 * Convenience wrapper: groupLetter = lnClass[0]. Returns
 * IED_MODEL_LN_CATEGORY_OTHER for a NULL or empty lnClass.
 */
LnCategory
IedModelLnCategory_forLnClass(const char* lnClass);

/*
 * Recovers an LnCategory from a raw, concatenated wire LN instance name
 * (prefix + lnClass + inst, no delimiter between them - the exact string
 * IedModelUtils_buildLnName produces, e.g. "XCBR1", "MyBkrPTOC2") with no
 * lnClass field available separately - used only by
 * ied_model_online_loader's no-SCL fallback path, where MMS ACSI directory
 * browsing returns only this already-built name. Strips the trailing digit
 * run (inst) then suffix-matches the remainder against the full IEC 61850-7-4
 * standard LN class name dictionary, longest match wins (handles an
 * arbitrary vendor prefix glued directly in front with no delimiter). Never
 * guesses: no confident match returns IED_MODEL_LN_CATEGORY_OTHER, same as
 * an unrecognized group letter. NULL/empty input also returns OTHER.
 */
LnCategory
IedModelLnCategory_forWireInstanceName(const char* wireName);

/*
 * Canonical uppercase name for a single LnCategory value ("CONTROL",
 * "MEASUREMENT", "PROTECTION", "OTHER") - used both for diagnostic logging
 * (ied_model_usecases.c's whole-device filter exclusion log) and as the
 * wire-facing string ipc_dispatcher tags each forwarded data point with.
 * Returns "UNKNOWN" for a value that isn't exactly one of the four defined
 * LnCategory constants (shouldn't happen - every classifier in this feature
 * only ever produces one of the four - but never crashes/guesses on a bad
 * input). Returns a static string literal - never owned, never freed.
 */
const char*
IedModelLnCategory_toString(LnCategory category);

#endif /* IED_MODEL_LN_CATEGORY_H_ */
