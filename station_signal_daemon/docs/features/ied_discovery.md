# ied_discovery

## 1. Overview

`ied_discovery` finds candidate IEC 61850 MMS devices on the local network so an operator doesn't
have to already know a target IP. It runs a two-stage verification per candidate host: (1) a
cheap, bounded-concurrency TCP probe — reusing `scl_bootstrap`'s own async-connect sliding-window
state machine via `SclBootstrap_tcpProbeOnly` rather than reimplementing it — and (2) for TCP
survivors only, a real MMS/ACSE association (`IedConnection_connect`, immediately closed), never
file browsing or an SCL fetch. Only a host passing both stages counts as confirmed. It exists
because host discovery ("is anything speaking IEC 61850 MMS at this address") is a distinct
problem from SCL retrieval — a subnet scan needs a lightweight signal per candidate, not a full
file-service round trip.

This is host discovery on an **already-named local interface** (`getifaddrs()` + CIDR math), not
a substitute for SCL parsing — it never touches SCL or the data model at all, so it's a distinct
concern from CLAUDE.md's "No over-the-wire tree discovery" Hard Rule (see §5 for the precise
distinction). It is **not** part of `orchestration`'s own sequence — driven purely by
`scan_orchestration`'s worker, which wraps a single scan into a continuous, reference-counted
background service and republishes only genuinely new hosts over `scan_dispatcher`'s websocket.
Once a host is picked (by an operator, off that websocket feed), it's handed to the existing,
unmodified `scl_bootstrap`/`orchestration` pipeline as a plain host string, exactly like a
manually-typed host. Public boundary: `src/features/ied_discovery/service/ied_discovery_api.h`.

## 2. Public API surface

All declared in `src/features/ied_discovery/service/ied_discovery_api.h`.

- **`void IedDiscoveryConfig_defaults(IedDiscoveryConfig* config)`** — NULL-safe no-op if
  `config` is NULL. Fills: `tcpProbeTimeoutMs=500`, `maxConcurrentTcpProbes=64`,
  `mmsConnectTimeoutMs=3000`, `maxHosts=1024`, `acseAuthPassword=NULL`. No I/O, no allocation.

- **`IedDiscoveryHandle IedDiscovery_create(const IedDiscoveryConfig* config, IedDiscoveryError* outError)`**
  — allocates only, no I/O. `config == NULL` means "use `IedDiscoveryConfig_defaults`". Deep-copies
  `acseAuthPassword` into an internally owned buffer (`ownedAuthPassword`) via
  `IedDiscoveryUtils_safeStringDup`, then repoints `config.acseAuthPassword` at that owned copy so
  the handle stays valid independent of the caller's own buffer. Also creates a private
  `SclBootstrapHandle` delegate (see §3, `service/ied_discovery_api.c`) purely to reuse
  `SclBootstrap_tcpProbeOnly` — only `tcpProbeTimeoutMs`/`maxConcurrentTcpProbes` are copied into
  the delegate's config; the delegate's other fields (`mmsConnectTimeoutMs`, `maxBrowseDepth`,
  `acseAuthPassword`) are irrelevant and left at their own defaults since the delegate never calls
  `SclBootstrap_scanAndFetch`. Returns NULL and sets `IED_DISCOVERY_ERR_OUT_OF_MEMORY` on
  allocation failure (handle `calloc` or delegate creation).

- **`LinkedList IedDiscovery_scanSubnet(IedDiscoveryHandle handle, const char* interfaceId, int mmsPort, IedDiscoveryError* outError)`**
  — enumerates every host strictly between the network/broadcast address of `interfaceId`'s own
  IPv4 subnet (via `getifaddrs` + CIDR math), excluding the interface's own address, then verifies
  each candidate the same two-stage way as `IedDiscovery_verifyHost`. Blocking/synchronous.
  Returns a `LinkedList` of heap `char*` IPs, **confirmed devices only** (both stages passed) — a
  subnet with zero confirmed devices is a valid, non-NULL, empty list, not an error. Caller owns
  the list: `LinkedList_destroyDeep(list, free)`. Returns NULL and sets `*outError` for: NULL
  `handle`/`interfaceId`, empty `interfaceId`, `mmsPort <= 0`
  (`IED_DISCOVERY_ERR_INVALID_ARGUMENT`); interface missing/down/no-IPv4
  (`IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND`); or the subnet's host count exceeding
  `config.maxHosts` (`IED_DISCOVERY_ERR_SUBNET_TOO_LARGE`) — a safety valve against silently
  probing an accidentally huge range (e.g. a misidentified `/8`). Also NULL+`ERR_OUT_OF_MEMORY` on
  any allocation failure along the way (candidate-list build, confirmed-list create, or the
  `tcpProbeOnly` reachability array).

