#ifndef MMS_DATASET_MANAGER_DISCOVERY_H_
#define MMS_DATASET_MANAGER_DISCOVERY_H_

#include "features/mms_dataset_manager/domain/mms_dataset_manager_types.h"

/*
 * INTERNAL to this feature - not part of the public boundary
 * (service/mms_dataset_manager_api.h is). The two REUSE tiers plus the
 * once-per-cycle server-side discovery that feeds them both: find what is
 * already on the device, and hand it to a target rather than creating
 * anything new. Creation itself is mms_dataset_manager_provisioning.h's job,
 * reached only when everything here comes up empty.
 */

/*
 * Discovers every dataset already sitting on the server, once per connect
 * cycle (called from MmsDatasetManager_beginCycle, before any dataset is
 * created), scoped to the distinct set of LDs this client's own Dyn targets
 * live under. Feeds two things: (1) an accurate starting budget (see
 * MmsDatasetManagerUseCases_computeInitialDynamicDatasetBudget) instead of
 * blindly trusting SCL's declared max with zero awareness of what already
 * exists (confirmed on a real device: a leftover pile of domain-scoped
 * datasets from an earlier ungracefully-terminated run silently ate most of
 * the real budget before this client's own counter believed anything was
 * wrong); (2) a pool MmsDatasetManagerDiscovery_adoptUnclaimedDataset draws
 * from to reuse an existing dataset instead of self-creating a new one -
 * "primarily try to use existing/foreign datasets and create our own only via
 * necessity," per explicit user direction.
 *
 * Uses IedConnection_getLogicalDeviceDataSets (third_party/include/iec61850_client.h)
 * - a real, always-fresh wire query returning every dataset name under one
 * LD, in bare MMS notation (e.g. "LLN0$dyn", NOT LD-prefixed) - prefixed with
 * "<ldName>/" here to produce the same full "LD/LN..." reference form this
 * codebase uses everywhere else. Domain/VMD-scoped datasets only - an
 * association-scoped ("@"-prefixed) one belongs to whichever OTHER
 * connection created it and isn't stored under any LD's own namespace, so it
 * can never appear here and never needs to (it's already gone the moment
 * that other connection closes, never a discovery/reuse candidate).
 *
 * Scoped to Dyn targets' own LDs only, not a full-device sweep of every LD
 * regardless of relevance - keeps the added round-trip cost proportional to
 * what this client actually needs (typically far fewer LDs than the device's
 * total), at the honest cost of not catching a stale leftover under an LD
 * that no longer has any Dyn target at all (an LN/RCB removed since the
 * leftover was created) - a real but narrow gap, not worth a full-device
 * sweep's extra cost to close today.
 *
 * Returns a LinkedList of owned, full "LD/LN...$..." reference strings
 * (never NULL, empty if no Dyn targets or nothing discovered). Caller owns:
 * LinkedList_destroyDeep(list, free).
 */
LinkedList
MmsDatasetManagerDiscovery_findExistingServerDatasets(MmsDatasetManagerHandle handle);

/*
 * Tier 2 of the resolution order: for an RCB with no SCL datSet, a dataset
 * may already be assigned to it on the live device right now - realistically,
 * by a commissioning/engineering tool (e.g. Siemens DIGSI) during substation
 * engineering, independent of whatever client connects later; whoever set it
 * and whether they're still associated is irrelevant, the live RCB value is
 * the only signal consulted. This is also the common reconnect-time path for
 * a BUFFERED target reusing its own domain-scoped dataset from a prior
 * connect cycle, now that MmsDatasetManagerNaming_looksLikeOurOwnName no
 * longer rejects it - the cheap, correct "nothing to do" case this tier
 * exists to serve. ClientReportControlBlock_getDataSetReference(rcb) is free
 * here - `rcb` is already fetched by the caller, no extra wire round-trip for
 * the read itself.
 *
 * Unlike the static (SCL) case, this dataset's real member list is NOT known
 * locally - IedModel never had a matching DataSet registered for this RCB
 * (SCL declared no datSet at all). A single, bounded
 * IedConnection_getDataSetDirectory call resolves the actual member list,
 * live - narrower than the "no over-the-wire tree discovery" Hard Rule's
 * existing ied_model_online_loader exception on every axis: one already-named
 * dataset's member list (not a tree walk), fired only for RCBs with no SCL
 * datSet (the same population self-creation already makes a wire call for
 * today via IedConnection_createDataSet - a peer call on the same RCB set,
 * not a new category gaining wire access), and at most once per
 * (RCB, live-dataset-identity) pair for the whole process lifetime thanks to
 * the caller's own resolvedDatasetReference fingerprint check.
 *
 * On success, registers `liveDataset` into session->claimedDatasetNames -
 * the same claim-tracking the adopt/self-create tiers already self-register
 * into. Without this, the end-of-cycle orphan cleanup would see this name as
 * unclaimed and delete it out from under the RCB that is, at that very
 * moment, actively reporting through it - a real bug this closes: a reused
 * live dataset is just as "needed this cycle" as an adopted or self-created
 * one, it was simply invisible to that bookkeeping before.
 *
 * Returns true and fills *outMemberRefs (LinkedList of owned "$"-joined,
 * LD-prefixed member-reference char*) on success. Returns false
 * (*outMemberRefs left untouched) if no DatSet is currently assigned, it looks
 * like our own dangling name (unbuffered targets only), or the live fetch
 * itself fails or yields zero wire-convertible members - the caller falls
 * through to the next tier unchanged in every one of these cases, exactly as
 * if this tier didn't exist.
 */
