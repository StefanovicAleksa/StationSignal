#ifndef MMS_DATASET_MANAGER_NAMING_H_
#define MMS_DATASET_MANAGER_NAMING_H_

#include "features/mms_dataset_manager/domain/mms_dataset_manager_types.h"

/*
 * INTERNAL to this feature - not part of the public boundary
 * (service/mms_dataset_manager_api.h is). Dataset NAME derivation plus the
 * cleanup-tracking list every domain-scoped name has to land in. Shared by
 * discovery/ and provisioning/, which both need to reconstruct a target's own
 * deterministic name to recognize it again on a later connect.
 */

/* Two distinct naming schemes depending on `buffered`:
 *
 *   - UNBUFFERED: "@"-prefixed => association-scoped (destroyed automatically
 *     when this connection closes - see IedConnection_createDataSet's own doc
 *     comment) - no explicit delete needed, no risk of leaking the device's
 *     dataset budget across reconnects/restarts. lnReference is sanitized
 *     ('/' -> '_') purely so the generated name reads sensibly in logs;
 *     createDataSet doesn't require any particular naming beyond the leading
 *     "@".
 *
 *   - BUFFERED: an association-scoped dataset is destroyed the instant this
 *     connection closes - semantically incompatible with a buffered RCB,
 *     whose whole purpose is to keep reporting through a disconnect.
 *     Confirmed directly against the vendored reference server
 *     (mms_mapping/reporting.c's updateReportDataset: an "@"-prefixed
 *     dataSetName is rejected outright when rc->buffered is true, surfacing
 *     to the client as IED_ERROR_OBJECT_VALUE_INVALID/32) and against a real
 *     SIPROTEC 6MD device (see GAP3_DYNAMIC_DATASET_NOTES.md). Instead builds
 *     a domain/VMD-scoped name - "$"-joined, no "@" prefix, same convention
 *     ied_model already uses for an SCL-declared datasetReference
 *     (IedModelUseCases_getReportSubscriptionTargets) - which persists on the
 *     server past this connection, exactly what a buffered RCB needs.
 *     lnReference already has the required "LDName/LNodeName" shape
 *     (IedConnection_createDataSet's own doc comment: "LDName/LNodeName.dataSetName
 *     for permanent domain or VMD scope data sets" - the client library
 *     splits on the first '/' for the domain, then converts any '.' in the
 *     remainder to '$' for the wire form). Whole-device clustering means the
 *     input here is often target->objectReference instead (e.g.
 *     "LDName/LNName.BR.rcbName", so each Dyn target gets its own uniquely
 *     named dataset rather than sharing an LN-wide one) - THAT string
 *     contains a literal '.' (the ".BR."/".RP." RCB-instance segment), which
 *     createDataSet's own internal conversion would silently fold to '$' on
 *     the wire, but this function's own returned string would NOT reflect
 *     unless it applies the identical conversion itself: any '.' is
 *     explicitly replaced with '$' below before appending "$dyn", so the
 *     name we keep for ClientReportControlBlock_setDataSetReference always
 *     matches the real server-side name bit-for-bit, regardless of createDataSet's
 *     own internal behavior. Confirmed as a real, previously-latent bug: a
 *     dot left unconverted here produces a dataset reference the server
 *     genuinely can't resolve, surfacing as IED_ERROR_OBJECT_VALUE_INVALID/32
 *     on the following setRCBValues - identical-looking symptom to, but a
 *     completely different root cause from, the association-scoped-on-a-
 *     buffered-RCB bug this whole function exists to fix.
 *     Deterministic and stable across reconnects/restarts by construction -
 *     required so a later connect recognizes and reuses its own
 *     already-created dataset (see the IED_ERROR_OBJECT_EXISTS handling in
 *     mms_dataset_manager_provisioning.c) instead of erroring or duplicating,
 *     and so MmsDatasetManager_cleanupOnStop can find it again by name.
 *
 * Caller owns the returned string (free). NULL on allocation failure. */
