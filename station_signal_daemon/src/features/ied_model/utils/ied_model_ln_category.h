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
 * run (inst), then two stages:
 *
 *   1. suffix-match the remainder against the IEC 61850-7-4 standard LN class
 *      name dictionary, longest match wins (handles an arbitrary vendor
 *      prefix glued directly in front with no delimiter);
 *   2. failing that, read the last FOUR characters as the class name and
 *      classify by its first letter - IEC 61850-6 fixes an LN class at
 *      exactly four characters, so this classifies standard classes the
 *      dictionary happens to omit and vendor extensions alike, without
 *      needing an entry for either.
 *
 * Returns IED_MODEL_LN_CATEGORY_OTHER when neither stage yields a known group
 * letter, when fewer than four characters remain, and for NULL/empty input.
 */
LnCategory
IedModelLnCategory_forWireInstanceName(const char* wireName);

/*
 * Whether an LN of this class is exempt from category filtering entirely -
 * true for exactly "LLN0", false for everything else. LLN0 is an LD's own
 * status node (Mod/Beh/Health/NamPlt): it is what tells a consumer whether
 * that whole Logical Device is on, blocked, or faulted, so it must reach the
 * frontend whatever categories the technician picked. Its group letter is
 * 'L', so IedModelLnCategory_forLnClass classifies it OTHER - it is
 * deliberately left classified that way (ipc_dispatcher's outbound
 * `category` string, and therefore the frontend, is unchanged); this
 * predicate is a SEPARATE, orthogonal exemption honored at every filter
 * site, never a fifth category. Deliberately LLN0 only, not LPHD or the rest
 * of the 'L' group - those are ordinary reportable data with no
 * whole-LD-status role. NULL/empty returns false.
 */
bool
IedModelLnCategory_isAlwaysIncludedLnClass(const char* lnClass);

/*
 * Wire-instance-name counterpart of IedModelLnCategory_isAlwaysIncludedLnClass,
 * for ied_model_online_loader's no-SCL fallback path (no separate lnClass
 * field - see IedModelLnCategory_forWireInstanceName's own doc comment).
 * Deliberately does NOT reuse that function's dictionary match: its trailing
 * digit-strip removes LLN0's own '0' (which is part of the class name, not an
 * SCL inst), leaving "LLN", which matches nothing. LLN0 also never carries a
 * prefix or an inst per IEC 61850-6, so an exact "LLN0" comparison is both
 * correct and complete here. NULL/empty returns false.
 */
bool
IedModelLnCategory_isAlwaysIncludedWireInstanceName(const char* wireName);

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
