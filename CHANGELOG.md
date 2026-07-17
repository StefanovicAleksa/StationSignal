# Changelog — ied_reporter_daemon

Full incident-by-incident history behind the current-state facts in `CLAUDE.md`. This file is
the historical record: root causes, real-hardware findings, reasoning behind reversals, and the
sequence of design decisions that produced the system `CLAUDE.md` describes today. `CLAUDE.md`
itself should stay a compact, current-state-only reference; new history belongs here, not there.

## `main.c` / daemon shape evolution

`src/main.c` now wires the real thing: `OrchestrationConfig_defaults` (which also defaults
`config.ipcDispatcherConfig`, overridable via an optional 5th argv slot for the websocket
port) -> `Orchestration_create` -> registers the three remaining `printf`-based
connection-state/RCB-status/liveness-diagnostic passthroughs (report/GOOSE DATA records have
no caller-facing setter at all anymore - see `ipc_dispatcher`'s own bullet below for why) ->
(if `host` argv was omitted/empty: `ied_discovery`'s interactive scan/manual-add/pick flow via
`src/main_discovery_prompt.c`, see that feature's own bullet below) -> `Orchestration_run`
(port/interface from argv; host either argv-supplied or the just-picked one; IED name either
argv-supplied or empty, triggering `orchestration`'s own auto-detect) -> blocks on
`SIGINT`/`SIGTERM` -> `Orchestration_destroy`. Notably, `main.c` never includes
`features/ipc_dispatcher/service/ipc_dispatcher_api.h` at all - orchestration owns that
feature's entire lifecycle end-to-end, the same way it already owns ied_model/
mms_report_client/goose_subscriber's. All six `src/features/` (`scl_bootstrap/`,
`ied_model/`, `mms_report_client/`, `goose_subscriber/`, `ipc_dispatcher/`, `ied_discovery/`)
plus `src/orchestration/` were implemented at this point (every feature named in the original
Expected-features list exists, `ied_discovery` being a later, deliberate, user-requested
addition beyond the original five).

**`main.c`'s `host`/`iedName` argv no longer default to `"127.0.0.1"`/`"Reporter1"`** — those
only ever matched the bundled `integration_tests/ied_simulator` fixture and directly
contradicted the point of `ied_discovery` (an operator who doesn't already know the IP
shouldn't need to already know the IED name either). Omitting `host` now triggers the
interactive discovery flow instead of silently defaulting to the test simulator's address; no
automated test depended on the old defaults (every `tests/`/`integration_tests/` case calls
`orchestration`'s API directly, never the compiled `main.c` binary).

**`main.c`'s `host`-omitted flow was rewired from a one-shot blocking scan to a continuous
background scan (`scan_orchestration`)**, so `main.c` can prove the new process actually works
end-to-end the same way it already proves orchestration's report/GOOSE pipeline works — this
mirrors the daemon's broader direction of becoming a set of background processes eventually
managed by an external Go API + local-network frontend (not built yet), rather than a
one-shot CLI tool. Previously `main_discovery_prompt.c` called `IedDiscovery_scanSubnet` once,
blocking until the whole sweep finished, then presented a static list. `main.c` started a
`ScanOrchestration` scan (interactive-flow default, on `interface`), registered a printf
device-found callback (same style as the existing `onReportConnState`/`onRcbStatus`/
`onGooseStatus` passthroughs) so discoveries printed as they streamed in, and
`main_discovery_prompt.c`'s loop re-snapshotted the scan's live, growing host list
(`ScanOrchestration_snapshotDiscoveredHosts`) fresh before every prompt reprint instead of
reading a static list — the scan kept running concurrently in the background the whole time
the operator was at the prompt. The manually-typed-IP path was unchanged (still a separate,
plain `IedDiscoveryHandle` calling `IedDiscovery_verifyHost` directly — `ied_discovery` itself
was not modified at all by this change). Once a device was picked (from the list or manually
verified), `main.c` stopped that scan (`ScanOrchestration_stopScan` — may block, see
`scan_orchestration`'s own documented limitation; `main.c` printed a "Stopping scan..."
diagnostic first) before falling through to the existing, completely unmodified
`scl_bootstrap`/`orchestration` pipeline. All of this is now historical only — see below.

**`main.c` was rewritten again to support reporting on MULTIPLE IEDs at once** (explicit user
request) — its primary long-lived job became `device_manager` + `control_dispatcher`, not one
fixed boot-time device. The entire inline `if (sclFilePath) {...} else {...}` bootstrap-fallback
block plus the direct `Orchestration_create`/`Orchestration_set*Callback` calls this bullet
originally described above were GONE from `main.c` — extracted verbatim into
`device_manager/domain/device_manager_bootstrap_policy.c` so both `main.c`'s own boot-time
device and `control_dispatcher`'s worker thread would share exactly one copy. **Consequence, an
accepted simplification, not a silent regression**: the three diagnostic `printf` passthroughs
(`onReportConnState`/`onRcbStatus`/`onGooseStatus`) went away too — `device_manager` creates
each device's own `OrchestrationHandle` internally and never exposes it to `main.c`, so there
was no attachment point for them anymore; the control websocket became the real
diagnostic/status interface going forward, not process-local `printf`. **Breaking argv change**:
the old 5th argv slot (an `ipc_dispatcher` port override) went away — there was no longer one
fixed dispatcher to point an override at, since `device_manager` owns per-device port allocation
from its own configured range; `acseAuthPassword`/`sclFilePath` each moved one slot earlier as a
result. The `host`-omitted scan flow (previous paragraph) was otherwise unchanged in spirit —
once it picked a host, that host was handed to the same `DeviceManager_startReporting` call the
argv-supplied-host path used, instead of calling `Orchestration_run`/`_runFromLocalFile` directly
the way this file used to — one path for "start a device" instead of two divergent ones. A
failed boot-time device (scan picked nothing, or `DeviceManager_startReporting` itself failed)
was no longer fatal to the whole process either — `control_dispatcher` stayed up regardless,
since it was this daemon's real interface now. `rebuild_proj.sh` and the manual build command
were updated to include `src/device_manager/*/*.c` alongside `src/orchestration/`/
`src/scan_orchestration/`'s own `.c` files (the latter two were themselves missing from
`rebuild_proj.sh` until this pass — a gap from `scan_orchestration`'s own original commit, fixed
here since `main.c` cannot build without them regardless).

**`main.c` was rewritten once more, most recently, to remove its remaining terminal/CLI surface
entirely** (explicit user request, turning the daemon into a pure background process-runner for
an external API layer) — every earlier description above of argv slots, a boot-time device, or
an interactive discovery prompt is now historical only and no longer reflects current behavior.
`main.c` now takes **zero arguments** (`int main(void)`) and does nothing but create
`device_manager` + `scan_orchestration` + `control_dispatcher`, start the one always-on control
websocket, and block on `SIGINT`/`SIGTERM` until torn down in the same reverse order as before.
The entire `host`-omitted background-scan-plus-interactive-prompt block and the
argv-supplied-host boot-time `DeviceManager_startReporting` call (both described above) are
gone, along with `src/main_discovery_prompt.c`/`.h` (deleted — nothing else referenced them).
`control_dispatcher`'s `START_SCAN`/`STOP_SCAN` and `START_REPORTING`/`STOP_REPORTING` commands
are now the daemon's **only** way to start/stop anything — this was already fully implemented
and tested (the scan actions had landed in an earlier pass this doc had failed to document at
all until this rewrite, a real doc/code drift caught while planning this change).
`rebuild_proj.sh` and the manual build command were updated to drop
`src/main_discovery_prompt.c`. `ied_discovery`'s own service header doc comment was updated to
stop referencing `main_discovery_prompt.c` as the discovery interaction medium.

## `mms_report_client` / `goose_subscriber` rollback and reconnect bugfix family

**Bugfix 1, surfaced by wiring `ipc_dispatcher` into orchestration's own rollback paths**:
`MmsReportClientConnection_destroy` (`src/features/mms_report_client/data/mms_report_client_connection.c`)
used to destroy `handle->wakeSignal` (the semaphore) *before* `IedConnection_destroy` - but
`IedConnection_destroy` can synchronously re-fire `onStateChanged` (it internally closes the
connection again even if `MmsReportClientConnection_stop` already closed it), which
unconditionally posts that semaphore. This use-after-free crashed intermittently whenever a
caller destroyed an actively-connecting client - previously unreachable in practice because
the only code path that destroys an already-started `MmsReportClientHandle` mid-connection is
orchestration's own "a later stage failed, roll back the already-started report client"
rollback branch, and every prior orchestration E2E run happened to have GOOSE succeed (via
`sudo`), so that branch was never actually exercised until orchestration started deterministically
reaching it in an environment without `CAP_NET_RAW`. Fixed by destroying the connection before
the semaphore (reverse of the old order) - see that function's own comment for the full story.

**Bugfix 2 in the same rollback family, surfaced by real-hardware testing against a ~40-RCB
device**: `enableAllTargets`/`enableOneTarget` (same file as above) used to loop through every
cached RCB target with no check for a concurrent stop request. When orchestration's fail-hard
rollback calls `MmsReportClient_destroy` on an already-started client (e.g. because GOOSE
subscriber start failed on a bad/absent network interface) while the supervisor thread is still
mid-loop enabling later RCBs on a separate thread, `MmsReportClientConnection_stop`'s
`IedConnection_close` races that in-flight loop with no coordination - the RCB being processed
at that exact moment fails (a timeout, since its connection is being pulled out from under it -
inherent and accepted, not fixable without deeper library-level synchronization), and *every*
remaining target then also gets attempted and fails immediately with `IED_ERROR_CONNECTION_LOST`
- a long, noisy, entirely wasted cascade (confirmed directly: one interface failure turned into
~40 doomed MMS round-trips and ~40 spurious error lines). Previously unreachable in practice
for the same reason as the bugfix above - every fixture/E2E test has 2-4 RCBs on loopback,
finishing the whole enable loop in well under a millisecond, nowhere near enough of a window for
a concurrent rollback to land mid-loop. Fixed by checking `handle->stopRequested` once per
`enableAllTargets` loop iteration and again at the top of `enableOneTarget` (defense-in-depth for
the narrow gap between the loop's own check and the call actually landing) - turns the cascade
into one expected failure plus a prompt, quiet stop. Also relevant if this same rollback fires
when `Orchestration_runFromLocalFile`/`_runFromOnlineDiscovery`'s own GOOSE stage fails after
`mms_report_client` has already started - identical race, same fix, since all three entry points
share this one `runFromIedModelHandle` tail.

`mms_report_client` gained ACSE password authentication (`MmsReportClientConfig.acseAuthPassword`,
new `data/mms_report_client_auth.c`) — previously only `scl_bootstrap`'s SCL-discovery
connection could authenticate, so a real IED requiring auth on every association would let
the daemon discover+fetch its SCL but then fail outright to establish the actual reporting
connection. `src/main.c` exposed this as an optional argv slot, reused for both
`config.bootstrapConfig.acseAuthPassword` and `config.reportClientConfig.acseAuthPassword`
(same physical IED, same credential, two independent `IedConnection`s) — unlike
`scl_bootstrap`'s retry-on-rejection approach, `mms_report_client` applies the configured
password unconditionally from the very first connect attempt, since it always targets one
already-known IED with no ambiguity to resolve via retry.

**Both reporting workers were made strictly event-driven, closing a gap where periodic/non-event
wire traffic could otherwise reach `ipc_dispatcher` looking like a real event.**
`mms_report_client` never writes `TrgOps`/`IntgPd` when enabling an RCB (unchanged, deliberate),
so a real IED whose SCL configures `period="true"` (integrity triggering — common in real vendor
exports, confirmed against this repo's own fixtures) will genuinely push periodic integrity
reports forever; the one-shot `GI` snapshot on enable is also not a "something changed" event.
`mms_report_client` filters via a per-position value-diff cache (`domain/mms_report_client_usecases.c`'s
`buildEntries`, through `shouldForwardAndUpdateCache`, backed by
`MmsReportClientMemberRefCacheEntry.lastForwardedValues`, built once per RCB alongside the
Gap 4 cache): forwarded only if its value differs from the last one actually forwarded for that
exact wire position. **`shouldForwardAndUpdateCache` ALWAYS runs this diff-check, for every
entry, regardless of the server's own `ClientReport_getReasonForInclusion` bitmask — reason is
never trusted to bypass it.** This wasn't always true: an earlier version of this function
trusted a real-change bit (`DATA_CHANGE`/`QUALITY_CHANGE`/`DATA_UPDATE`,
`MmsReportClientUseCases_hasRealChangeReason`) as an unconditional "skip the diff-check, always
forward" signal — real-hardware testing against a live IED proved that trust unsafe: the device
tagged hundreds of consecutive, byte-identical reports as `DATA_CHANGE` even though the value
never actually changed (confirmed directly via `previousValue == value` on every one of them —
see `previousValue`'s own field in the current CLAUDE.md). Since `GI` and `DATA_CHANGE` are
independent, combinable `ReasonForInclusion` bits (confirmed against
`third_party/include/iec61850_client.h` — nothing stops a server from setting both at once), this
also explains "GI reports reaching the frontend": a GI-triggered entry that also happened to
carry a real-change bit used to bypass bootstrap-suppression the same way.
`MmsReportClientUseCases_hasRealChangeReason` was deleted entirely (no longer called anywhere)
once this landed — `reason` is still carried on `MmsReportEntry` as informational metadata, just
never consulted for the forward/drop decision. `goose_subscriber`'s equivalent function was
never affected by this bug (GOOSE has no `ReasonForInclusion` concept at all and has always
diff-checked unconditionally) — that asymmetry (GOOSE working correctly while MMS flooded) is
what first narrowed the bug down to this one function.

The "nothing cached yet" case (`cached == NULL`) is a **bootstrap** event —
`shouldForwardAndUpdateCache` silently seeds the cache from it but returns `false` (never
forwarded). This became **unconditional**, for the same reason the reason-trust bypass above had
to go entirely, not just partially: at the time this fix landed, a reconnect reset this same
cache to `NULL` on every (re-)enable (`MmsReportClientUseCases_resetValueDiffCache`, called from
`enableOneTarget` — **this reset mechanism was later removed entirely, see the "cache is now
never reset" redesign further below** — the cache became populated once and preserved forever,
and `cached == NULL` is now only expected on a position's genuine first-ever observation), so
"first observation, cached == NULL, tagged with a real-change reason" was *structurally
indistinguishable* from "a reconnect's redelivered-but-unchanged report, cache freshly reset,
spuriously tagged with a real-change reason" — the exact pattern the real device demonstrated.
Trusting reason on the `cached == NULL` branch for one would necessarily also trust it for the
other, reopening the same bug — this reasoning is why `reason` stays untrusted unconditionally
even after the reset mechanism itself was later removed.

`mms_report_client` at that point **stopped requesting GI at all, on any enable**
(`MmsReportClientConfig.generalInterrogationOnEnable` was removed entirely, along with the
`ClientReportControlBlock_setGI`/`RCB_ELEMENT_GI` branch in `enableOneTarget`) — GI proved
unreliable on real hardware (see above), and this made `mms_report_client` structurally
identical to `goose_subscriber` in this respect: no artificial snapshot was requested at
all, matching GOOSE's own GI-less design exactly (a foreign client's own GI, or a buffered
RCB's redelivery on re-enable, could still produce a report this client observed with
`reason=GI` or a stale-looking `DATA_CHANGE` — those were still handled correctly, just never
requested by us). **Accepted consequence, previously observable only by deliberately disabling
GI in a test, now true unconditionally at that time**: nothing artificially seeded the cache on
enable, so a device's first-ever genuine change was *also* bootstrap-suppressed, exactly like a
GI snapshot would have been — visibility resumed starting with the second transition, whose
`previousValue` correctly reflected the first (silently-seeded) one.
`integration_tests/mms_report_client/`'s tests each performed an explicit throwaway seed flip
before the one they actually asserted on, documenting this. GI was later reinstated — see below.

**The per-position value-diff filter above, by itself, broke `ipc_dispatcher`'s quality pairing**
(found against real production traffic): quality (`q`) almost never changes value
report-to-report and rarely carries a real-change reason on a report triggered by its sibling
value changing, so after the first `GI` snapshot `q`'s own diff-check dropped it on every
subsequent report while its value sibling (e.g. `stVal`) kept forwarding — and since
`ipc_dispatcher`'s `IpcDispatcherUseCases_pairQuality` only paired entries present in that same
record, quality showed as `null` forever after the first report. Symmetrically, a genuine
quality-only change (no value change) left a lone `q` with no forwarded value sibling, which
`pairQuality` also dropped outright — a real quality-degradation event vanished silently too.
Fixed by making `buildEntries` group-aware: it runs in three phases (candidate collection →
per-candidate value-diff-filter decision → a group-extension pass) instead of a single
decide-and-emit pass. A candidate that didn't individually qualify still forwards if ANY other
candidate resolving to the same group anchor does. One unified "any group member qualifies →
forward the whole group" rule handles both directions (value drags quality along; quality drags
its value sibling(s) along) with no special-casing of which DA is "the value" vs "the quality" —
confirmed by the user's explicit choice to keep this full symmetry rather than narrow it, even
knowing it means a sibling DA that doesn't change in lockstep with its group (e.g. `t`/`stSeld`
on a real device that doesn't bump them on every `stVal` change) gets resent unchanged whenever
any other group member does. A candidate whose reference doesn't resolve to any anchor at all is
its own ungroupable singleton, falling back to the exact pre-existing solo diff-check.

**Group anchor resolution is an ancestor walk, not a single-`$`-strip** (found against real
measured-value traffic: a CMV's nested `cVal$mag$f` chain never paired with its own quality,
which lives 3 `$`-segments shallower at the CMV instance's own level, e.g. `...phsA$q` vs
`...phsA$cVal$mag$f` — a single last-`$` strip lands on `...phsA$cVal$mag`, never matching).
Every `q`-named candidate's own prefix (up to its last `$`) becomes an "anchor"; every candidate
(including `q` itself) resolves to the LONGEST anchor it's genuinely nested under (starts with
`anchor$`) — a flat attribute (`Pos$stVal`) resolves directly to its DO's own `q`, exactly as
before; a deeply nested CONSTRUCTED-DA chain resolves to the same DO/SDO-level anchor several
segments up, not whatever its own last segment happens to be. Candidates resolving to the same
anchor are one group. `ipc_dispatcher`'s `IpcDispatcherUseCases_pairQuality` has the identical
ancestor-walk fix (`findQualityIndexForValue`) for the same reason, on the pairing side —
both sides need it: this feature's grouping decides whether quality is even present in a given
record, `ipc_dispatcher`'s pairing decides whether a present quality gets attached to the right
value in the JSON. Proven via new unit tests in both features' usecases test files, covering
the nested-CMV case, an anti-overreach case (two independent CMV instances under a shared
higher ancestor must never cross-pair), both drag directions, the both-unchanged case, the
ungroupable-singleton fallback, and the same grouping working across Gap 4 decomposition (two
leaves of one DO-level FCDA, not just two independently-authored FCDA entries).

Symmetrically, `goose_subscriber` used to forward a `GooseSubscriberRecord` on every accepted
GOOSE frame, including ordinary `MinTime`/`MaxTime` heartbeat retransmissions (same `stNum`,
incremented `sqNum`) that carry no actual data change — `GooseSubscriberUseCases_isDuplicateStNum`
plus a new per-target `hasForwardedStNum`/`lastForwardedStNum` pair (`GooseSubscriberTargetEntry`,
written only by the frame adapter thread, deliberately not under `targetStateLock` since the
liveness thread never touches them) now skips forwarding when a frame's `stNum` matches the
last one actually forwarded for that target; a STALE/INVALID_STATE→VALID transition resets this
so the next real frame is always delivered at least once. Neither change touches
`ipc_dispatcher`, which stays reason/stNum-unaware — filtering happens entirely upstream.

**Bugfix 3 in the rollback/reconnect family, surfaced by real-hardware testing**: after the
reason-trust removal above, a real device still showed the exact same flood of forwarded
`MMS_REPORT` messages with `previousValue == value` return after being physically disconnected
and reconnected. Root-caused to two compounding bugs in `mms_report_client_connection.c`, both
now fixed, neither related to `ReasonForInclusion` (which was already fully untrusted by this
point) — confirming `shouldForwardAndUpdateCache` itself was already byte-for-byte identical to
`goose_subscriber`'s own equivalent, and the recurrence had a purely structural cause instead:
(1) **reconnect "storm"**: `onStateChanged` posts `handle->wakeSignal` on *every* state
transition, not just `IED_STATE_CLOSED` (`connectionLostSignal` is only set for `CLOSED`) — and
`IedConnection_connect()` itself drives `CONNECTING` then `CONNECTED`, each posting once, so by
the time `supervisorLoop` first reached its wait after a successful connect, wakes were already
pending. The old code treated *any* wake with `connectionLostSignal == false` as "spurious, go
reconnect anyway" (`continue` back to the top of the outer loop), so one real connect could
trigger `enableAllTargets()` — a fresh RptEna cycle per RCB (this repo's own reference client
requested GI too, at the time) — more than once, back to back, with nothing having actually been
lost. Fixed by replacing that `continue` with an inner wait loop that stays in the same connected
phase, consuming every wake, until a genuine `connectionLostSignal` (or stop) arrives.
(2) **unsynchronized cache access**: `enableOneTarget`'s
`MmsReportClientUseCases_resetValueDiffCache` call runs on the supervisor thread, while
`mms_report_client_report_adapter.c`'s `onReport` reads/mutates the exact same
`lastForwardedValues` slots (via `MmsReportClientUseCases_buildReportRecord`) on libiec61850's
own report-reader thread — `grep -rn "Mutex\|lock" src/features/mms_report_client` returned zero
hits before this fix, so nothing prevented these two threads from racing on the same `MmsValue*`
slots. Combined with (1)'s storm (more overlapping enable cycles means more contention windows),
this is the most direct explanation for the exact symptom observed: a torn/use-after-free-style
read of a cache slot concurrently being reset can end up aliasing the incoming value, defeating
the diff check and forwarding a false "change" with a corrupted previous value. Fixed by adding
a binary-mutex `Semaphore memberRefCacheLock` to `struct sMmsReportClientHandle` (created/
destroyed alongside `wakeSignal`, same `Semaphore_create(1)` idiom as `goose_subscriber`'s own
`targetStateLock`), held around both the reset call in `enableOneTarget` and the
`buildReportRecord` call in `onReport`. **Why GOOSE never needed this**: `goose_subscriber`'s
own reset-then-process happens synchronously, in one function call, on the single `GooseReceiver`
reception thread (`GooseSubscriberFrameAdapter_onGooseReceived`) — no second thread ever touches
its `lastForwardedValues`. MMS can't be restructured to match that shape (the reset is tied to an
async `setRCBValues` write while reports arrive on a separate library-owned thread), so an
explicit lock is what achieves the same mutual-exclusion guarantee GOOSE gets for free. Proven
via a strengthened `integration_tests/mms_report_client/` reconnect test asserting a single
reconnect now produces exactly one additional RCB-enable event, not two-or-more.

**Bugfix 4 in the same family, plus a deliberate scope-narrowing, both at explicit user
request**: even after bugfix 3 above, the reset in `enableOneTarget` still ran *after*
`IedConnection_setRCBValues` returned, not before — a real, if narrower, race than (2) above:
the write that enables reporting (and, at the time, requested GI) can itself trigger a report
dispatched on libiec61850's own report-reader thread before the supervisor thread gets back
around to resetting the cache a few lines later, diffing that report against a STALE
(pre-disconnect, on reconnect) cache instead of a freshly-cleared one — plausibly explaining a
real-device burst where everything looks "changed" right after a connect/reconnect, independent
of the already-fixed storm/locking bugs. Fixed by moving the `memberRefCacheLock`-guarded
`MmsReportClientUseCases_resetValueDiffCache` call to run immediately after
`IedConnection_installReportHandler`, before the mask is built and `IedConnection_setRCBValues`
is even called — no report for this RCB can be dispatched before the reset has already run,
closing the window structurally rather than hoping the supervisor thread wins a scheduling race.
Resetting unconditionally, even if the subsequent write fails, was harmless (nothing can report
for a not-yet-enabled RCB either way). Separately, at the same request, GI was removed from this
feature entirely rather than merely left untrusted (the two changes shipped together since both
touch the same few lines of `enableOneTarget`).

**Change 5 in the same family — the value-diff cache became NEVER reset at all, on either
`mms_report_client` or `goose_subscriber`, at explicit user request**: every bugfix above
(storm/locking/reset-ordering) treated "reset the cache on every reconnect/recovery" as a given
and fixed increasingly narrow races around *when* that reset ran — but the reset itself was the
root of a separate, simpler problem: a device with a perfectly good last-known value lost it
every time the connection blipped, since a freshly-nulled cache turns the reconnect's own GI/
redelivered snapshot back into a bootstrap event (silently seeded, never forwarded,
`previousValue` left `NULL`) even though a real prior value existed a moment before. The fix:
`MmsReportClientUseCases_resetValueDiffCache`/`GooseSubscriberUseCases_resetValueDiffCache` were
deleted entirely (zero remaining callers) — `enableOneTarget`
(`mms_report_client_connection.c`) no longer resets anything before its enable write, and the
frame adapter (`goose_subscriber_frame_adapter.c`'s `onGooseReceived`) no longer resets anything
on a STALE/INVALID_STATE→VALID transition (it still resets the unrelated `hasForwardedStNum`
heartbeat-dedup flag there, untouched). The cache became populated **exactly once**, on a
position's genuine first-ever observation, and **preserved for the rest of the client/
subscriber's lifetime** (only freed at `STOP_REPORTING`/destroy, as before). GI (MMS) and the
liveness-recovery mechanism (GOOSE) were otherwise unchanged — still forced/detected on every
reconnect/recovery — but the fresh snapshot they produce now diffs against the **real, preserved
last-known value** instead of a wiped-clean one: a genuine change made while disconnected now
correctly forwards with a real, non-`NULL` `previousValue`; an unchanged resend is still
correctly suppressed by the ordinary diff check, no bootstrap logic or reset-timing race
involved. A new per-RCB/per-target `everPopulated` flag (`MmsReportClientMemberRefCacheEntry`/
`GooseSubscriberMemberRefCache`, set once at the end of the first report/frame either feature's
own `buildEntries` ever processes) exists purely to gate a debug check in
`shouldForwardAndUpdateCache` (both features): a cache slot found `NULL` *after* `everPopulated`
is already `true` should now be structurally impossible (nothing ever resets a slot back to
`NULL` again) — `fprintf(stderr, ...)` fires on every such occurrence, since it now signals a
real bug worth investigating on sight, not routine startup noise. `memberRefCacheLock`
(MMS)/`targetStateLock` (GOOSE) are unchanged — the MMS lock in particular is now technically
guarding a single-writer scenario (the report-adapter thread is the cache's only remaining
writer, since the supervisor thread no longer touches it), kept anyway as cheap, uncontended
insurance. Proven via new unit tests in both features' usecases test files driving
`buildReportRecord`/`buildRecord` multiple times in a row with no reset call anywhere in
between, asserting: the first call silently seeds the cache and sets `everPopulated`; a
simulated-reconnect/recovery call with a genuinely different value forwards with a real
`previousValue`; an unchanged resend is suppressed exactly like any other duplicate.

`ied_model`'s `IedModel_getGooseSubscriptionTargets` returns `GooseSubscriptionTarget*`
(object reference plus optional VLAN/APPID/dst-MAC parsed from SCL's `<GSE><Address>`),
not a bare `char*` — this was a breaking change made when `goose_subscriber` needed the
addressing data to configure `GooseSubscriber_setDstMac`/`setAppId` filters; there were no
other consumers at the time.

`ied_model` also gained `IedModel_getDataSetMemberReferences` (ordered, heap-allocated
reference strings for one dataset, purely local — walks the already-parsed SCL `DataSet`,
never over-the-wire) and `GooseSubscriptionTarget` gained a `datasetReference` field
(mirroring `ReportControlBlockTarget`'s) — both added so `mms_report_client`/`goose_subscriber`
could label report/GOOSE entries by their dataset position.

## GI removal and reinstatement

**GI was later reinstated, at explicit user request, after real-world use surfaced a gap the
removal above didn't account for**: the removal's own reasoning ("bootstrap suppression alone
gets the same outcome without asking the device for one") assumed *some* report always arrives
to naturally seed `shouldForwardAndUpdateCache`'s cache first. That's false for a device with no
periodic integrity reporting (`IntgPd`) and no other traffic at enable time — without GI, the
cache is only ever seeded by whatever report happens to arrive first, and on such a device
that's the very first GENUINE value change after connecting. That change gets silently
bootstrap-suppressed right along with it (the exact same mechanism that correctly suppresses a
real GI snapshot), so only the *second* change onward was ever visible — a real device with this
reporting profile would appear to the frontend as if the filter were dropping legitimate
changes, because it was. `enableOneTarget` (`data/mms_report_client_connection.c`) now
unconditionally sets `RCB_ELEMENT_GI` and calls `ClientReportControlBlock_setGI(rcb, true)` on
every enable — first connect and every reconnect alike — no config knob (unlike the pre-removal
`generalInterrogationOnEnable` bool this codebase once had; nothing asked for a way to turn it
off this time). This is a narrow, deliberate use of GI: it exists ONLY to force an immediate,
deterministic snapshot at enable time, and that snapshot is NEVER trusted or forwarded — it lands
on the exact same `cached == NULL` bootstrap-suppression branch any other first observation
would, in `shouldForwardAndUpdateCache`. At the time this reasoning was written, the cache was
also reset immediately before this write on every enable, so GI's snapshot was always diffed
against a freshly-cleared cache — that reset no longer exists (see change 5 above): the cache is
populated once and preserved forever, so on a genuine first-ever connect GI's snapshot still
lands on the same `cached == NULL` bootstrap branch (the cache is still empty at that point), but
on every reconnect after that, GI's snapshot instead diffs against the real, preserved
last-known value from before the disconnect. Re-adding GI is safe this time specifically because
the OTHER half of the original bugfix — never trusting a report's `reason` bit for filtering —
was never touched or weakened; that, not GI's absence, is what made the original real-hardware
flooding bug possible, and it still applies unconditionally to every report regardless of
source (this client's own requested GI, a foreign client's concurrent GI, or a buffered RCB's
redelivered backlog). On a genuine first-ever connect specifically, the still-empty cache may
receive both a GI-triggered snapshot AND a buffered RCB's redelivered backlog carrying the same
live value in either order — whichever lands first hits the `cached == NULL` branch and seeds
the cache, whichever lands second is then a byte-identical duplicate and is suppressed by the
ordinary value-diff check instead; neither ever reaches the callback either way, so this is
order-independent by construction, not a race the implementation has to win. `goose_subscriber`
is untouched — it has no GI concept at all, and its own `cached == NULL` bootstrap suppression
(already order-independent for the same structural reason) needed no equivalent change.
`integration_tests/mms_report_client/`'s tests were updated to match the new behavior: the
"throwaway seed flip" pattern each test previously used to manually stand in for GI's absence is
gone — since GI now seeds the cache deterministically at enable time, the first flip in each
test is itself a real, immediately-forwarded change; the reconnect test's redelivery-suppression
assertion is unchanged in outcome but its comment now explains the GI/buffered-redelivery
order-independence above.

## Real-hardware comparison, decomposition, and EntryID bugs

**A second, distinct real-hardware bug was found shortly after the GI reinstatement above,
against a real production device**: right at connect, a buffered RCB forwarded the SAME report
content 3 times in a row — `value == previousValue` on every field — before settling into
correct filtering. Root-caused to `shouldForwardAndUpdateCache`'s diff-check
(`MmsReportClientUseCases_isDuplicateValue`, and the identical cross-RCB-dedup call in
`MmsReportClientUseCases_shouldForwardAcrossRcb`) calling libiec61850's `MmsValue_equals`
directly — a raw, byte-exact comparison, confirmed by reading the vendored source, that's wrong
for two IEC 61850 types that show up constantly in real report datasets: **`MMS_UTC_TIME`**
(`memcmp`s all 8 bytes, but the last byte is a `TimeQuality` flag — leap-second-known/
clock-failure/clock-not-synchronized/accuracy — not part of "when did this happen," and can
legitimately wobble right around a reconnect even though the displayed millisecond timestamp is
unchanged) and **`MMS_BIT_STRING`** (the wire encoding for CODEDENUM/Dbpos/Tcmd-style status
points — `memcmp`s the whole buffer INCLUDING unused padding bits, which real device firmware is
commonly inconsistent about zero-padding across different report-generation code paths, e.g. a
GI-triggered read vs. a live-change report — two values that decode to the identical integer,
and thus render identically in the JSON, can still fail a raw `memcmp`). Fixed by a new
`valuesAreSemanticallyEqual` (`domain/mms_report_client_usecases.c`) that type-switches: same
type required (preserves existing `MmsValue_equals` type-mismatch behavior — not the bug);
`MMS_UTC_TIME` compared via `MmsValue_getUtcTimeInMs` (the same accessor `ipc_dispatcher`'s own
value codec already uses to render this type, so "same JSON output" now correctly implies "same
by this filter" too); `MMS_BIT_STRING` compared via `MmsValue_getBitStringSize` (a guard) plus
`MmsValue_getBitStringAsInteger` (again, the same accessor the value codec already uses); every
other type falls through to `MmsValue_equals` unchanged. **Affects `goose_subscriber` identically**
(`GooseSubscriberUseCases_isDuplicateValue` and `shouldForwardAcrossTarget` both called
`MmsValue_equals` directly too — GOOSE frames carry the same DA types) — fixed with an
independently-duplicated copy of the same helper in `goose_subscriber_usecases.c`, per this
codebase's established per-feature-domain-layer convention. Proven via new unit tests
(`tests/mms_report_client/test_mms_report_client_usecases.c`,
`tests/goose_subscriber/test_goose_subscriber_usecases.c`) constructing same-value-different-
quality-byte `MMS_UTC_TIME` pairs and same-decoded-integer-different-size `MMS_BIT_STRING` pairs
directly — **note the exact real-world byte pattern (same declared size, differing UNUSED padding
bits) can't be reproduced via the public `MmsValue` API**: `MmsValue_setBitStringBit` itself
refuses to touch bit positions ≥ the declared size (confirmed directly in libiec61850's own
source), so every `MmsValue` constructible via the public API has its padding bits permanently
zeroed by `MmsValue_newBitString`'s own `calloc` — that gap only exists in a real device's own
wire encoding, not in anything reachable through well-behaved client code, which is exactly why
it was a genuine, hard-to-suspect field bug rather than something a normal test would catch.
Separately (not the cause of the wrong comparison, but why the burst happened specifically at
connect and then stopped): `supervisorLoop` used to reset `currentBackoffMs` to `0`
unconditionally on every *momentary* successful connect, before the connection proved it could
stay up — a real, flaky link that connects then bounces right back (unlike the clean loopback
simulator, which never does this) got stuck retrying at the initial ~1s backoff tier forever
instead of escalating, giving the comparison bug repeated fresh chances to fire in a tight burst
right after connect. Fixed by only resetting `currentBackoffMs` if the just-lost connection had
actually stayed up for `MMS_REPORT_CLIENT_STABLE_CONNECTION_MS` (`5000`ms) — reuses the existing,
already-tested exponential-backoff math (`MmsReportClientUseCases_computeNextBackoffDelay`)
unchanged, just fixes *when* it resets rather than inventing a new debounce mechanism.

**A third, distinct real-hardware bug was found shortly after, also surfacing after
disconnect/reconnect cycles on real hardware**: a structured attribute (`Pos`, a DPC on LN
`SCSWI2`) had its `stVal`/`t` sub-elements swapped — `stVal` showed a huge millisecond-timestamp
number, `t` showed a plain boolean. Root-caused to the Gap-4 structure-decomposition path
(`collectCandidates`, mirrored in `goose_subscriber_usecases.c`): it flattens a structured
attribute's wire value (`MmsReportClientUtils_flattenStructure`, walking the received
`MmsValue`'s own `MMS_STRUCTURE` element order) and zips it index-for-index against a
**locally-resolved** reference list built from this daemon's own parsed SCL file
(`IedModel_getDataSetMemberLeafReferences`, a depth-first walk of the SCL `<DOType>`'s literal
XML `<DA>`/`<SDO>` child order). The only safety check before trusting this zip was
`flattenedCount == memberLeafCounts[i]` — a bare **count** comparison; nothing verified
**order**, and nothing in MMS/IEC 61850 requires a `<DOType>`'s declared `<DA>` order to match a
real device's actual runtime attribute order (confirmed: `ClientReport_getDataReference`/
`OptFlds` `DataRef` cannot help here either — it's fundamentally per-top-level-dataset-member
(per-FCDA) only, with no protocol-level concept of a reference for a leaf *inside* a structured
value). This same-count-different-order gap was already documented as an *assumption* in
`IedModelUseCases_getDataSetMemberLeafReferences`'s own doc comment, but had no actual guard.
Fixed with a new `ied_model` accessor, `IedModel_getDataSetMemberLeafWireTypes` (mirrors
`_getDataSetMemberLeafReferences`/`_getDataSetMemberLeafSemantics` exactly — same index-aligned,
same decomposed-vs-leaf split — but reads each leaf's already-known `DataAttributeType` directly
off its `DataAttribute` node, set once at SCL-load time via `IedModelUtils_mapBType`; no new
SCL-parsing pass needed, since `struct sDataAttribute` already stores this field, fully exposed
in `iec61850_model.h`) plus a new `IedModel_dataAttributeTypeMatchesMmsType(DataAttributeType
expected, MmsType actual)` cross-check, called once per decomposed leaf
(`decomposedLeafTypesMatch`, both `mms_report_client_usecases.c` and its `goose_subscriber`
twin) alongside the existing count check — on ANY leaf's type mismatch, falls back to the raw/
non-decomposed entry exactly like a count mismatch already did (silently — same posture, no new
logging), rather than trusting a same-count-wrong-order zip. Only implements **confident**,
well-established type groupings (BOOLEAN↔`MMS_BOOLEAN`, TIMESTAMP↔`MMS_UTC_TIME`,
QUALITY/CODEDENUM/CHECK/GENERIC_BITSTRING/OPTFLDS/TRGOPS↔`MMS_BIT_STRING`, the INT*/FLOAT*/
ENUMERATED family↔`MMS_INTEGER`/`MMS_UNSIGNED`/`MMS_FLOAT`, VISIBLE_STRING*/UNICODE_STRING_255↔
`MMS_VISIBLE_STRING`/`MMS_STRING`) — per this codebase's "don't guess IEC 61850 semantics" rule,
anything not explicitly modeled (`IEC61850_UNKNOWN_TYPE`, `OCTET_STRING_*`, `ENTRY_TIME`,
`PHYCOMADDR`, `CURRENCY`, `CONSTRUCTED`) always matches (no check), rather than risk
false-positive-rejecting a genuinely well-ordered structure. Proven via new unit tests in both
`tests/ied_model/` (the new accessor + the type-compatibility matrix) and both
`mms_report_client`/`goose_subscriber` usecases test files (a same-count-different-order fixture
— a UTC_TIME value and a boolean swapped into a `stVal`/`t`-labeled pair, exactly reproducing
the real-hardware symptom — asserting fallback to the raw entry, plus a regression case proving
a genuinely-matching order still decomposes normally). No new E2E test: `ied_simulator` builds
its dynamic model directly from the same code that produces its own reference list, so wire
order and reference order are inherently always in sync there — this bug class can only be
exercised at the unit level with hand-built fixtures, which is what the existing Gap-4
decomposition tests already do. Also plausibly explains the "oscillating quality" symptom
observed alongside this on the same device: `q` and `stVal` (Dbpos-coded) are both wire-typed
`MMS_BIT_STRING`, so if the order mismatch zipped the `...Pos$q` reference to what was actually
the wire's `stVal` position, `ipc_dispatcher` would still successfully decode it as quality (it
only type-checks for `MMS_BIT_STRING`, which still passed) — just decoding the wrong bits; not
independently confirmed, would need real-hardware verification once this fix is deployed.

**A fifth distinct real-hardware bug, found via real websocket-output logs showing a buffered
RCB's `stVal` oscillating true/false every few ms with a frozen, hours-old `t` on every
reconnect**: `enableOneTarget` (`data/mms_report_client_connection.c`) enabled every RCB with
`RCB_ELEMENT_RPT_ENA | RCB_ELEMENT_GI [| RCB_ELEMENT_DATSET]` but never `RCB_ELEMENT_ENTRY_ID` /
`ClientReportControlBlock_setEntryId` — for a **buffered** RCB, that means the server has no way
to know what this client already received, so it redelivers its entire unacknowledged backlog on
every `RptEna` transition (every reconnect). Since the value-diff cache only remembers the
single last forwarded value, replaying the same multi-entry alternating backlog again defeats
it — entry 1 of the new redelivery looks "changed" relative to wherever the cache landed after
entry N of the previous redelivery. Root-caused against real hardware logs showing an
environment where reconnects are frequent (a gateway-less point-to-point link that Ubuntu
periodically deprioritizes, requiring manual re-enable) — not a rare edge case. Fixed by
persisting the most recently observed `ClientReport_getEntryId` per RCB
(`MmsReportClientMemberRefCacheEntry.lastEntryId`, written unconditionally in
`mms_report_client_report_adapter.c`'s `onReport` — independent of whether that report's own
entries survive the value-diff filter, since EntryID tracks "durably received," not "passed our
own forwarding decision" — and read in `enableOneTarget` on every (re)enable of a `target->buffered`
RCB, both sides guarded by the existing `memberRefCacheLock`). On the very first-ever enable
(`lastEntryId` still NULL) this is a no-op, same full-backlog behavior as before. If
`IedConnection_setRCBValues` fails with `RCB_ELEMENT_ENTRY_ID` set (server rejects an EntryID it
no longer recognizes — IEC 61850 leaves the exact failure mode here implementation-defined, not
guessed at), `enableOneTarget` retries once without it rather than leaving that RCB unreported.
GOOSE needs no equivalent — it has no buffered/EntryID-acknowledged delivery model at all.

**A second, distinct bug surfaced directly by testing this fix end-to-end**: the group-aware
quality-drag pass in `buildEntries` (see the quality-pairing bug above for the mechanism) drags
a sibling candidate into `forward[i]=true` purely by checking whether another candidate under
the same anchor forwarded — never checking whether the dragged-in candidate has a value of its
own in THIS report. A real buffered redelivery can legitimately carry a NULL element for one
dataset position (confirmed directly: `q`'s own `MmsValue_getElement` returned NULL on 3
consecutive redelivered entries in the E2E test this fix added) while its `stVal` sibling
forwards a real change — the old code dragged the NULL-valued `q` candidate in anyway, and
phase 3's unconditional `updateValueDiffCache(memberRefCache, c->slot, c->value)` then
overwrote `q`'s real cached value with NULL, silently corrupting the cache (a later report
would find `cached == NULL` after `everPopulated` was already true — the exact "this should
never happen" condition `shouldForwardAndUpdateCache` already logs loudly for). Fixed by
skipping any candidate with a NULL own value in the group-extension pass — mirrors the "never
overwrite a real cached value with NULL" principle `shouldForwardAndUpdateCache`'s own `!value`
branch already applies, just closing the same gap on the drag-in path.
`goose_subscriber_usecases.c` has the identical, independently-duplicated fix (same structural
gap, same per-feature-domain-layer duplication convention) even though it wasn't the feature
that surfaced it. Proven end-to-end via a new `integration_tests/mms_report_client/` test,
`test_secondReconnectWithNoNewChanges_doesNotRedeliverBacklog`: accumulates a multi-entry
alternating backlog on a real `ied_simulator` "Reporter1" instance while disconnected, reconnects
once (backlog must be delivered — real, wanted data), then forces a second reconnect with zero new
changes and asserts nothing is redelivered. Required adding `entryID="true"` to `brcbMain`'s/
`brcbDup`'s `<OptFields>` in both `fixtures/reporter1.cid` (client-side) and
`integration_tests/ied_simulator/src/sim_server.c`'s own `RPT_OPT_ENTRY_ID` flag (server-side) —
without it the server never includes an EntryID in the report at all, so there is nothing to
resume from; no other E2E test asserts on `entryId`, so this was a safe additive change to the
shared simulator.

## EntryID resumption — structural no-op finding, and the OptFlds fix

**The EntryID-resumption fix above turned out to be a structural no-op against the same real
device that originally surfaced it, root-caused via a temporary `ied_reporter_debug_entryid.log`
diagnostic (`data/mms_report_client_connection.c`'s/`_report_adapter.c`'s own `appendDebugLog`
calls, added specifically to settle this — still in place as of this writing, pending
real-hardware confirmation that the fix below actually gets the device sending EntryID now;
remove once confirmed — see `CLAUDE.md`'s Current State section)**: every single received report
(2581/2581 in one capture, across every RCB) carried no EntryID at all, confirmed by a new
per-report log line showing `entryId=(none)`/`hasSeqNum=0` unconditionally — meaning
`MmsReportClientMemberRefCacheEntry.lastEntryId` could never be populated, so
`RCB_ELEMENT_ENTRY_ID` never made it into the enable mask, and every reconnect kept falling
back to a full backlog resume regardless of how correct the resumption logic itself was. Root
cause: this RCB's `OptFlds.EntryID` simply isn't enabled in the device's own current
configuration, and `enableOneTarget` never asked for it — `OptFlds` (unlike `RptEna`/`GI`/
`DatSet`) was one of the attributes this feature deliberately left untouched, relying on
whatever the device already had configured. Fixed, at explicit user request, by having
`enableOneTarget` proactively OR `RPT_OPT_ENTRY_ID` into a buffered RCB's OptFlds on enable
(`ClientReportControlBlock_getOptFlds`/`_setOptFlds`, `RCB_ELEMENT_OPT_FLDS` in the mask) —
read the device's current OptFlds first and only add the one bit, never clobbering whatever
else is already configured (seqNum/timeStamp/dataSet/reasonCode/etc., all left exactly as they
are), and only written back (only added to the mask) if the bit isn't already set, since
OptFlds isn't expected to reset itself across reconnects the way `RptEna` does. Deliberate
choice over the alternative (a site-side SCL/engineering-tool config change enabling
`entryID="true"` directly on the device) specifically so this daemon degrades gracefully
against a device's out-of-the-box configuration instead of requiring a site visit first,
matching this project's broader goal of working robustly across different real-world
environments without assuming ideal IED configuration. The site-config alternative remains a
valid, arguably more "intended" fix and is worth knowing about if the client-side OR-in
approach ever proves insufficient (e.g. a device that outright rejects a client-initiated
OptFlds write) — in that case `RCB_ELEMENT_OPT_FLDS`/`setOptFlds` would fail the same way any
other `IedConnection_setRCBValues` element can, surfaced through the existing generic
error-and-log path, not a special case of its own.

## `mms_report_client` dynamic dataset creation

**Dynamically creates a dataset for RCBs whose SCL declares no `datSet` at all**
(`datSet="Dyn"` in SCL `<ReportSettings>` terms — confirmed against a real device,
`E13_6MD`/`IEC 61850v2 JA4 station.scd`: every one of its ~174 `ReportControl` elements omits
`datSet`, and RCBs there are parented under the specific LN they report on, not just `LLN0`,
contradicting the earlier assumption that an RCB's parent LN is always `LLN0`). Previously this
feature deliberately never created datasets itself (`setRCBValues` just failed with
`IED_ERROR_OBJECT_VALUE_INVALID`, logged, RCB skipped) — that stance blocked reporting entirely
on this whole class of device. Fixed via `data/mms_report_client_connection.c`'s
`getOrCreateDynamicDataset` (called from `enableOneTarget` only when
`target->datasetReference` is NULL), which synthesizes an association-scoped dataset
(`IedConnection_createDataSet` with an `@`-prefixed name — destroyed automatically when the
connection closes, so no explicit cleanup/leak risk across reconnects) covering **every FC=ST/MX
leaf attribute under the RCB's own LN** — "all the variables" for that LN, by this codebase's
existing FC=ST/MX "reportable" convention (see `IedModel_getReadTargets`). The member list comes
from a new `ied_model` accessor, `IedModel_getReportableAttributeReferencesForLogicalNode(handle,
lnReference)` (`ReportControlBlockTarget` gained an `lnReference` field for this), purely local
like every other `ied_model` accessor — never over-the-wire. `mms_report_client_api.c`'s
`buildMemberRefCache` uses this same accessor (not just the connection layer) to seed the RCB's
reference-labeling/value-diff cache up front, so dynamic RCBs get the exact same
reference-labeling/value-diff-filter treatment as SCL-declared ones, no special-casing
downstream. A new domain usecase, `MmsReportClientUseCases_buildWireMemberReferences`, converts
this codebase's standard `"$"`-joined reference form to `IedConnection_createDataSet`'s required
dot/bracket wire form. A per-connect-cycle cache (LN reference → generated dataset name, built
fresh in `enableAllTargets`, discarded at the end) de-dupes dataset creation across an LN's
redundant reserved RCB instances (e.g. `urcbA..urcbJ` all sharing one LN) — without it, a device
like `E13_6MD` would attempt to create the same dataset ~10× over just for one LN's reserved
slots. **Known, deliberately unsolved limitations**: no chunking against a device's
`maxAttributes` cap (an LN with more reportable leaves than the cap fails `createDataSet` for
that LN, falls back to the pre-existing failure mode); no handling of a device's total
dataset-count cap being smaller than its unique-LN count (per-LN scope, not per-LDevice) — both
are honest, unresolved trade-offs from a design discussion that intentionally deferred multiple
stakeholder-specific scope questions rather than guessing. Proven end-to-end against a real
`ied_simulator` IED in `integration_tests/mms_report_client/` (a fixture RCB parented under a
non-`LLN0` LN, no `datSet` at all, mirroring `E13_6MD`'s real shape).

## `ipc_dispatcher` quality/label/CODEDENUM history

Quality (`q`) pairing — flagged as unbuilt for a long time — was eventually solved in
`ipc_dispatcher`: `domain/ipc_dispatcher_usecases.c`'s `IpcDispatcherUseCases_pairQuality`
(reference format confirmed via `IedModelUseCases_getDataSetMemberReferences`:
`"<LDName>/<LN>$<FC>$<DO>$<DA>"`) finds each value's `q` sibling by walking **up its own
ancestor prefixes** (one `$`-segment at a time via `findQualityIndexForValue`), not just
stripping the last `$` — see the ancestor-walk description above for the full mechanism and why
the original single-`$`-strip implementation only ever found quality for flat attributes.

`MmsValue` scalars are converted to JSON-friendly types by `MmsValue_getType()`
(`utils/ipc_dispatcher_value_codec.c`) — boolean/integer/unsigned/float/string/UTC-time map
directly; **`MMS_BIT_STRING` maps to a raw unsigned integer** (`MmsValue_getBitStringAsInteger`)
— covers CODEDENUM-typed value DAs (`Dbpos`/`Tcmd`, per `IedModelUtils_mapBType`, e.g. a
breaker's `Pos.stVal`) that wire-encode as a bitstring and previously fell through to the
unsupported placeholder (found against real production traffic). Deliberately a raw integer, not
a named enum string (e.g. Dbpos's own `0`=intermediate-state/`1`=off/`2`=on/`3`=bad-state per IEC
61850-7-3) — this function, by itself, has no way to know which specific CODEDENUM a given
bitstring represents (Dbpos and Tcmd share the same wire type but different meanings), and
guessing a decoded label without per-type verification would violate this repo's own "don't
guess IEC 61850 semantics" rule; the raw bit pattern is always correct regardless. Quality's own
bitstring never reaches this path at all — `IpcDispatcherUseCases_pairQuality` excludes every
`q`-named entry from ever being treated as a value, routing it to `_decodeQuality` instead, so
this addition can't double-decode or conflict with quality handling.

**This codebase previously carried a descriptive-label feature for the specific `Dbpos` case
(an SCL-derived semantic side table plus an additive `"label"`/`"previousLabel"` JSON field),
removed again at explicit user request** so every CODEDENUM value (`Dbpos`, `Tcmd`, or any
other) is reported identically — a raw integer via `MmsValue_getBitStringAsInteger`, no
per-type special casing anywhere in `ied_model`/`mms_report_client`/`goose_subscriber`/
`ipc_dispatcher`. Removal was driven by the same reasoning the original addition's own caveat
already flagged: treating one CODEDENUM subtype differently from the rest was itself the
discrepancy the user wanted gone, not a value worth keeping despite it.

## `ied_model_online_loader` real-hardware bug

**A fourth bug, found against real production hardware (a device whose MMS server never serves
an SCL file, forcing it through this loader): every MMS report from such a device showed EVERY
data point as `"<unsupported:structure>"` with `quality: null`, while GOOSE on the same device
worked fine.** Root cause: `IedModelOnlineLoaderUseCases_convertAcsiRefToWireRef`
(`domain/ied_model_online_loader_usecases.c`) built its output using the ACSI reference's whole
`"LD/LN"` prefix, unsplit, as the first `$`-segment (e.g.
`"VR4C1C01A1LD0/SP16GGIO5$ST$Ind"`) — handed straight to `DataSetEntry_create` as its
`variableName`, directly violating this file's own documented dynamic-model gotcha #1 (no
LD-wire-name prefix) that the call site's own comment claimed was already satisfied but wasn't.
Consequence: `IedModelUseCases_getDataSetMemberLeafReferences` (`ied_model`, shared by both
`mms_report_client`/`goose_subscriber`) tokenizes `entry->variableName` on `$` and passes the
first token straight to `LogicalDevice_getLogicalNode` as a bare LN name — for every
online-discovered member this token was actually `"LD/LN"` combined, so the LN lookup silently
failed, `IedModel_getDataSetMemberLeafReferences` returned an empty (not NULL) list — outwardly
indistinguishable from "already a leaf, nothing to decompose" — Gap-4 structure decomposition
was never attempted for any online-discovered DO-level member, and the raw, undecomposed
`MMS_STRUCTURE` reached `ipc_dispatcher`'s value codec, which has no branch for it. GOOSE wasn't
affected on this particular device only because its own GOOSE dataset(s) happen to be
leaf-level already (or don't include this structured DO) — it shares the exact same buggy
conversion and would hit the identical failure for any DO-level GOOSE dataset member. Fixed by
splitting the `"LD/LN"` prefix on `/` inside `convertAcsiRefToWireRef` and using only the LN
portion in the output, matching the SCL loader's own convention exactly; the misleading "No
LD-name prefix" comment at the `DataSetEntry_create` call site
(`data/ied_model_online_loader_connection.c`) was corrected to describe what the code actually
does now instead of asserting something false. Proven via a new
`tests/ied_model_online_loader/test_ied_model_online_loader_usecases.c` (a direct regression
test: a DO-level and a leaf-level ACSI reference each now convert to a bare-LN-prefixed wire
reference, plus the pre-existing malformed-input/NULL cases) — no existing E2E fixture exercises
Gap-4 decomposition through this loader (`integration_tests/ied_model_online_loader/`'s own
assertions stop at report/GOOSE target-list shape, not per-report leaf decomposition), so this
is unit-level-only coverage for now.

**A third dynamic-model construction gotcha, found the hard way while building this feature's
own E2E test** (alongside the two already documented from `ied_simulator`):
`LogicalDevice_create(name, parent)` implicitly PREPENDS its parent `IedModel`'s own name to
`name` to form the LD's real wire name — feeding it `IedConnection_getLogicalDeviceList`'s
already-fully-qualified names (e.g. `"Reporter1LD1"`) against a model built via
`IedModel_create("Reporter1")` produced a corrupted double-prefixed name,
`"Reporter1Reporter1LD1"`. Fixed by building the internal model via `IedModel_create("")` —
sidesteps ever needing to know/derive the server's true IED name at all, since every LD name
discovery ever handles is already fully qualified straight from the wire. Relatedly:
`ReportControlBlock_create`'s `dataSetName` and `GSEControlBlock_create`'s `dataSet` parameters
both want the BARE local dataset name (confirmed against `ied_model_scl_loader.c`'s own usage,
which always passes SCL's raw `datSet="..."` attribute straight through) — passing
`getRCBValues`/`getGoCBValues`'s own fully-qualified reference there instead (as this loader
did before the E2E test caught it) produces an unresolvable double-qualified reference once
`IedModelUseCases_getReportSubscriptionTargets`/`_getGooseSubscriptionTargets` re-prepend their
own `lnRef$` on top of it a second time.

## Two libiec61850 dynamic-model gotchas found while building `ied_simulator`

**Two non-obvious libiec61850 dynamic-model gotchas hit while building the simulator**
(worth knowing before touching `sim_server.c` or writing another dynamic-model-based
server): (1) `DataSetEntry_create`'s variable reference is `"<lnName>$<fc>$<doName>$<daName>"`
with **no** LD-wire-name prefix (confirmed against `libiec61850/examples/server_example_dynamic/`);
including one makes the entry silently fail server-side resolution, which fails the whole
dataset's access check and `RptEna` with `DATA_ACCESS_ERROR_OBJECT_VALUE_INVALID`.
(2) `DataSet_create`'s `name` argument already gets `"<lnName>$"` prepended internally
(`dynamic_model.c`) - pass the bare local name (`"ds1"`), not `"LLN0$ds1"`, or it double-prefixes
and fails `IedModel_lookupDataSet`'s server-side match against a client-supplied `DatSet`
reference (`DATA_ACCESS_ERROR_TEMPORARILY_UNAVAILABLE`).

## `device_manager` connection-health monitor — added, then removed

A connection-health monitor (a reaper thread that auto-stopped a device on a genuine MMS
disconnect or GOOSE staleness transition, broadcasting a `DEVICE_STOPPED` message) was added to
`device_manager` at one point, then removed again at explicit user request — reverted back to
the simpler contract described in `CLAUDE.md` today (a device is torn down only by an explicit
`STOP_REPORTING` call; `mms_report_client`/`goose_subscriber`'s own retry-forever internal
supervisor loops just keep reconnecting with exponential backoff, exactly as they did before
that feature existed). `control_dispatcher`'s matching unsolicited `DEVICE_STOPPED` push
(`ControlDispatcher_notifyDeviceStopped`) was wired to this monitor and removed together with
it, for the same reason — a device only ever leaves the registry via an explicit
`STOP_REPORTING` request/response round trip, which every connected client already gets its own
direct ack for.

## Why each later feature was added (beyond the original five)

- `ied_discovery/` — a later, deliberate, user-requested addition (LAN auto-discovery of
  candidate IEDs), not a case of inventing scope unprompted.
- `ied_model_online_loader/` — also added later at explicit user request, after encountering a
  real device (e.g. OMICRON IED Scout's "Simulate IED" mode) that associates fine but never
  serves an SCL file over MMS file services at all.
- `scan_dispatcher/` — also added later at explicit user request, turning network scanning into
  a continuously-running background process with its own streamed-out websocket, managed by
  start/stop entry points ahead of the real external API layer that will eventually drive them.
- `control_dispatcher/` — also added later at explicit user request, turning reporting itself
  into a multi-device, callable-action service ("start reporting on device X", "stop reporting
  on device Y") instead of one fixed device per process run.

## `scan_orchestration` first-sweep race — discovered hosts silently lost right after `START_SCAN`

**Symptom, found while manually testing scanning against a fast/local responder (using
`ws_test_scan.html`, this repo's hand-rolled browser test client)**: a host that should have been
discovered on the very first sweep of a new scan sometimes never appeared on the
`scan_dispatcher` websocket at all — not delayed, genuinely never delivered — even though a
second, manually-triggered scan against the same host worked fine.

**Root cause**: `scan_dispatcher`'s websocket only binds on a scan's own 0→1 active-scan
transition, which happens synchronously inside `ScanOrchestration_startScan` — i.e. by the time
`control_dispatcher`'s `START_SCAN` ack reaches the client, the socket is already listening. But
the client can't have a connection open to it *before* that ack arrives (it doesn't know the
scan exists yet), so any well-behaved client reconnects only *after* receiving the ack. Both
`ipc_dispatcher` and `scan_dispatcher`'s ring buffers use "start-from-now" read cursors — a
newly-connected client sees only what's published after it connects, no backlog replay (see
"IPC / Reporting Out" above). `scan_orchestration`'s worker thread, meanwhile, was starting its
first subnet sweep immediately, with no coordination with that reconnect at all. Against a fast
or local responder, the worker could complete its first sweep and publish a genuinely-new host
before the client's post-ack reconnect had even finished its websocket handshake — the publish
landed on a ring buffer with no reader yet attached, and `ScanOrchestrationUseCases_isHostNew`'s
own seen-set dedup meant that same host would never be republished on any later sweep of the same
scan, since as far as the worker is concerned it's already been reported once. A slower responder
or a busier network masked the race by accident (the sweep just happened to take longer than the
client's reconnect), which is why it looked intermittent/timing-dependent rather than
deterministic in ad hoc manual testing.

**Fix**: `scan_orchestration_worker.c`'s `sweepLoop` now sleeps a fixed
`SCAN_ORCHESTRATION_INITIAL_SWEEP_GRACE_MS` (300ms) before running its very first sweep only —
every sweep after that runs back-to-back as before, uninterrupted by any further delay. 300ms is
comfortably above a loopback/LAN client's real reconnect time (typically single-digit
milliseconds) without meaningfully slowing down discovery — a scan's very first result arriving
~300ms later is not user-visible in practice, and every subsequent sweep on that scan is
unaffected. This closes the race structurally (the publish literally cannot happen before the
grace period elapses) rather than trying to detect or work around a client that hasn't reconnected
yet, which `scan_orchestration` has no visibility into (it doesn't know or care how many clients
are attached to `scan_dispatcher`, by design — see that feature's own "purely transport" bullet).
