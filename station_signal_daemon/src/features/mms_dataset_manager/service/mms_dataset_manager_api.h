#ifndef MMS_DATASET_MANAGER_API_H_
#define MMS_DATASET_MANAGER_API_H_

#include "linked_list.h"
#include "iec61850_client.h"
#include "features/ied_model/service/ied_model_api.h"
#include "features/mms_dataset_manager/domain/mms_dataset_manager_types.h"

/*
 * Public boundary of the mms_dataset_manager feature. Other features
 * (mms_report_client, ...) should only ever include this header - never reach
 * into domain/data/utils directly.
 *
 * Owns everything about an IED's report DATASETS, against an already-loaded
 * ied_model and an already-connected IedConnection: discovers what already
 * exists on the device (SCL-declared static datasets, live-assigned dynamic
 * ones, foreign/leftover ones from other tools or earlier runs), then fills
 * whatever coverage gap remains with datasets of its own, sized and budgeted
 * against the device's declared <DynDataSet>/<ConfDataSet> capacity. Hands
 * back one resolved dataset per Report Control Block target.
 *
 * Deliberately knows NOTHING about enabling RCBs, report handlers, value-diff
 * caching or reconnect supervision - that is mms_report_client's job. Works
 * with an IedModelHandle in ANY AccessMode.
 *
 * THREADING: not internally synchronized. Every call below must come from one
 * thread at a time, driving one cycle strictly sequentially
 * (beginCycle -> resolveForTarget* -> endCycle). mms_report_client's own
 * reconnect supervisor thread is the only caller today and satisfies this by
 * construction.
 */

/*
 * iedModel: borrowed - caller retains ownership and must not release it
 * before MmsDatasetManager_destroy.
 * connection: borrowed, and MUST be the same live IedConnection the caller
 * enables its RCBs on. This feature deliberately never opens an association
 * of its own: an association-specific ("@"-prefixed) dataset created on a
 * different connection would be invisible and unassignable to the RCB being
 * enabled, and a second association is exactly the extra device load this
 * codebase avoids.
 * targets: borrowed ReportControlBlockTarget* list (from
 * IedModel_getReportSubscriptionTargets) - must outlive this handle.
 * Returns NULL and sets *outError on argument/allocation failure. Does NOT
 * touch the network here - nothing happens until MmsDatasetManager_beginCycle.
 */
MmsDatasetManagerHandle
MmsDatasetManager_create(IedModelHandle iedModel, IedConnection connection, LinkedList targets,
        MmsDatasetManagerError* outError);

/*
 * Starts one connect cycle: runs the once-per-cycle server-side dataset
 * discovery, seeds both dataset-count budgets from SCL (correcting the
 * ConfDataSet one for what discovery just found already on the device), and
 * builds the whole-device cluster plan assigning each spare Dyn RCB slot its
 * own share of the device's reportable data. Logs a one-line summary per
 * budget pool.
 *
 * Must be called after every (re)connect, before any resolveForTarget call:
 * nothing here is carried across reconnects, because the device's own
 * server-side state may genuinely have changed since the last one. Calling it
 * twice without an intervening endCycle ends the previous cycle first, so a
 * caller can never silently leak a session.
 */
void
MmsDatasetManager_beginCycle(MmsDatasetManagerHandle handle);