- **`IedDiscoveryHostStatus IedDiscovery_verifyHost(IedDiscoveryHandle handle, const char* host, int mmsPort, IedDiscoveryError* outError)`**
  — verifies one caller-supplied host/port identically to a scanned candidate (same two-stage TCP
  probe then MMS/ACSE association) — no special-cased trust for a manually-typed entry, so
  typos/wrong IPs are caught the same way a subnet-scan miss would be. Returns the specific
  `IedDiscoveryHostStatus` outcome (`IED_DISCOVERY_HOST_CONFIRMED` /
  `IED_DISCOVERY_HOST_NOT_TCP_REACHABLE` / `IED_DISCOVERY_HOST_NOT_MMS_DEVICE`) so a manual-add
  caller can show *why* a candidate was rejected. `*outError` is set only for argument-level
  failures (NULL `handle`/`host`, empty `host`, `mmsPort <= 0`) — a bad-argument call also returns
  `IED_DISCOVERY_HOST_NOT_TCP_REACHABLE` as its status value, so callers must check `*outError`
  first, not just branch on the status enum.

- **`void IedDiscovery_destroy(IedDiscoveryHandle handle)`** — NULL-safe. Destroys the
  `SclBootstrapHandle` delegate, frees `ownedAuthPassword`, frees the handle.

