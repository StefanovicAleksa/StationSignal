# Gap 3 — dynamic dataset creation caps/chunking: handoff notes

**Update (later session): whole-device clustering, discover-before-create/adoption, and proactive
orphan cleanup are now implemented** — see `station_signal_daemon/CHANGELOG.md`'s "Redesigned into
whole-device clustering + reuse-before-create + proactive orphan cleanup" entry and `CLAUDE.md`'s
own `mms_report_client` bullet for the current-state description. This closes the per-connection
dataset-count-budget-tracking half of "Agreed direction so far §3" below (now driven by real
server-discovered state, not a blind SCL-max reset) and goes further (whole-device coverage,
adoption of existing/foreign datasets, orphan reclamation) than this file originally scoped. The
**no-SCL empirical/adaptive-discovery case (§2 below — inferring an unknown cap from
`createDataSet` *failure* patterns) remains unimplemented** — a genuinely different mechanism from
the discovery of *already-existing* datasets that is now implemented, still an open gap.

Continuation notes for picking this up in a new chat. Written after two prior sessions:
(1) diagnosed why the daemon couldn't see/enable Siemens SIPROTEC dynamic-dataset-style RCBs and
fixed the SCL-parsing gap (gap 1) + added debug logging; (2) analyzed a real run against a real
6MD-family device (`172.16.0.5:102`) and had a design discussion about gap 3. Nothing described
below as "proposed"/"not yet implemented" has been coded yet — this file is planning input for a
fresh chat, not a changelog of finished work.

## Where things stand

- **Gap 1 (fixed, committed)**: `station_signal_daemon/src/features/ied_model/data/ied_model_scl_loader.c`
  now unescapes and parses Siemens' `<Private type="...ControlBlockStorage...">`-wrapped
  `<ReportControl>` payloads into real RCB targets, instead of only detecting-and-ignoring them.
  Tests added in `integration_tests/ied_model/`.
- **Debug logging (fixed, committed)**: `mms_report_client_connection.c` now logs which of the
  three dataset-resolution tiers (SCL/live/self-created) won for each RCB, member counts before
  every `createDataSet` attempt, and success confirmations (previously only failures logged
  anything). `mms_report_client_report_adapter.c` now logs on report arrival, per-RCB value-diff
  filter drops, cross-RCB dedup drops, and successful forwards — previously the report-reception
  path had **zero** logging anywhere, which was itself the answer to "why didn't we see a change
  we made on the real device": there was no way to tell from the terminal whether a report ever
  arrived at all vs. got silently filtered.
- **Gap 2 (device limitation, not our bug, not fixable in our code)**: the real device's file
  service responds fine to `GetFileDirectory` but returns zero matching files — confirmed directly
  in a real run's log (`scl_bootstrap`'s `directory browse -> ok, 0 match(es)`). This is why the
  online-discovery fallback (`ied_model_online_loader`) engages for this device family at all.