/*
 * Resolves which dataset one RCB target should bind, in a strict four-tier
 * order, and reports which tier won via *outResolution:
 *
 *   1. STATIC     - target->datasetReference, if SCL declared one for this
 *                   RCB. Never touches the network.
 *   2. PULL LIVE  - the device ALREADY has a dataset assigned to this RCB
 *                   right now - realistically one created by a commissioning/
 *                   engineering tool (e.g. Siemens DIGSI) during substation
 *                   engineering, independent of whatever client connects
 *                   later. Also the genuine reconnect-time path for a
 *                   BUFFERED target's own domain-scoped dataset, which
 *                   persists past a connection close.
 *   3. ADOPT      - an existing, not-yet-claimed dataset (ours from a prior
 *                   run, or a completely foreign one from another tool)
 *                   already sits on the server under this RCB's own LD.
 *                   "Primarily try to use existing/foreign datasets and
 *                   create our own only via necessity," per explicit product
 *                   direction. Adoption is non-destructive and shareable, so
 *                   it applies to ANY existing dataset regardless of origin.
 *   4. SELF-CREATE - only if all three above are absent/unusable, creates
 *                   this target's own planned whole-device cluster.
 *                   Unbuffered: association-scoped (auto-destroyed on
 *                   disconnect, no cleanup needed). Buffered: domain/VMD-
 *                   scoped, since an association-scoped dataset is destroyed
 *                   the instant the connection closes - which a real device
 *                   rejects assigning to a buffered RCB outright.
 *
 * Tiers 2 and 3 no longer run live, per target, here: MmsDatasetManager_beginCycle's
 * own claim pass (MmsDatasetManagerProvisioning_runClaimPass) already ran
 * both, for every target, before this cycle's whole-device cluster plan even
 * existed - see that function's own doc comment for why tier 3's adoption
 * ordering across targets sharing an LD requires a genuine up-front pass
 * rather than resolving lazily per target. This call just returns that
 * cached outcome (or, if none exists, resolves tier 4 fresh) - no wire call
 * of its own for tiers 1-3, and no `rcb` parameter needed (the claim pass
 * fetches its own).
 *
 * *outResolution is fully written on every path, including total failure
 * (datasetReference NULL). The caller owns it and MUST release it with
 * MmsDatasetManager_destroyResolution exactly once, however its own sequence
 * ends. A NULL datasetReference is NOT automatically a failure - check
 * `wasNeeded` to tell a benign unused spare RCB slot from a real problem (see
 * MmsDatasetResolution's own doc comment).
 */
void
MmsDatasetManager_resolveForTarget(MmsDatasetManagerHandle handle, ReportControlBlockTarget* target,
        MmsDatasetResolution* outResolution);

/* Frees a resolution's owned fields and resets it to empty. NULL-safe, and
 * safe to call twice on the same resolution. */
void
MmsDatasetManager_destroyResolution(MmsDatasetResolution* resolution);

/*
 * Ends the current connect cycle and tears its session down.
 *
 * runOrphanCleanup: when true, first runs the proactive orphan-cleanup pass -
 * deleting OUR OWN domain-scoped datasets that are sitting on the device but
 * weren't needed by any target this cycle, reclaiming that budget. This is
 * what closes the ungraceful-restart gap MmsDatasetManager_cleanupOnStop
 * cannot (a killed or crashed daemon never reaches a graceful stop at all).
 * Pass false during teardown, where the connection is about to go away and a
 * best-effort cleanup pass is just noise. The bar for deletion is strict: a
 * dataset is only ever deleted if it exactly reconstructs as some real
 * buffered Dyn target's own deterministic name. A foreign dataset is NEVER
 * deleted - only ever adopted.
 *
 * Safe to call without a matching beginCycle (no-op).
 */
void
MmsDatasetManager_endCycle(MmsDatasetManagerHandle handle, bool runOrphanCleanup);

/*
 * Graceful-stop cleanup: for every RCB still bound to one of the
 * domain-scoped datasets this handle created over its whole lifetime,
 * disables RptEna and clears DatSet, then deletes those datasets.
 *
 * MUST be called while the connection is still live and BEFORE the caller's
 * own IedConnection_close - both setRCBValues and deleteDataSet need a live
 * association. Both steps are required: a dataset still referenced by an
 * RCB's DatSet is refused for deletion regardless of RptEna (confirmed
 * against the reference server, IED_ERROR_OBJECT_CONSTRAINT_CONFLICT/35).
 *
 * Scoped strictly to datasets this handle itself created - never an
 * SCL-static target's permanent engineering configuration, never a
 * foreign/adopted dataset another tool still relies on. Best-effort and
 * idempotent: failures are logged, never fatal, and a second call is a no-op.
 */
void
MmsDatasetManager_cleanupOnStop(MmsDatasetManagerHandle handle);

/* Frees the handle. Does NOT touch the network (the connection may already be
 * closed by now) - call MmsDatasetManager_cleanupOnStop first if server-side
 * datasets should be released. Does not free the borrowed iedModel,
 * connection or targets. NULL-safe. */
void
MmsDatasetManager_destroy(MmsDatasetManagerHandle handle);

#endif /* MMS_DATASET_MANAGER_API_H_ */
