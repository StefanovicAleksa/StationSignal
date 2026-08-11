import { computed, ref } from 'vue'
import { defineStore } from 'pinia'

import {
  getNetworkStatus,
  applyNetworkConfig,
  probeReachability,
  confirmNetworkConfigAt,
  revertNetworkConfig,
} from '@/services/settingsApi'
import { ApiError } from '@/types/api'
import type { NetworkConfig, NetworkStatus } from '@/types/api'
import { logger } from '@/utils/logger'
import type { ProbeOutcome } from '@/services/settingsApi'
import { t } from '@/i18n'

// One reconnect attempt, kept so the page can show *why* it is still waiting. Without this the
// only visible signal was a countdown, which made "polling and being refused" indistinguishable
// from "not polling at all".
export interface PollAttempt {
  at: number
  origin: string
  outcome: ProbeOutcome
  status?: number
  error?: string
  durationMs: number
}

const MAX_POLL_LOG = 12

export type SettingsPhase =
  | 'idle'
  | 'applying'
  | 'waitingForReconnect'
  | 'confirming'
  | 'confirmed'
  | 'reverted'
  | 'reverting'
  | 'error'

export interface SettingsError {
  code: string
  message: string
}

// The box's address swap completes in ~1.2s (systemd timer + `nmcli device reapply`), so the
// first probe waits past that rather than opening a socket in the middle of it. Probes are
// bounded and overlap-safe now, so the backoff stays tight — the aim is to confirm within a few
// seconds of the box coming up, not to be gentle with a server that is one HTTP GET away.
const POLL_BASE_DELAY_MS = 1500
const POLL_MAX_DELAY_MS = 3000
// How much slack past the server's own advertised expiresAt to allow before giving up and
// assuming the OS-level auto-revert (see deploy/scripts/station-signal-netconfig.sh) already
// fired — polling a dead address forever would just spin.
const REVERT_GRACE_MS = 5000