bool
MmsDatasetManagerDiscovery_pullLiveDataset(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        ClientReportControlBlock rcb, LinkedList* outMemberRefs);

/*
 * Tier 3 of the resolution order: before self-creating a NEW dataset for a
 * Dyn target's own whole-device cluster, check whether an existing,
 * not-yet-claimed dataset already sits on the server under this target's own
 * LD (findExistingServerDatasets' own pool, computed once per connect cycle)
 * - "primarily try to use existing/foreign datasets and create our own only
 * via necessity," per explicit user direction. Assigning an existing dataset
 * to an RCB is non-destructive and shareable - it doesn't modify the dataset
 * object itself, and any other client/tool that also references it (or
 * created it) is entirely unaffected - unlike deletion, which stays strictly
 * limited to datasets this client can prove it created itself (see the
 * orphan-cleanup pass), adoption applies to ANY existing dataset regardless
 * of origin.
 *
 * PREFERS THIS EXACT TARGET'S OWN PREVIOUSLY-ASSIGNED NAME FIRST (buffered
 * targets only - the domain-scoped naming scheme is the only one that can
 * persist/be rediscovered across cycles at all): if
 * MmsDatasetManagerNaming_buildDatasetName(target->objectReference, true) is
 * among this cycle's discovered candidates and still unclaimed, it's tried
 * before the general LD-wide scan. This is secondary hardening, independent
 * of pullLiveDataset already being the primary path for a buffered target
 * reclaiming its own dataset - without it, this target's own name could
 * otherwise be claimed by a DIFFERENT sibling RCB under the same LD during
 * the general scan (which has no such preference), forcing THIS target to
 * self-create instead of reusing a dataset it already owns, and - the real
 * risk - mismatching the SIBLING's own resolvedDatasetReference fingerprint,
 * spuriously resetting the sibling's value-diff cache back to bootstrap.
 *
 * Each adopted dataset is claimed by exactly ONE target this cycle (tracked
 * in session->claimedDatasetNames, checked before considering a candidate)
 * and never shared across multiple targets - maximizing distinct device
 * coverage the same way whole-device clustering's own fresh-create path
 * already does, rather than letting several targets redundantly adopt the
 * same one. A candidate found unusable (unresolvable or empty) is also
 * claimed, purely so it isn't re-probed via a wasted wire round-trip for
 * every remaining target this cycle.
 *
 * Mirrors pullLiveDataset's own contract closely (success + outMemberRefs) so
 * the caller can reconcile decode shape the SAME way it does for tier 2 - an
 * adopted dataset's real content is resolved live, exactly like a
 * commissioning-tool-assigned one; the only difference is where the reference
 * itself came from (server-wide discovery vs. the RCB's own already-set
 * DatSet attribute). Unlike pullLiveDataset, the resolved name isn't already
 * known to the caller (there's no live RCB value to read it from), so it's
 * returned directly - borrowed, owned by session->existingServerDatasets,
 * valid for the rest of the current connect cycle.
 *
 * Returns NULL (outMemberRefs left untouched) if this target's LN has no
 * '/' (malformed), no candidate exists under this target's own LD, or every
 * candidate found there is already claimed/unusable - the caller falls
 * through to self-creation unchanged in every one of these cases, exactly as
 * if this tier didn't exist.
 */
const char*
MmsDatasetManagerDiscovery_adoptUnclaimedDataset(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        LinkedList* outMemberRefs);

#endif /* MMS_DATASET_MANAGER_DISCOVERY_H_ */
