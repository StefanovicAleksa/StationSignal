import { ref } from 'vue'
import { defineStore } from 'pinia'

import { getNetworkStatus, applyNetworkConfig, isReachableAt, confirmNetworkConfigAt } from '@/services/settingsApi'
import { ApiError } from '@/types/api'
import type { NetworkConfig, NetworkStatus } from '@/types/api'

export type SettingsPhase = 'idle' | 'applying' | 'waitingForReconnect' | 'confirming' | 'confirmed' | 'reverted' | 'error'

export interface SettingsError {
  code: string
  message: string
}

const POLL_BASE_DELAY_MS = 1000
const POLL_MAX_DELAY_MS = 5000
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

  let pollAttempt = 0
  let pollTimer: ReturnType<typeof setTimeout> | null = null

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
      pendingExpiresAt.value = new Date(pending.expiresAt).getTime()
      newOrigin.value = computeOrigin(pending.new.cidr)
      phase.value = 'waitingForReconnect'
      pollAttempt = 0
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
      return
    }
    pollAttempt += 1
    const delay = Math.min(POLL_BASE_DELAY_MS * 1.5 ** (pollAttempt - 1), POLL_MAX_DELAY_MS)
    const jitter = delay * (0.8 + Math.random() * 0.4)
    nextPollAt.value = Date.now() + jitter
    pollTimer = setTimeout(() => void attemptPoll(), jitter)
  }

  async function attemptPoll() {
    if (!newOrigin.value || phase.value !== 'waitingForReconnect') return
    const reachable = await isReachableAt(newOrigin.value)
    if (phase.value !== 'waitingForReconnect') return
    if (reachable) {
      await doConfirm()
    } else {
      schedulePoll()
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

  // Manual "check now" escape hatch, mirroring stores/scan.ts's retryNow.
  async function pollNow() {
    if (phase.value !== 'waitingForReconnect') return
    clearPollTimer()
    await attemptPoll()
  }

  function reset() {
    clearPollTimer()
    phase.value = 'idle'
    applyError.value = null
    pendingExpiresAt.value = null
    newOrigin.value = null
    pollAttempt = 0
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
    loadStatus,
    submit,
    pollNow,
    reset,
    dispose,
  }
})

function toSettingsError(err: unknown): SettingsError {
  if (err instanceof ApiError) {
    return { code: err.code, message: err.message }
  }
  return { code: 'UNKNOWN', message: err instanceof Error ? err.message : 'Unexpected error' }
}
