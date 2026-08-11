import { ref } from 'vue'
import { defineStore } from 'pinia'

import { startScan, stopScan, listScans } from '@/services/scanApi'
import { createScanSocket, type ScanSocket } from '@/services/scanSocket'
import { ApiError, type ScanResultMessage } from '@/types/api'
import { t } from '@/i18n'

export type ScanPhase = 'starting' | 'active' | 'stopping' | 'error'

export interface ScanError {
  code: string
  message: string
  retryable: boolean
}

export interface ScanSession {
  key: string
  scanId: number | null
  interfaceId: string
  mmsPort: number
  sweepIntervalMs?: number
  phase: ScanPhase
  results: ScanResultMessage[]
  error: ScanError | null
  retryAttempt: number
  nextRetryAt: number | null
  connectionInterrupted: boolean
}

const BASE_RETRY_DELAY_MS = 1000
const MAX_RETRY_DELAY_MS = 10000

export const useScanStore = defineStore('scan', () => {
  const sessions = ref<Record<string, ScanSession>>({})
  const mountError = ref<ScanError | null>(null)
  const mountRetryAttempt = ref(0)
  const mountNextRetryAt = ref<number | null>(null)

  let sessionSeq = 0
  let mountEpoch = 0
  let socket: ScanSocket | null = null
  const retryTimers = new Map<string, ReturnType<typeof setTimeout>>()
  const pendingRetries = new Map<string, () => void>()
  let mountRetryTimer: ReturnType<typeof setTimeout> | null = null
  let pendingMountRetry: (() => void) | null = null

  function ensureSocketConnected() {
    if (socket) return
    socket = createScanSocket({
      onMessage: (message) => {
        const session = Object.values(sessions.value).find((s) => s.scanId === message.scanId)
        session?.results.push(message)
      },
      onDisconnected: (reason) => {
        if (reason === 'intentional') return
        for (const session of Object.values(sessions.value)) {
          if (session.phase === 'active' || session.phase === 'stopping') {
            session.connectionInterrupted = true
          }
        }
        if (reason === 'exhausted') {
          socket = null
          ensureSocketConnected()
        }
      },
    })
    socket.connect()
  }

  function clearRetryTimer(key: string) {
    const timer = retryTimers.get(key)
    if (timer) {
      clearTimeout(timer)
      retryTimers.delete(key)
    }
    pendingRetries.delete(key)
    const session = sessions.value[key]
    if (session) session.nextRetryAt = null
  }

  function scheduleRetry(key: string, action: () => void) {
    const session = sessions.value[key]
    if (!session) return
    session.retryAttempt += 1
    const delay = Math.min(BASE_RETRY_DELAY_MS * 2 ** (session.retryAttempt - 1), MAX_RETRY_DELAY_MS)
    const jitter = delay * (0.8 + Math.random() * 0.4)
    session.nextRetryAt = Date.now() + jitter
    pendingRetries.set(key, action)
    retryTimers.set(key, setTimeout(action, jitter))
  }

  function retryNow(key: string) {
    const timer = retryTimers.get(key)
    if (timer) {
      clearTimeout(timer)
      retryTimers.delete(key)
    }
    pendingRetries.get(key)?.()
  }

  function nextKey(): string {
    return `session-${sessionSeq++}`
  }

  async function start(interfaceId: string, mmsPort: number, sweepIntervalMs?: number): Promise<string> {
    const key = nextKey()
    sessions.value[key] = {
      key,
      scanId: null,
      interfaceId,
      mmsPort,
      sweepIntervalMs,
      phase: 'starting',
      results: [],
      error: null,
      retryAttempt: 0,
      nextRetryAt: null,
      connectionInterrupted: false,
    }
    await attemptStart(key)
    return key
  }

  async function attemptStart(key: string) {
    const session = sessions.value[key]
    if (!session) return
    session.phase = 'starting'
    session.error = null
    // Connect before issuing the request, not after it resolves — the daemon's first scan
    // sweep can fire within ~300ms of the ack with no backlog replay, so a socket opened only
    // after the REST round-trip completes routinely misses it entirely.
    ensureSocketConnected()

    try {
      const response = await startScan({
        interfaceId: session.interfaceId,
        mmsPort: session.mmsPort,
        sweepIntervalMs: session.sweepIntervalMs,
      })
      const current = sessions.value[key]
      if (!current) return
      current.scanId = response.scanId
      current.phase = 'active'
      current.retryAttempt = 0
    } catch (err) {
      const current = sessions.value[key]
      if (!current) return
      const scanError = toScanError(err)
      current.error = scanError
      current.phase = 'error'
      if (scanError.retryable) {
        scheduleRetry(key, () => attemptStart(key))
      }
    }
  }

  async function stop(key: string) {
    const session = sessions.value[key]
    if (!session || session.scanId === null) return
    clearRetryTimer(key)
    session.phase = 'stopping'
    session.error = null
    await attemptStop(key)
  }

  async function attemptStop(key: string) {
    const session = sessions.value[key]
    if (!session || session.scanId === null) return

    try {
      await stopScan(session.scanId)
      delete sessions.value[key]
    } catch (err) {
      const current = sessions.value[key]
      if (!current) return

      if (err instanceof ApiError && err.code === 'SCAN_NOT_FOUND') {
        delete sessions.value[key]
        return
      }

      const scanError = toScanError(err)
      current.error = scanError

      if (scanError.retryable) {
        scheduleRetry(key, () => attemptStop(key))
      } else {
        current.phase = 'active'
      }
    }
  }

  function clearMountRetryTimer() {
    if (mountRetryTimer) {
      clearTimeout(mountRetryTimer)
      mountRetryTimer = null
    }
    mountNextRetryAt.value = null
    pendingMountRetry = null
  }

  function scheduleMountRetry(action: () => void) {
    mountRetryAttempt.value += 1
    const delay = Math.min(BASE_RETRY_DELAY_MS * 2 ** (mountRetryAttempt.value - 1), MAX_RETRY_DELAY_MS)
    const jitter = delay * (0.8 + Math.random() * 0.4)
    mountNextRetryAt.value = Date.now() + jitter
    pendingMountRetry = action
    mountRetryTimer = setTimeout(action, jitter)
  }

  function retryMountNow() {
    if (mountRetryTimer) {
      clearTimeout(mountRetryTimer)
      mountRetryTimer = null
    }
    pendingMountRetry?.()
  }

  async function reconcileOnMount() {
    ensureSocketConnected()
    const epoch = ++mountEpoch
    try {
      const scans = await listScans()
      if (epoch !== mountEpoch) return
      const knownScanIds = new Set(
        Object.values(sessions.value)
          .map((s) => s.scanId)
          .filter((id): id is number => id !== null),
      )

      for (const summary of scans) {
        if (knownScanIds.has(summary.scanId)) continue
        const key = nextKey()
        sessions.value[key] = {
          key,
          scanId: summary.scanId,
          interfaceId: summary.interfaceId,
          mmsPort: summary.mmsPort,
          sweepIntervalMs: summary.sweepIntervalMs,
          phase: 'active',
          results: [],
          error: null,
          retryAttempt: 0,
          nextRetryAt: null,
          connectionInterrupted: false,
        }
      }

      clearMountRetryTimer()
      mountRetryAttempt.value = 0
      mountError.value = null
    } catch (err) {
      if (epoch !== mountEpoch) return
      const scanError = toScanError(err)
      mountError.value = scanError
      if (scanError.retryable) {
        scheduleMountRetry(reconcileOnMount)
      }
    }
  }

  function dispose() {
    mountEpoch++
    for (const key of Array.from(retryTimers.keys())) clearRetryTimer(key)
    clearMountRetryTimer()
    socket?.disconnect()
    socket = null
  }

  return {
    sessions,
    mountError,
    mountRetryAttempt,
    mountNextRetryAt,
    start,
    stop,
    retryNow,
    reconcileOnMount,
    retryMountNow,
    dispose,
  }
})

function toScanError(err: unknown): ScanError {
  if (err instanceof ApiError) {
    return {
      code: err.code,
      message: err.message,
      retryable: err.code === 'DAEMON_UNREACHABLE',
    }
  }
  return {
    code: 'UNKNOWN',
    message: err instanceof Error ? err.message : t('common.unexpectedError'),
    retryable: false,
  }
}