export const useSettingsStore = defineStore('settings', () => {
  const status = ref<NetworkStatus | null>(null)
  const statusError = ref<SettingsError | null>(null)
  const loadingStatus = ref(false)

  const phase = ref<SettingsPhase>('idle')
  const applyError = ref<SettingsError | null>(null)
  const pendingExpiresAt = ref<number | null>(null)
  const newOrigin = ref<string | null>(null)
  const nextPollAt = ref<number | null>(null)
  const pollLog = ref<PollAttempt[]>([])

  let pollAttempt = 0
  let pollTimer: ReturnType<typeof setTimeout> | null = null
  let confirmClaimed = false

  // A change the box is still holding open, either because it's genuinely in flight or because
  // an earlier one got stuck. Until it's cleared, every apply is refused — so whenever this is
  // true the UI has to offer the way out rather than just reporting the refusal.
  const stuckPending = computed(
    () => Boolean(status.value?.pending) || applyError.value?.code === 'CHANGE_ALREADY_PENDING',
  )

  function clearPollTimer() {
    if (pollTimer) {
      clearTimeout(pollTimer)
      pollTimer = null
    }
    nextPollAt.value = null
  }

  async function loadStatus() {
    loadingStatus.value = true
    statusError.value = null
    try {
      status.value = await getNetworkStatus()
    } catch (err) {
      statusError.value = toSettingsError(err)
    } finally {
      loadingStatus.value = false
    }
  }

  // The box is reachable at the same protocol/port, just a different host — nginx (production)
  // and the dev API server alike listen the same way regardless of which address answers.
  function computeOrigin(cidr: string): string {
    const ip = cidr.split('/')[0]
    const { protocol, port } = window.location
    return port ? `${protocol}//${ip}:${port}` : `${protocol}//${ip}`
  }

  async function submit(config: NetworkConfig) {
    phase.value = 'applying'
    applyError.value = null
    try {
      const pending = await applyNetworkConfig(config)
      // Anchored to this browser's own clock, not the box's: remainingSeconds is a relative
      // duration, so it stays correct even if the box's clock is wrong (e.g. no NTP reachable
      // over a bare direct-Ethernet link with no upstream internet) — comparing the box's
      // absolute expiresAt against our Date.now() doesn't survive that.
      pendingExpiresAt.value = Date.now() + pending.remainingSeconds * 1000
      newOrigin.value = computeOrigin(pending.new.cidr)
      phase.value = 'waitingForReconnect'
      pollAttempt = 0
      confirmClaimed = false
      pollLog.value = []
      schedulePoll()
    } catch (err) {
      applyError.value = toSettingsError(err)
      phase.value = 'error'
    }
  }

  function schedulePoll() {
    if (!newOrigin.value) return
    if (pendingExpiresAt.value !== null && Date.now() > pendingExpiresAt.value + REVERT_GRACE_MS) {
      phase.value = 'reverted'
      clearPollTimer()
      // Re-read the box's own state rather than assuming the auto-revert did its job: it runs
      // nmcli too, and it can fail exactly the same way the apply did. If it left the change
      // pending, stuckPending goes true and the UI offers to clear it.
      void loadStatus()
      return
    }
    pollAttempt += 1
    const delay = Math.min(POLL_BASE_DELAY_MS * 1.5 ** (pollAttempt - 1), POLL_MAX_DELAY_MS)
    const jitter = delay * (0.8 + Math.random() * 0.4)
    nextPollAt.value = Date.now() + jitter
    // The next tick is queued when this one *fires*, not when its probe resolves. Chaining off
    // the probe meant a single hung fetch stalled the loop completely — one attempt, ~100s, the
    // whole confirmation window gone. The cadence must not depend on how long a probe takes.
    pollTimer = setTimeout(() => {
      void attemptPoll()
      schedulePoll()
    }, jitter)
  }

  async function attemptPoll() {
    if (!newOrigin.value || phase.value !== 'waitingForReconnect') return
    const origin = newOrigin.value
    const result = await probeReachability(origin)

    const attempt: PollAttempt = { at: Date.now(), origin, ...result }
    pollLog.value = [...pollLog.value, attempt].slice(-MAX_POLL_LOG)
    // Mirrored to the console so a failing run can be copied out of devtools wholesale. Debug,
    // so a prod build stays quiet — pollLog is the real record either way, and the Settings page
    // renders it.
    logger.debug('[station-signal] reconnect probe', attempt)

    if (phase.value !== 'waitingForReconnect') return

    // 'blocked-by-cors' counts as reachable on purpose: something answered at the new address, so
    // the box came up. Proceeding to confirm turns an infinite silent retry into one concrete,
    // reportable error — the previous code could never get past this point to find that out.
    if (result.outcome === 'ok' || result.outcome === 'blocked-by-cors') {
      // Probes now overlap, so two can come back reachable before either flips the phase. Claim
      // the transition synchronously — doConfirm is not idempotent, it POSTs.
      if (confirmClaimed) return
      confirmClaimed = true
      clearPollTimer()
      await doConfirm()
    }
  }

  async function doConfirm() {
    if (!newOrigin.value) return
    phase.value = 'confirming'
    try {
      await confirmNetworkConfigAt(newOrigin.value)
      phase.value = 'confirmed'
      window.location.href = newOrigin.value
    } catch (err) {
      applyError.value = toSettingsError(err)
      phase.value = 'error'
    }
  }

  // Clears a pending change the box is holding open, putting it back on its previous address.
  // Reachable whether or not this browser is the one that started the change — a marker left by
  // a failed auto-revert (or one that outlived an API restart) blocks everybody equally.
  async function revert() {
    clearPollTimer()
    phase.value = 'reverting'
    applyError.value = null
    try {
      await revertNetworkConfig()
      phase.value = 'idle'
      pendingExpiresAt.value = null
      newOrigin.value = null
    } catch (err) {
      applyError.value = toSettingsError(err)
      phase.value = 'error'
    }
    // Either way, show what the box actually reports now rather than what we hoped for.
    await loadStatus()
  }

  // Manual "check now" escape hatch, mirroring stores/scan.ts's retryNow.
  async function pollNow() {
    if (phase.value !== 'waitingForReconnect') return
    clearPollTimer()
    await attemptPoll()
    // Re-arm: the loop now reschedules itself from the timer callback, so cancelling that timer
    // above would otherwise leave the reconnect loop dead if this probe didn't confirm.
    if (phase.value === 'waitingForReconnect') {
      schedulePoll()
    }
  }

  function reset() {
    clearPollTimer()
    phase.value = 'idle'
    applyError.value = null
    pendingExpiresAt.value = null
    newOrigin.value = null
    pollAttempt = 0
    confirmClaimed = false
  }

  function dispose() {
    clearPollTimer()
  }

  return {
    status,
    statusError,
    loadingStatus,
    phase,
    applyError,
    pendingExpiresAt,
    newOrigin,
    nextPollAt,
    pollLog,
    stuckPending,
    loadStatus,
    submit,
    revert,
    pollNow,
    reset,
    dispose,
  }
})

function toSettingsError(err: unknown): SettingsError {
  if (err instanceof ApiError) {
    return { code: err.code, message: err.message }
  }
  return { code: 'UNKNOWN', message: err instanceof Error ? err.message : t('common.unexpectedError') }
}
