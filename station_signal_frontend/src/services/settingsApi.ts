import { apiClient } from './apiClient'
import type { NetworkConfig, NetworkPendingChange, NetworkStatus } from '@/types/api'

export function getNetworkStatus(): Promise<NetworkStatus> {
  return apiClient.get<NetworkStatus>('/settings/network')
}

export function applyNetworkConfig(config: NetworkConfig): Promise<NetworkPendingChange> {
  return apiClient.post<NetworkPendingChange>('/settings/network', config)
}

// Undoes a provisionally-applied change without waiting out the box's own auto-revert, and
// clears the pending state that would otherwise refuse every later apply. This is the recovery
// path for a change that never came up — including one whose auto-revert itself failed, which is
// otherwise only clearable with shell access to the box.
export function revertNetworkConfig(): Promise<void> {
  return apiClient.post<void>('/settings/network/revert')
}

// isReachableAt/confirmNetworkConfigAt deliberately bypass apiClient (which always targets this
// page's own origin, via API_BASE_URL) — after submitting a new IP, the box we need to reach is
// no longer at this page's origin, it's at the address we just applied. These are used only by
// stores/settings.ts's post-apply reconnect flow.

// Why a probe outcome and not a boolean: a rejected cross-origin fetch tells JavaScript nothing
// about *why* it failed — the browser deliberately withholds that detail. A box that isn't
// answering, one that is answering but rejecting the origin, and one whose reverse proxy 502s all
// collapsed into `false` here, and the reconnect loop then retried forever showing the same
// message for all three. That is why an IP change that visibly worked could still never confirm.
export type ProbeOutcome =
  // Answered 2xx and allowed this origin to read it — the box is up and confirmable.
  | 'ok'
  // Answered, but with a non-2xx status (e.g. nginx 502 because the API behind it is down).
  | 'http-error'
  // Something answered — the opaque no-cors probe below succeeded — but the CORS-mode read was
  // rejected. The box is up; the *confirm* call is what's broken.
  | 'blocked-by-cors'
  // Nothing answered at all: connection refused/reset, DNS failure.
  | 'unreachable'
  // Took longer than PROBE_TIMEOUT_MS. Distinct from 'unreachable' on purpose: a socket that
  // hangs rather than being refused means something accepted the connection and then went quiet,
  // which is what happens to a probe opened during the interface's address swap.
  | 'timeout'

export interface ProbeResult {
  outcome: ProbeOutcome
  status?: number
  error?: string
  durationMs: number
}

// A probe opened in the instant the box swaps its address can hang until the *browser's* own
// connection timeout — ~90s in Firefox. Since the whole confirmation window is only 90s, one such
// probe swallowed it entirely and the reconnect loop got exactly one attempt. A liveness probe
// must never be able to consume its own deadline.
const PROBE_TIMEOUT_MS = 2500

function isTimeout(err: unknown): boolean {
  return err instanceof DOMException && (err.name === 'TimeoutError' || err.name === 'AbortError')
}

export async function probeReachability(baseUrl: string): Promise<ProbeResult> {
  const startedAt = Date.now()
  const elapsed = () => Date.now() - startedAt
  // cache:'no-store' so a failed attempt can't be served back from cache on the next tick.
  const init = (): RequestInit => ({ signal: AbortSignal.timeout(PROBE_TIMEOUT_MS), cache: 'no-store' })

  try {
    const res = await fetch(`${baseUrl}/health`, init())
    return { outcome: res.ok ? 'ok' : 'http-error', status: res.status, durationMs: elapsed() }
  } catch (err) {
    const reason = err instanceof Error ? `${err.name}: ${err.message}` : String(err)

    // A timeout has already answered the only question the re-probe exists to settle — nothing
    // usable responded in time — so don't pay for it twice and double the worst case.
    if (isTimeout(err)) {
      return { outcome: 'timeout', error: reason, durationMs: elapsed() }
    }

    // Fast failure: "box is down" and "box is up but refused this origin" are genuinely
    // indistinguishable here, because the browser withholds cross-origin failure detail. An
    // opaque no-cors response still resolves whenever the server answered at all, and still
    // rejects when nothing is listening — the one way to tell them apart from inside a browser.
    try {
      await fetch(`${baseUrl}/health`, { ...init(), mode: 'no-cors' })
      return { outcome: 'blocked-by-cors', error: reason, durationMs: elapsed() }
    } catch (reprobeErr) {
      if (isTimeout(reprobeErr)) {
        return { outcome: 'timeout', error: reason, durationMs: elapsed() }
      }
      return { outcome: 'unreachable', error: reason, durationMs: elapsed() }
    }
  }
}

export async function confirmNetworkConfigAt(baseUrl: string): Promise<void> {
  const res = await fetch(`${baseUrl}/settings/network/confirm`, { method: 'POST' })
  if (!res.ok) {
    throw new Error(`confirm failed with status ${res.status}`)
  }
}