char*
MmsDatasetManagerNaming_buildDatasetName(const char* lnReference, bool buffered);

/* Dedup-inserting append to handle->domainScopedDynamicDatasetNames (see that
 * field's own doc comment) - called only for buffered/domain-scoped names,
 * never for "@"-scoped ones (those need no cleanup tracking at all). Cheap
 * linear scan: this list holds at most one entry per target needing a
 * domain-scoped self-created dataset, realistically single digits to tens of
 * entries even on a large IED. */
void
MmsDatasetManagerNaming_rememberDomainScopedName(MmsDatasetManagerHandle handle, const char* datasetName);

/* Cheap, purely-local skip: true if datasetRef looks like a name
 * MmsDatasetManagerNaming_buildDatasetName would itself generate for this
 * target - i.e. a DANGLING reference to a PRIOR connection's own
 * association-scoped dataset, already destroyed server-side the moment that
 * old connection closed ("@"-prefixed = destroyed automatically on connection
 * close). A real device is not obligated to clear an RCB's DatSet attribute
 * just because the dataset object it named is gone, so getRCBValues can
 * legitimately still echo it back after we reconnect. This is
 * belt-and-suspenders only, not the sole safety net - the real one is
 * MmsDatasetManagerDiscovery_pullLiveDataset's own
 * IedConnection_getDataSetDirectory failure handling, which falls through
 * either way (a dangling reference would fail to resolve there too).
 *
 * ONLY MEANINGFUL FOR UNBUFFERED TARGETS: this whole "dangling" rationale
 * rests on the association-scoped ("@"-prefixed) name being destroyed the
 * instant the prior connection closed. A buffered target's own dataset is
 * domain/VMD-scoped instead and genuinely PERSISTS past a connection close -
 * that persistence is the entire reason buildDatasetName gives buffered
 * targets a different naming scheme in the first place. So a buffered
 * target's own name showing up as its RCB's live DatSet on reconnect is not
 * dangling at all - it's the expected, persistent, reusable case, and
 * rejecting it here was a real bug: it forced every buffered Dyn RCB's
 * reconnect through the adopt tier instead of the cheap, correct pull-live
 * reuse path, and (compounded by adoption having no preference for a target's
 * own name) could cause sibling RCBs under the same LD to cross-adopt each
 * other's datasets on every reconnect, spuriously resetting their value-diff
 * caches back to bootstrap and permanently suppressing real reports.
 *
 * Checks the one naming scheme self-creation can produce for this target:
 * buildDatasetName(objectReference, target->buffered) - every Dyn target is
 * keyed/named by its own objectReference now (whole-device clustering assigns
 * each target its own unique cluster, no more LN-wide shared name).
 * target->buffered is a fixed, model-level property of this specific RCB, so
 * whichever scheme (association-scoped vs domain-scoped) this target would
 * itself produce is the only one relevant here. Checked unconditionally, not
 * gated on whether this target has a cluster assignment THIS cycle - it may
 * have been assigned one on a prior cycle whose dataset (association-scoped
 * and gone, or a domain-scoped one this same client already owns and would
 * otherwise re-detect as "live") is now dangling/stale from this cycle's
 * point of view. */
bool
MmsDatasetManagerNaming_looksLikeOurOwnName(const char* datasetRef, const ReportControlBlockTarget* target);

/* Small owned-string-list membership check - used by discovery to dedupe which
 * LDs it has already queried and to track which discovered dataset names have
 * already been tried/claimed this cycle (so the same unusable candidate isn't
 * re-probed for every remaining target, and the same usable one isn't handed
 * to two different targets), and by the stop/orphan cleanup passes. */
bool
MmsDatasetManagerNaming_stringListContains(LinkedList list, const char* value);

#endif /* MMS_DATASET_MANAGER_NAMING_H_ */