Config notes: `IedDiscoveryConfig.maxHosts` is the ceiling on a *scanned subnet's* host count
(`scanSubnet`-only concern — `verifyHost` has no such ceiling, it's always exactly one host).
`IedDiscoveryConfig.acseAuthPassword` is independent of `scl_bootstrap`'s/`mms_report_client`'s
own equivalent config field — features never share config structs across the public boundary,
even when the underlying mechanism (ACSE password auth, one retry on rejection) is identical.

## 3. Per-file breakdown

### `service/ied_discovery_api.h` / `ied_discovery_api.c`
The only public header. `ied_discovery_api.c` is orchestration glue over the domain/data layers,
no protocol logic of its own beyond the two-stage sequencing:
- `IedDiscoveryConfig_defaults` — see §2.
- `IedDiscovery_create` — see §2; builds the `SclBootstrapHandle` delegate.
- `verifyOneHost(handle, host, mmsPort)` (static) — the shared two-stage core both public
  functions funnel through. Wraps the single `host` in a throwaway one-element `LinkedList`
  (`LinkedList_destroyStatic` after, since `host` is caller-owned, never freed by this function),
  calls `SclBootstrap_tcpProbeOnly` on it. `reachable == NULL` (allocation failure inside the
  delegate) or `reachable[0] == false` both fall through to
  `IED_DISCOVERY_HOST_NOT_TCP_REACHABLE`. Only if the TCP probe succeeds does it call
  `IedDiscoveryMmsProbe_associate` for phase 2, mapping its bool result to
  `IED_DISCOVERY_HOST_CONFIRMED`/`IED_DISCOVERY_HOST_NOT_MMS_DEVICE`.
- `IedDiscovery_verifyHost` — validates arguments, then calls `verifyOneHost` directly (single
  host, no subnet enumeration).
- `IedDiscovery_scanSubnet` — validates arguments, resolves the interface via
  `IedDiscoveryNetif_getInterfaceIpv4`, checks `IedDiscoveryCidr_hostCount(netmask) >
  config.maxHosts` up front (fails fast before ever building the candidate list), then
  `IedDiscoveryCidr_buildCandidateList`. An empty candidate list (valid for `/31`/`/32`, or a
  subnet with only the interface's own address left after exclusion) short-circuits to an empty
  `confirmed` list immediately, skipping the TCP probe call entirely. Otherwise: one single
  `SclBootstrap_tcpProbeOnly` call covers **every** candidate at once (not one call per host — the
  whole point of reusing the bounded-concurrency batch API), then a second pass walks the
  candidate list in lockstep with the `reachable[]` array by index, calling
  `IedDiscoveryMmsProbe_associate` only for candidates where `reachable[index]` is true, appending
  a `IedDiscoveryUtils_safeStringDup`'d copy of each one that also passes phase 2 into `confirmed`.
  Note phase 2 here is still fully **sequential** — only phase 1 (the TCP probe) is batched/
  concurrent; each surviving candidate's MMS association happens one at a time, in candidate-list
  order.
- `IedDiscovery_destroy` — see §2.

### `domain/ied_discovery_types.h`
Pure data definitions, no behavior, no third-party includes beyond `scl_bootstrap_api.h` (needed
only for the `SclBootstrapHandle` delegate field type). Domain vocabulary is deliberately small —
this feature's only job is "which hosts on this subnet are really speaking IEC 61850 MMS."
- `IedDiscoveryError` — `OK` / `ERR_INVALID_ARGUMENT` / `ERR_OUT_OF_MEMORY` /
  `ERR_INTERFACE_NOT_FOUND` (named interface missing, down, or has no IPv4 address) /
  `ERR_SUBNET_TOO_LARGE` (host count > `config.maxHosts`).
- `IedDiscoveryHostStatus` — `HOST_CONFIRMED` (both stages passed) / `HOST_NOT_TCP_REACHABLE`
  (phase 1 never succeeded within `tcpProbeTimeoutMs`) / `HOST_NOT_MMS_DEVICE` (TCP reachable, but
  the real MMS/ACSE association failed or was rejected, including after an auth retry if
  configured — so whatever answered isn't a real IEC 61850 MMS device, or requires credentials
  this handle doesn't have).
- `IedDiscoveryConfig` — `tcpProbeTimeoutMs` (default 500, phase-1 per-candidate bound),
  `maxConcurrentTcpProbes` (default 64 — deliberately higher than `scl_bootstrap`'s own default of
  16, since a subnet scan can mean up to a few hundred candidates, not a handful of manually-typed
  hosts), `mmsConnectTimeoutMs` (default 3000, phase-2 association bound), `maxHosts` (default
  1024, `scanSubnet`'s safety-valve ceiling), `acseAuthPassword` (NULL = never retry with auth;
  copied internally; independent of every other feature's own equivalent field).
- `struct sIedDiscoveryHandle { IedDiscoveryConfig config; char* ownedAuthPassword;
  SclBootstrapHandle tcpProbeDelegate; }` — defined here, not hidden behind a further internal
  header (opacity is enforced by which header is exposed — `service/ied_discovery_api.h` only —
  matching `scl_bootstrap`/`ied_model`'s own `struct s*Handle` convention). `tcpProbeDelegate` is
  owned purely to call `SclBootstrap_tcpProbeOnly`, a legitimate cross-feature call through
  `scl_bootstrap`'s public service API, reusing its nontrivial async TCP-probe state machine
  rather than duplicating it.

### `domain/ied_discovery_cidr.h` / `ied_discovery_cidr.c`
Pure IPv4/CIDR arithmetic — no sockets, no `getifaddrs`, no third-party includes at all, fully
unit-testable without a real network interface. `address`/`netmask` are host-byte-order
`uint32_t` — the data layer (`ied_discovery_netif.c`) `ntohl`'s them from `getifaddrs`'
`sockaddr_in` before calling in here.
- `IedDiscoveryCidr_networkAddress(address, netmask)` — `address & netmask`.
- `IedDiscoveryCidr_broadcastAddress(address, netmask)` — `(address & netmask) | ~netmask`.
- `IedDiscoveryCidr_hostCount(netmask)` — `rangeSize = ~netmask` (e.g. `0xFF` for a `/24`);
  returns 0 if `rangeSize <= 1` (a `/31`, `rangeSize=1`, or `/32`, `rangeSize=0`, has no usable
  host strictly between network and broadcast), else `rangeSize - 1` (excludes the broadcast
  address; the network address is excluded by the caller loop's `host = network + 1` start, not
  by this subtraction itself — for a `/24`, `rangeSize=0xFF=255`, so this returns 254, matching
  network `.0` and broadcast `.255` both excluded).
- `formatDottedQuad(address)` (static) — `malloc(16)` (`"255.255.255.255"` + NUL),
  `snprintf`s the four octets shifted out of the big-endian-style packed `uint32_t`.
- `IedDiscoveryCidr_buildCandidateList(address, netmask, excludeAddress, maxHosts)` — re-checks
  `hostCount(netmask) > maxHosts` itself (redundant with `ied_discovery_api.c`'s own upfront check,
  but makes this function safe to call standalone/from tests without relying on the caller having
  already checked) and returns NULL if so. Computes `network`/`broadcast`, then loops
  `host` from `network + 1` to `broadcast - 1` inclusive (`for (host = network + 1; host <
  broadcast; host++)`), skipping `host == excludeAddress` (the interface's own address — no point
  probing self). Each surviving host is dotted-quad-formatted and appended, ascending numeric
  order (equivalently ascending dotted-quad order within one octet-4 range). Returns a valid,
  non-NULL, empty `LinkedList` for a netmask with zero usable hosts (`/31`, `/32`) — not an error;
  only the `maxHosts` ceiling or a `malloc`/`LinkedList_create` failure returns NULL (and on a
  mid-loop `formatDottedQuad` failure, everything built so far is torn down via
  `LinkedList_destroyDeep(candidates, free)` before returning NULL). Caller owns the returned
  list: `LinkedList_destroyDeep(list, free)`.

### `data/ied_discovery_auth.h` / `ied_discovery_auth.c`
Isolates ACSE authentication wiring behind one function — a deliberate duplicate of
`scl_bootstrap_auth.c`'s `SclBootstrapAuth_configurePasswordAuth` (itself already duplicated once
more, into `mms_report_client_auth.c`). Features never reach into another feature's data/domain
layers, only their public service header, and this snippet is small enough to match that
established precedent rather than warranting a new shared entry point.
- `IedDiscoveryAuth_configurePasswordAuth(IedConnection conn, const char* password)` — NULL-safe
  no-op if either arg is NULL. Pulls the `MmsConnection` out of `conn` via
  `IedConnection_getMmsConnection`, then its `IsoConnectionParameters` via
  `MmsConnection_getIsoConnectionParameters`; NULL-checks both and bails if either is NULL. Creates
  an `AcseAuthenticationParameter`, sets mechanism `ACSE_AUTH_PASSWORD` and the password (cast to
  `char*`, matching the vendored header's non-const signature), installs it via
  `IsoConnectionParameters_setAcseAuthenticationParameter`. Must be called before
  `IedConnection_connect()` — auth is negotiated at association time.

### `data/ied_discovery_mms_probe.h` / `ied_discovery_mms_probe.c`
Phase 2 of verification: a real MMS/ACSE association, immediately closed — no file browsing, no
SCL fetch (that heavier work is `scl_bootstrap`'s own job, deferred until a host is actually
picked; an accepted tradeoff that the picked host gets associated-with twice overall in exchange
for keeping the scan phase itself lightweight).
- `tryAssociateOnce(host, port, connectTimeoutMs, password, bool* outAccessDenied)` (static) — one
  connect attempt. Creates an `IedConnection`, configures password auth via
  `IedDiscoveryAuth_configurePasswordAuth` only if `password` is non-NULL, sets the connect
  timeout only if `connectTimeoutMs > 0`, calls `IedConnection_connect`. On failure, sets
  `*outAccessDenied = (err == IED_ERROR_ACCESS_DENIED || err == IED_ERROR_CONNECTION_REJECTED)`
  (the same heuristic `scl_bootstrap_mms_session.c`'s `runSequenceOnce` documents for
  ACSE-level-rejection detection), destroys the connection, returns false. On success, closes and
  destroys the connection, returns true.
- `IedDiscoveryMmsProbe_associate(host, port, connectTimeoutMs, ownedAuthPassword)` — tries once
  with `password=NULL`. If that succeeds, returns true immediately. If it failed **and**
  `accessDenied` was set **and** `ownedAuthPassword` is non-NULL, retries exactly once on a fresh
  `IedConnection` with the password configured, returning that attempt's result directly. Any
  other failure (not access-denied-shaped, or no password configured) returns false without a
  retry. Mirrors `scl_bootstrap`'s own one-retry-on-rejection policy — a real device that requires
  auth shouldn't be wrongly excluded from the discovered list just because discovery doesn't know
  yet whether a password applies.

### `data/ied_discovery_netif.h` / `ied_discovery_netif.c`
Interface IPv4 lookup via plain POSIX libc (`getifaddrs`/`freeifaddrs`, `<ifaddrs.h>`/`<net/if.h>`)
— not a new third-party dependency.
- `IedDiscoveryNetif_getInterfaceIpv4(interfaceId, outAddress, outNetmask)` — NULL-checks
  `interfaceId` (non-NULL, non-empty), `outAddress`, `outNetmask` up front. Calls `getifaddrs`;
  returns false on failure. Walks the linked `ifaddrs` list, skipping any entry with a NULL
  `ifa_addr`/`ifa_netmask`, a non-`AF_INET` family, a non-matching `ifa_name`, or missing
  `IFF_UP`. The first (and only) matching entry has its `sockaddr_in.sin_addr.s_addr` `ntohl`'d
  into `*outAddress`/`*outNetmask` (host byte order, matching `ied_discovery_cidr.c`'s expected
  input), `found = true`, loop breaks. Always `freeifaddrs(addrs)` before returning. Down,
  missing, and no-IPv4 interfaces are all collapsed into one `false` return — not distinguished
  further, since nothing downstream needs to tell these apart (the caller only ever surfaces
  `IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND` for any of them).

### `utils/ied_discovery_utils.h` / `ied_discovery_utils.c`
One small reusable string helper, shared by the data/service layers.
- `IedDiscoveryUtils_safeStringDup(s)` — NULL-safe `strdup`: `NULL` in, `NULL` out; otherwise
  `malloc(strlen(s)+1)` + `memcpy`. Caller owns the result (`free`). Used both for
  `IedDiscovery_create`'s owned password copy and for each confirmed host string added to
  `scanSubnet`'s result list.

## 4. Threading & concurrency model

- **No background thread of its own anywhere in this feature.** `IedDiscovery_scanSubnet` and
  `IedDiscovery_verifyHost` are fully synchronous and block the calling thread for the entire
  operation — same posture as `scl_bootstrap`'s own blocking entry points. Whatever thread the
  caller runs these on is blocked for the whole duration; `scan_orchestration`'s per-scan worker
  thread is responsible for putting `scanSubnet` on its own background thread and making it
  interruptible between sweeps (not mid-sweep — see §5).
- **Phase 1 (TCP probe) concurrency is entirely borrowed from `scl_bootstrap`.**
  `SclBootstrap_tcpProbeOnly` → `SclBootstrapTcpProbe_scan` achieves its "up to
  `maxConcurrentTcpProbes` in flight at once" behavior through non-blocking async-connect
  (`Socket_connectAsync`/`Socket_checkAsyncConnectState` from `hal_socket.h`) plus polling, driven
  entirely on the **single calling thread** — not OS threads, not `select`/`poll` on raw fds. A
  fixed-size sliding window of `ProbeSlot`s (each holding a `Socket`, a `hostIndex` into the host
  list or `-1` if empty, and a `startTimeMs`) is refilled as slots resolve: connected → reachable,
  failed or elapsed ≥ `timeoutMs` → unreachable, both free the slot for the next unstarted host. A
  10ms `Thread_sleep` between rounds when nothing resolved avoids busy-spinning. `ied_discovery`
  reuses this machinery unmodified via the public wrapper rather than re-implementing an
  async-connect state machine a second time — one call to `SclBootstrap_tcpProbeOnly` covers the
  entire candidate batch (not one call per host).
- **Phase 2 (MMS/ACSE association) is strictly sequential**, both in `verifyOneHost` (trivially,
  one host) and in `scanSubnet`'s second pass (one candidate at a time, in candidate-list/`index`
  order, walking `reachable[]` in lockstep). Each association is one fully connected/closed/
  destroyed `IedConnection` (up to two attempts, for the auth retry) before moving to the next
  candidate — no concurrency here at all, unlike phase 1.
- **No locks anywhere in this feature.** The handle is not shared/mutated concurrently by design —
  one blocking call in flight per handle at a time — and there's no shared mutable state between
  phase 1 and phase 2 beyond the plain `bool[]` reachability array passed by pointer down the call
  stack.
- `scan_orchestration`'s worker (this feature's only real caller — see §6) owns a private
  `IedDiscoveryHandle` per scan and calls `scanSubnet` in a sweep→diff→publish→interruptible-sleep
  loop on its own worker thread; this feature has zero awareness of that loop, scan IDs, or
  `scan_dispatcher`.

## 5. Known limitations / deliberate scope boundaries

- **This is network HOST discovery on an already-named local interface — not the "no
  over-the-wire tree discovery" Hard Rule.** That Hard Rule (CLAUDE.md) is about not walking a
  *connected device's own data-model tree* over MMS before SCL has been parsed — its one narrow,
  deliberate exception is `ied_model_online_loader`. `ied_discovery` has **zero** SCL/data-model
  involvement anywhere: phase 1 is a bare TCP connect, phase 2 is an MMS/ACSE association that is
  connected and then **immediately closed**, no directory browse, no file fetch, no ACSI
  model-discovery walk. It answers "is anything at this address speaking IEC 61850 MMS at all,"
  never "what does this device's data model look like." Once a host is confirmed, it's handed off
  as a plain host string to the entirely separate, unmodified `scl_bootstrap`/`orchestration`
  pipeline — the same pipeline a manually-typed host would go through, which is where SCL
  parsing/the Hard Rule's actual concern lives.
  - Concretely: the Hard Rule question is "does this code learn about a device's LDs/LNs/DOs/DAs
    by querying the live device instead of parsing SCL?" `ied_discovery` never asks that question
    at all — it doesn't request directory listings, doesn't request LN/DO/DA structure, doesn't
    build an `IedModel`. It only asks "does an MMS association succeed."
- **No `_start`/`_stop` pair on this feature itself** — both public entry points are one blocking
  call each; there's never a background operation in flight for a `_stop()` to tear down. Any
  interruptibility (e.g. mid-scan cancellation) is `scan_orchestration`'s responsibility layered
  on top, not something this feature provides.
- **Phase 2 is fully sequential, never batched** — only phase 1 benefits from bounded concurrency.
  A subnet with many TCP-reachable-but-slow-to-associate hosts pays that cost one host at a time.
- **`IedDiscovery_scanSubnet` cannot be interrupted mid-sweep** — it's one blocking call; whatever
  caller wraps it (`scan_orchestration`'s worker) can only decline to start the *next* sweep, not
  abort one already in progress.
- **`maxHosts` is a hard ceiling, not a suggestion** — a subnet larger than `config.maxHosts`
  (default 1024) fails the whole `scanSubnet` call with `IED_DISCOVERY_ERR_SUBNET_TOO_LARGE`
  rather than probing a truncated subset; there's no partial-scan mode.
- **ACSE password auth is the only auth mechanism supported** (`ACSE_AUTH_PASSWORD`), same as
  `scl_bootstrap`/`mms_report_client` — no certificate-based or other ACSE mechanisms.
- **A confirmed host can still fail to onboard later** — `scanSubnet`/`verifyHost` only prove an
  MMS/ACSE association succeeds at the moment of the scan; they say nothing about whether the
  device actually serves an SCL file (`scl_bootstrap`'s job) or has connectivity/auth stable
  enough to survive a real `orchestration` run afterward.
- **IPv4 only** — `ied_discovery_netif.c` only matches `AF_INET` entries; no IPv6 subnet
  enumeration.
- **Interface-down/missing/no-IPv4 are collapsed into one error** — `getInterfaceIpv4` doesn't
  distinguish "interface doesn't exist" from "interface exists but is down" from "interface is up
  but has no IPv4 address"; all three surface as `IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND`.

## 6. Cross-feature dependencies

**Calls into:**
- `scl_bootstrap`'s public service API (`src/features/scl_bootstrap/service/scl_bootstrap_api.h`)
  — specifically `SclBootstrap_create`/`SclBootstrap_destroy` (to own a delegate handle) and
  `SclBootstrap_tcpProbeOnly` (to reuse its bounded-concurrency async TCP-probe state machine for
  phase 1). This is the **only** cross-feature dependency `ied_discovery` has. It never calls
  `SclBootstrap_scanAndFetch` — its own phase 2 is a lighter, purpose-built MMS/ACSE-association-
  only check (`IedDiscoveryMmsProbe_associate`), not a full browse/fetch.
- libiec61850's `IedConnection`/`iec61850_client.h` API directly for phase 2 (connect, close,
  destroy) — no wrapping feature between this and the library, same posture as
  `scl_bootstrap`/`mms_report_client`.
- `hal_socket.h`-family async-connect primitives, but only transitively through
  `SclBootstrap_tcpProbeOnly` — `ied_discovery` itself has no direct `Socket_*` calls.
- Plain POSIX `getifaddrs`/`freeifaddrs` (`<ifaddrs.h>`) for interface lookup — not a third-party
  dependency.
- Nothing from `ied_model`, `mms_report_client`, or `goose_subscriber` — zero `IedModelHandle`
  dependency anywhere; doesn't parse SCL, doesn't know about RCBs/GoCBs (see §5).

**Called by:**
- `src/scan_orchestration/`'s own worker, exclusively — each active scan owns a private
  `IedDiscoveryHandle` and its own seen-set, looping sweep (`IedDiscovery_scanSubnet`) → diff
  against the seen-set → publish only genuinely new hosts over `scan_dispatcher`'s websocket →
  interruptible sleep → repeat. This is `ied_discovery`'s **only** real caller in the daemon.
- **Not** called by `src/orchestration/` at all — orchestration's own sequence starts directly
  from a caller-supplied host (`scl_bootstrap` first stage), with no discovery step of its own.
  `ied_discovery` and `orchestration` are siblings under the daemon, not a pipeline stage of each
  other; a discovered host only re-enters the picture as a plain string argument to
  `control_dispatcher`'s `START_REPORTING` command, chosen by whatever's consuming
  `scan_dispatcher`'s output.
- The old in-process interactive terminal-prompt adapter (`main_discovery_prompt.c`) that used to
  call this feature directly from `main.c` has been removed — `control_dispatcher`'s
  `START_SCAN`/`STOP_SCAN` commands are the daemon's only scan-driving interface now, and
  `scan_orchestration` is the only thing between them and this feature.

**Hard Rules this feature must never violate** (from CLAUDE.md, applies repo-wide but concretely
constrains this feature):
- Never hand-roll MMS parsing — protocol handling goes through libiec61850 (`IedConnection`)
  exclusively, which this feature already does for phase 2.
- Never touch `third_party/` — pre-built/vendored; nothing in this feature edits vendored headers.
- Never add a silent, automatic tree-discovery path — this feature's phase 2 is deliberately
  capped at "association succeeds/fails," never escalating to a directory browse or model walk
  (see §5 for the detailed Hard-Rule distinction).

## 7. Tests

**`tests/ied_discovery/`** (Unity unit tests, hermetic aside from one deliberate real-`getifaddrs`
case — wired into `tests/Makefile`'s explicit `TESTS` list):

- `test_ied_discovery_cidr.c` — pure CIDR math, no network I/O at all:
  `test_networkAddress_masksToSubnetFloor`, `test_broadcastAddress_isSubnetCeiling` (both against
  a `/24`), `test_hostCount_slash24_is254`, `test_hostCount_slash30_is2`,
  `test_hostCount_slash31_and_slash32_areZero` (the zero-usable-host edge cases), and
  `buildCandidateList` coverage: `test_buildCandidateList_slash24_excludesNetworkBroadcastAndOwnAddress`
  (254 usable hosts minus the excluded own-address = 253, network/broadcast/own-address all
  absent, `.1`/`.254` both present), `test_buildCandidateList_slash30_hasTwoHosts` (exactly `.9`/
  `.10` for a `.8/30`), `test_buildCandidateList_slash31_isValidEmptyList` (confirms the
  "valid, non-NULL, empty list" contract, not an error), and
  `test_buildCandidateList_returnsNull_whenHostCountExceedsMaxHosts` (a `/24`'s 254 hosts against a
  `maxHosts=100` ceiling returns NULL).
- `test_ied_discovery_netif.c` — real `getifaddrs()` against **loopback only**:
  `test_getInterfaceIpv4_succeeds_forLoopback` asserts `"lo"` resolves to `0x7F000001`
  (127.0.0.1) — chosen specifically because `"lo"` is guaranteed present with an IPv4 address in
  any environment this test runs in, including sandboxed CI containers with no other interface,
  unlike a real LAN interface which wouldn't be safe to assert on unconditionally.
  `test_getInterfaceIpv4_fails_forBogusInterfaceName` and
  `test_getInterfaceIpv4_fails_onNullArguments` (all three NULL-arg combinations) round out
  negative-path coverage.
- `test_ied_discovery_api.c` — argument-validation and config-defaults wiring, plus **one**
  deterministic real-`getifaddrs` assertion that needs no network probe at all: mirrors
  `test_goose_subscriber_api`/`test_mms_report_client_api`'s "wiring only" spirit.
  `test_configDefaults_setsSaneValues` checks all five default values. `test_create_succeeds_withNullConfig`/
  `test_destroy_doesNotCrash_onNullHandle` cover handle lifecycle. `test_verifyHost_rejectsNullHandle`/
  `test_verifyHost_rejectsNullOrEmptyHost`/`test_verifyHost_rejectsNonPositivePort` and
  `test_scanSubnet_rejectsNullOrEmptyInterfaceId` cover argument validation without ever touching
  the network. `test_scanSubnet_errorInterfaceNotFound_forBogusInterface` proves the
  `IED_DISCOVERY_ERR_INTERFACE_NOT_FOUND` path. **The one deterministic ceiling assertion**:
  `test_scanSubnet_errorSubnetTooLarge_forLoopbackAtDefaultCeiling` scans `"lo"` at the default
  `maxHosts=1024` — loopback's real netmask (typically `/8`, ~16M hosts) far exceeds that ceiling,
  so this fails fast at the `maxHosts` check *before* any TCP probe ever happens, proving the
  safety valve deterministically without depending on any network topology beyond `"lo"` always
  existing.

**`integration_tests/ied_discovery/`** (`e2e_test_ied_discovery.c`, real `ied_simulator`
"Reporter1" IED in-process, real loopback TCP probe + real MMS/ACSE association via
`IedDiscovery_verifyHost` — no `sudo` needed, plain TCP/MMS only, same posture as
`integration_tests/scl_bootstrap/`):

- `test_verifyHost_confirmsRealDevice` — a live `SimServer` on `TEST_PORT` (10401, 200ms startup
  grace) yields `IED_DISCOVERY_HOST_CONFIRMED`.
- `test_verifyHost_notTcpReachable_whenNothingListening` — probing a genuinely dead port
  (`DEAD_PORT` 10499, nothing bound) yields `IED_DISCOVERY_HOST_NOT_TCP_REACHABLE`.
- `test_verifyHost_authRequiredNoPasswordConfigured_notMmsDevice` — server requires auth
  (`SimServer_requireAuthentication(sim, "secret123")`), client handle has no `acseAuthPassword`
  configured; yields `IED_DISCOVERY_HOST_NOT_MMS_DEVICE`.
- `test_verifyHost_authRequiredCorrectPassword_confirmed` — same auth-required server, client
  configured with the correct password; yields `IED_DISCOVERY_HOST_CONFIRMED`, proving the
  unauthenticated-first-attempt-then-retry sequence actually round-trips end to end.
- `test_verifyHost_authRequiredWrongPassword_notMmsDevice` — same server, wrong password
  configured; yields `IED_DISCOVERY_HOST_NOT_MMS_DEVICE` (retry attempted and also failed).

Together these five E2E cases prove the full `IedDiscoveryHostStatus` outcome space plus the
complete password-auth-retry symmetry (no-password / correct-password / wrong-password), against
a real device.

**Explicit gap: no real subnet scan anywhere in this test suite.** The E2E suite's own file
comment states this deliberately — a real LAN subnet scan (`IedDiscovery_scanSubnet` end to end,
including a real multi-candidate `getifaddrs` sweep) is machine-topology-dependent and not
something a hermetic, sandboxed test environment can assert on; there's no second real device on
a routable subnet to discover in CI. `test_ied_discovery_api.c`'s loopback-`maxHosts`-ceiling case
(above) is the one deterministic `getifaddrs`-touching assertion that substitutes for it. Per
CLAUDE.md, verifying a real sweep is a manual step: run the daemon and issue `START_SCAN` against
a machine with a real neighbor IED on the wire.
