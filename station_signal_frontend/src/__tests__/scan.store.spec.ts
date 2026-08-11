import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'

vi.mock('@/services/scanApi', () => ({
  startScan: vi.fn(),
  stopScan: vi.fn(),
  listScans: vi.fn(),
}))

vi.mock('@/services/scanSocket', () => ({
  createScanSocket: vi.fn(() => ({
    connect: vi.fn(),
    disconnect: vi.fn(),
  })),
}))

import { startScan, stopScan, listScans } from '@/services/scanApi'
import { createScanSocket } from '@/services/scanSocket'
import { useScanStore } from '@/stores/scan'
import { ApiError, type ScanResultMessage } from '@/types/api'

describe('useScanStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.clearAllMocks()
  })

  afterEach(() => {
    vi.useRealTimers()
  })

  it('goes starting -> active on a successful start', async () => {
    vi.mocked(startScan).mockResolvedValue({ scanId: 1 })
    const store = useScanStore()

    const promise = store.start('eth0', 102)
    const key = await promise

    expect(store.sessions[key]?.phase).toBe('active')
    expect(store.sessions[key]?.scanId).toBe(1)
    expect(createScanSocket).toHaveBeenCalledTimes(1)
  })

  it('connects the socket before the startScan REST call resolves', async () => {
    let resolveStart: ((value: { scanId: number }) => void) | undefined
    vi.mocked(startScan).mockReturnValue(
      new Promise((resolve) => {
        resolveStart = resolve
      }),
    )
    const store = useScanStore()

    const startPromise = store.start('eth0', 102)

    expect(createScanSocket).toHaveBeenCalledTimes(1)

    resolveStart?.({ scanId: 1 })
    await startPromise
  })

  it('runs two concurrent scans through one shared socket', async () => {
    vi.mocked(startScan).mockResolvedValueOnce({ scanId: 1 }).mockResolvedValueOnce({ scanId: 2 })
    const store = useScanStore()

    const keyA = await store.start('eth0', 102)
    const keyB = await store.start('eth1', 102)

    expect(store.sessions[keyA]?.scanId).toBe(1)
    expect(store.sessions[keyB]?.scanId).toBe(2)
    expect(store.sessions[keyA]?.phase).toBe('active')
    expect(store.sessions[keyB]?.phase).toBe('active')
    expect(createScanSocket).toHaveBeenCalledTimes(1)
  })

  it('one scan failing to start does not affect a concurrently active scan', async () => {
    vi.mocked(startScan)
      .mockResolvedValueOnce({ scanId: 1 })
      .mockRejectedValueOnce(
        new ApiError({ code: 'HOST_ALREADY_RUNNING', message: 'already running', stage: null, detail: null }, 409),
      )
    const store = useScanStore()

    const keyA = await store.start('eth0', 102)
    const keyB = await store.start('eth1', 102)

    expect(store.sessions[keyA]?.phase).toBe('active')
    expect(store.sessions[keyB]?.phase).toBe('error')
    expect(store.sessions[keyB]?.error?.retryable).toBe(false)
  })

  it('retries a failed start independently while another scan stays active', async () => {
    vi.mocked(startScan)
      .mockResolvedValueOnce({ scanId: 1 })
      .mockRejectedValueOnce(
        new ApiError({ code: 'DAEMON_UNREACHABLE', message: 'unreachable', stage: null, detail: null }, 503),
      )
      .mockResolvedValueOnce({ scanId: 2 })
    const store = useScanStore()

    const keyA = await store.start('eth0', 102)
    const keyB = await store.start('eth1', 102)

    expect(store.sessions[keyA]?.phase).toBe('active')
    expect(store.sessions[keyB]?.phase).toBe('error')
    expect(store.sessions[keyB]?.error?.retryable).toBe(true)

    await vi.advanceTimersByTimeAsync(2000)

    expect(store.sessions[keyB]?.phase).toBe('active')
    expect(store.sessions[keyB]?.scanId).toBe(2)
    expect(store.sessions[keyA]?.phase).toBe('active')
  })

  it('stopping every scan never disconnects the socket — only dispose() does', async () => {
    vi.mocked(startScan).mockResolvedValueOnce({ scanId: 1 }).mockResolvedValueOnce({ scanId: 2 })
    vi.mocked(stopScan).mockResolvedValue({ scanId: 1 })
    const store = useScanStore()

    const keyA = await store.start('eth0', 102)
    const keyB = await store.start('eth1', 102)
    const socketInstance = vi.mocked(createScanSocket).mock.results[0]?.value

    await store.stop(keyA)

    expect(store.sessions[keyA]).toBeUndefined()
    expect(store.sessions[keyB]?.phase).toBe('active')
    expect(socketInstance.disconnect).not.toHaveBeenCalled()

    await store.stop(keyB)

    expect(store.sessions[keyB]).toBeUndefined()
    expect(socketInstance.disconnect).not.toHaveBeenCalled()
  })

  it('routes SCAN_RESULT messages to the session with the matching scanId', async () => {
    let capturedOnMessage: ((message: ScanResultMessage) => void) | undefined
    vi.mocked(createScanSocket).mockImplementation((handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startScan).mockResolvedValueOnce({ scanId: 1 }).mockResolvedValueOnce({ scanId: 2 })

    const store = useScanStore()
    const keyA = await store.start('eth0', 102)
    const keyB = await store.start('eth1', 102)

    capturedOnMessage?.({ schemaVersion: 1, type: 'SCAN_RESULT', scanId: 1, host: '10.0.0.1', mmsPort: 102, discoveredAtMs: 1 })
    capturedOnMessage?.({ schemaVersion: 1, type: 'SCAN_RESULT', scanId: 2, host: '10.0.0.2', mmsPort: 102, discoveredAtMs: 2 })
    capturedOnMessage?.({ schemaVersion: 1, type: 'SCAN_RESULT', scanId: 99, host: '10.0.0.3', mmsPort: 102, discoveredAtMs: 3 })

    expect(store.sessions[keyA]?.results).toHaveLength(1)
    expect(store.sessions[keyA]?.results[0]?.host).toBe('10.0.0.1')
    expect(store.sessions[keyB]?.results).toHaveLength(1)
    expect(store.sessions[keyB]?.results[0]?.host).toBe('10.0.0.2')
  })

  it('re-arms a fresh socket once the previous one exhausts its own reconnect attempts', async () => {
    let capturedOnDisconnected: ((reason: 'intentional' | 'dropped' | 'exhausted') => void) | undefined
    vi.mocked(createScanSocket).mockImplementation((handlers) => {
      capturedOnDisconnected = handlers.onDisconnected
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startScan).mockResolvedValue({ scanId: 1 })

    const store = useScanStore()
    await store.start('eth0', 102)

    expect(createScanSocket).toHaveBeenCalledTimes(1)

    capturedOnDisconnected?.('exhausted')

    expect(createScanSocket).toHaveBeenCalledTimes(2)
  })

  it('stop is a no-op while the scan is still starting', async () => {
    let resolveStart: ((value: { scanId: number }) => void) | undefined
    vi.mocked(startScan).mockReturnValue(
      new Promise((resolve) => {
        resolveStart = resolve
      }),
    )
    const store = useScanStore()

    const startPromise = store.start('eth0', 102)
    const key = Object.keys(store.sessions)[0]!

    await store.stop(key)
    expect(stopScan).not.toHaveBeenCalled()
    expect(store.sessions[key]).toBeDefined()

    resolveStart?.({ scanId: 1 })
    await startPromise
  })

  it('treats SCAN_NOT_FOUND on stop as success', async () => {
    vi.mocked(startScan).mockResolvedValue({ scanId: 1 })
    vi.mocked(stopScan).mockRejectedValue(
      new ApiError({ code: 'SCAN_NOT_FOUND', message: 'not found', stage: null, detail: null }, 404),
    )
    const store = useScanStore()
    const key = await store.start('eth0', 102)

    await store.stop(key)

    expect(store.sessions[key]).toBeUndefined()
  })

  it('reconcileOnMount connects the socket eagerly even with zero active scans', async () => {
    vi.mocked(listScans).mockResolvedValue([])
    const store = useScanStore()

    await store.reconcileOnMount()

    expect(Object.keys(store.sessions)).toHaveLength(0)
    expect(createScanSocket).toHaveBeenCalledTimes(1)
  })

  it('reconcileOnMount adopts every already-active scan and connects the socket once', async () => {
    vi.mocked(listScans).mockResolvedValue([
      { scanId: 5, interfaceId: 'eth1', mmsPort: 102, sweepIntervalMs: 0 },
      { scanId: 6, interfaceId: 'eth2', mmsPort: 102, sweepIntervalMs: 0 },
    ])
    const store = useScanStore()

    await store.reconcileOnMount()

    const sessions = Object.values(store.sessions)
    expect(sessions).toHaveLength(2)
    expect(sessions.map((s) => s.scanId).sort()).toEqual([5, 6])
    expect(sessions.every((s) => s.phase === 'active')).toBe(true)
    expect(createScanSocket).toHaveBeenCalledTimes(1)
  })

  it('reconcileOnMount does not duplicate or reset an already-tracked scan', async () => {
    vi.mocked(listScans).mockResolvedValue([{ scanId: 5, interfaceId: 'eth1', mmsPort: 102, sweepIntervalMs: 0 }])
    const store = useScanStore()

    await store.reconcileOnMount()
    const key = Object.keys(store.sessions)[0]!
    store.sessions[key]!.results.push({
      schemaVersion: 1,
      type: 'SCAN_RESULT',
      scanId: 5,
      host: '10.0.0.9',
      mmsPort: 102,
      discoveredAtMs: 1,
    })

    await store.reconcileOnMount()

    expect(Object.keys(store.sessions)).toHaveLength(1)
    expect(store.sessions[key]?.results).toHaveLength(1)
  })

  it('ignores a stale reconcileOnMount response that resolves after dispose()', async () => {
    let resolveList: ((value: Awaited<ReturnType<typeof listScans>>) => void) | undefined
    vi.mocked(listScans).mockReturnValue(
      new Promise((resolve) => {
        resolveList = resolve
      }),
    )
    const store = useScanStore()

    const reconcilePromise = store.reconcileOnMount()
    store.dispose()

    resolveList?.([{ scanId: 5, interfaceId: 'eth1', mmsPort: 102, sweepIntervalMs: 0 }])
    await reconcilePromise

    expect(Object.keys(store.sessions)).toHaveLength(0)
  })

  it('reconcileOnMount failure sets mountError without touching sessions', async () => {
    vi.mocked(listScans).mockRejectedValue(
      new ApiError({ code: 'HOST_ALREADY_RUNNING', message: 'boom', stage: null, detail: null }, 409),
    )
    const store = useScanStore()

    await store.reconcileOnMount()

    expect(store.mountError?.code).toBe('HOST_ALREADY_RUNNING')
    expect(Object.keys(store.sessions)).toHaveLength(0)
  })

  // A discovered host used to be deleted from its session's results as soon as a connect
  // succeeded. The daemon's scan worker keeps a per-scan seen-set and never republishes a host it
  // has already reported, so that row never came back — stopping reporting left no way to reach
  // the device again short of restarting the whole scan. Results are now kept for the life of the
  // session, and ScanResultsTable derives each row's Connect/View state from the devices store.
  it('keeps a discovered host in its session results for the life of the scan', async () => {
    let capturedOnMessage: ((message: ScanResultMessage) => void) | undefined
    vi.mocked(createScanSocket).mockImplementation((handlers) => {
      capturedOnMessage = handlers.onMessage
      return { connect: vi.fn(), disconnect: vi.fn() }
    })
    vi.mocked(startScan).mockResolvedValue({ scanId: 1 })

    const store = useScanStore()
    const key = await store.start('eth0', 102)

    capturedOnMessage?.({ schemaVersion: 1, type: 'SCAN_RESULT', scanId: 1, host: '10.0.0.1', mmsPort: 102, discoveredAtMs: 1 })
    capturedOnMessage?.({ schemaVersion: 1, type: 'SCAN_RESULT', scanId: 1, host: '10.0.0.2', mmsPort: 102, discoveredAtMs: 2 })

    expect(store.sessions[key]?.results.map((result) => result.host)).toEqual(['10.0.0.1', '10.0.0.2'])
  })

  it('dispose clears retry timers and disconnects the socket exactly once', async () => {
    vi.mocked(startScan).mockResolvedValueOnce({ scanId: 1 }).mockResolvedValueOnce({ scanId: 2 })
    const store = useScanStore()
    await store.start('eth0', 102)
    await store.start('eth1', 102)
    const socketInstance = vi.mocked(createScanSocket).mock.results[0]?.value

    store.dispose()

    expect(socketInstance.disconnect).toHaveBeenCalledTimes(1)

    await vi.advanceTimersByTimeAsync(20000)
    expect(startScan).toHaveBeenCalledTimes(2)
  })
})