- **Gap 3 (this file's subject, not yet implemented)**: `getOrCreateDynamicDataset`
  (`mms_report_client_connection.c:136+`) creates one dataset per LN covering every FC=ST/MX leaf,
  with no chunking against a device's real caps and no per-connection dataset-count budget
  tracking.

## What the real device's log actually showed (important — refines the original gap-3 diagnosis)

Real run against `172.16.0.5:102` (`C5_6MD...`, `Siprotec-6MD66x` family, same as the 6MD663 SCD
already on disk at repo root as `IEC61850-Station.scd`):

- The device's own declared `<Services>` block for this IED family:
  `<DynDataSet max="15" maxAttributes="60" />` (confirmed in `IEC61850-Station.scd`).
- **First ~10 LNs' worth of unbuffered RCBs enabled successfully** — real proof the whole
  enable/dataset pipeline works end to end once a dataset is available.
- **FIXED (see `station_signal_daemon/CHANGELOG.md`'s "`mms_report_client` dynamic dataset
  creation" section)**: every buffered RCB (`brcbA01`/`brcbB01`) failed outright, even on an LN
  whose dataset creation itself succeeded — `error 32` (`IED_ERROR_OBJECT_VALUE_INVALID`) on one
  LD, `error 31` (`IED_ERROR_OBJECT_ATTRIBUTE_INCONSISTENT`) on others. Confirmed root cause
  (against both the vendored reference server and a real `192.168.1.43` SIPROTEC 6MD device):
  this device family rejects assigning an **association-scoped** (`@`-prefixed, destroyed the
  instant the connection closes) dataset to a **buffered** RCB specifically — semantically that
  combination can't survive the disconnect a buffered RCB exists to buffer through. **This was a
  separate problem from the attribute/count caps below.** Fixed by branching
  `buildDynamicDatasetName`/`getOrCreateDynamicDataset` on `target->buffered`: a buffered target
  now gets a **domain/VMD-scoped** name instead (persists past the connection, no longer
  association-scoped), with `IED_ERROR_OBJECT_EXISTS` on a later `createDataSet` attempt treated
  as a successful reuse, and explicit `IedConnection_deleteDataSet` cleanup on
  `MmsReportClientConnection_stop` (a domain-scoped dataset isn't auto-cleaned by the server on
  disconnect the way an association-scoped one is).
- **From a certain point on, every remaining `createDataSet` call failed with `error 99`
  (`IED_ERROR_UNKNOWN`), regardless of member count** — including a 6-member and a 9-member
  dataset. A dataset that small failing rules out the `maxAttributes="60"` cap as the active cause;
  **the real constraint hit in the field was the total dataset-COUNT budget (`DynDataSet
  max="15"`)**, not per-dataset size. Only ~10 *new* datasets were created before the wall was hit,
  which is suspiciously short of 15 — possible leftover datasets from an earlier IEDScout session
  against the same real device may have already been eating into that budget (worth checking/
  clearing directly on the device before assuming our own code path is double-counting anything).
- Once a dataset fails, `DATSET` is left unset and `setRCBValues` fails with `error 26`
  (`IED_ERROR_TEMPORARILY_UNAVAILABLE`) — matches the existing code's documented fallback exactly,
  though the code's own comment predicted `IED_ERROR_OBJECT_VALUE_INVALID`/32 for this case, not 26
  — a minor comment inaccuracy worth a one-line fix whenever this file is next touched.
- Root cause code: `IedClientError` enum confirmed at `third_party/include/iec61850_client.h:80-172`;
  the mapping in vendored `third_party_src/libiec61850/src/iec61850/client/ied_connection.c:150-263`
  shows `MMS_ERROR_RESOURCE_CAPABILITY_UNAVAILABLE` (the standard MMS error for exactly "resource
  limit exceeded") has **no case in the switch**, so it silently falls to `IED_ERROR_UNKNOWN`/99 —
  this is why "too many datasets" and "dataset too big" are likely indistinguishable from the raw
  error code alone (see the heuristic proposed below).

## Agreed direction so far (from the design discussion, not yet implemented)

### 1. When SCL is available — read the real caps instead of guessing

`ied_model_scl_loader.c` **never parses `<Services>` at all today** (confirmed: zero references to
`Services`/`DynDataSet`/`maxAttributes` anywhere in that file). Plan:
- Parse `<Services><DynDataSet max="N" maxAttributes="M" /></Services>` off the IED element during
  SCL load (same loader, same `LoaderContext`-style pass).
- Surface `N`/`M` through a new `ied_model` accessor (`service/ied_model_api.h`) so
  `mms_report_client` can consult real numbers instead of guessing.
- If `<DynDataSet>` isn't present in a given file at all (schema makes it optional), fall through
  to the empirical approach below rather than assuming a default.

### 2. When SCL is NOT available (online-discovery fallback) — deferred, needs its own research pass

No standard ACSI/MMS service exposes `DynDataSet max`/`maxAttributes` at runtime — they're
PIXIT/PICS-level facts, not queryable. Proposed (not fleshed out yet — this is the "figure out
later" half the user explicitly deferred):
- **Empirical/adaptive discovery via reacting to `createDataSet` failures**, not proactive
  knowledge:
  - A **small** dataset (single digits) failing with the resource-unavailable-mapped error signals
    the **count budget** is exhausted → stop attempting further `createDataSet` calls for the rest
    of this connect cycle (currently the code just keeps trying every remaining LN one by one, each
    a wasted real MMS round-trip — confirmed directly in the real log, dozens of doomed attempts in
    a row).
  - A **large** dataset failing, followed by a smaller retry succeeding, signals a genuine
    per-dataset size limit for that LN → cache the discovered working size for the rest of the
    connection so subsequent LNs chunk to it immediately instead of re-probing from scratch.
- Needs more thought before implementing: how many retries/what reduction strategy (halving? DO-group
  removal one at a time?), whether to persist a discovered budget/size across reconnects on the
  same device (probably yes, keyed by host — needs a place to live), and how to avoid false
  positives (e.g., a transient/unrelated MMS error being mistaken for a capacity signal).

### 3. Logical chunking, once a size limit is known (from either path above)

- **Group reportable attributes by parent Data Object (DO), not by an arbitrary flat cut.** The
  DO name is already present as the 3rd `$`-segment of every member reference string
  (`"<LDName>/<LN>$<FC>$<DO>$<DA>"` — confirmed in `IedModelUseCases_getReportableAttributeReferencesForLogicalNode`,
  `ied_model_usecases.c:803-837`, and `buildMemberReferenceForEntry`, same file). **No new accessor
  is needed just to derive DO grouping** — parse the existing flat reference list's 3rd segment.
  Never split one DO's own leaves (`stVal`/`q`/`t`, etc.) across two chunks.
  - Pack whole DO-groups into a chunk greedily up to `maxAttributes`.
- **Assign each chunk to one of the LN's own spare predefined RCB instances, instead of deduping
  them all onto one shared dataset.** Siemens's convention provides up to ten unbuffered instances
  per LN (`urcbA`..`urcbJ`) — today's `dynamicDatasetCache`/`getOrCreateDynamicDataset`
  (`mms_report_client_connection.c:136+`, keyed only by `target->lnReference`) collapses **all** of
  them onto the same single dataset, wasting nine of the ten slots. Proposed: chunk 1 → `urcbA`'s
  own dataset, chunk 2 → `urcbB`'s, etc. If an LN needs more chunks than it has spare RCB instances,
  log that plainly as a real device-imposed ceiling (not silently drop data) — "LN X needs N chunks
  but only has M predefined unbuffered RCBs; N-M chunk(s) won't be reported."
  - Needs a real design pass on exactly how `dynamicDatasetCache`'s dedup key changes (LN only →
    LN + chunk index), and how `enableOneTarget`/`enableAllTargets` decide which physical RCB
    instance (`urcbA` vs `urcbB` vs...) gets which chunk — today `ReportControlBlockTarget` doesn't
    carry an "instance index" concept at all, this would need to be added or derived from the
    RCB's own resolved runtime name (`resolveRcbRuntimeName`'s `01`/`02`/... suffix, or the
    `urcbA`..`urcbJ` letter itself).
- **Track a per-connection dataset-count budget/counter** (decremented on every successful
  `createDataSet`), not just a per-LN dedup cache — needed for both the SCL-known-budget case
  (stop cleanly at N) and the empirical case (stop on the first "small dataset rejected" signal).

## Key files/functions to start from in the next chat

- `station_signal_daemon/src/features/ied_model/data/ied_model_scl_loader.c` — where `<Services>`
  parsing would be added (no existing pass to extend; would be new).
- `station_signal_daemon/src/features/ied_model/domain/ied_model_types.h` /
  `service/ied_model_api.h` — where the new `DynDataSet max`/`maxAttributes` accessor would live.
- `station_signal_daemon/src/features/mms_report_client/data/mms_report_client_connection.c` —
  `getOrCreateDynamicDataset`, `buildDynamicDatasetName`, `dynamicDatasetCache`/
  `DynamicDatasetCacheEntry`, `enableOneTarget`/`enableAllTargets` — all of the actual
  chunking/budget/RCB-instance-assignment logic lands here.
- `station_signal_daemon/src/features/ied_model/domain/ied_model_usecases.c:803-837` —
  `IedModelUseCases_getReportableAttributeReferencesForLogicalNode`, the existing flat
  member-reference list the DO-grouping step would parse (3rd `$`-segment = DO name).
- `IEC61850-Station.scd` (repo root) — real reference for `<Services><DynDataSet .../></Services>`
  shape and the escaped-RCB `<RptEnabled max="N">` shape, for building realistic test fixtures.
- `log_run.log` (repo root) — the real device run analyzed in this session; useful ground truth
  for whatever fixture/test is built to reproduce the count-budget-exhaustion and
  buffered-RCB-rejection behaviors.

## Steps to work through when writing the actual implementation plan

1. Decide and confirm the exact `<Services><DynDataSet>` SCL shape/schema (optional attributes?
   what if only one of `max`/`maxAttributes` is present?) before writing the parser.
2. Design the new `ied_model` accessor's shape (single struct? two separate getters?) and whether
   it belongs on the IED as a whole or needs to vary per-AccessPoint/per-Server.
3. Design how a chunk maps to a physical RCB instance concretely — what changes on
   `ReportControlBlockTarget`/`ReportControlBlock_create` (or a new sibling structure) to carry an
   assigned chunk index, and how `enableAllTargets`'s existing per-target loop needs to change to
   assign chunks instead of deduping.
4. Design the per-connection budget counter's lifetime/reset points (new connect cycle after a
   reconnect — does the device's own dataset count actually reset, or do previously-created
   association-scoped datasets from THIS connection still count until this exact connection
   closes? tier-2 pull-live behavior may interact here too).
5. Only after 1-4 are settled for the SCL-available case: revisit the no-SCL empirical-discovery
   design (deferred, per the user's explicit request this session) — retry/backoff strategy,
   whether discovered caps persist across reconnects, false-positive avoidance.
6. Testing: `integration_tests/mms_report_client/` already runs against a real `ied_simulator` IED
   — check whether `ied_simulator`'s model generator can be configured with a small `DynDataSet
   max`/`maxAttributes` (or a fixture LN with many attributes) to reproduce chunking/budget
   exhaustion deterministically, without needing the real device.
