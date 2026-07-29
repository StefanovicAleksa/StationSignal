import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'

vi.mock('@/services/settingsApi', () => ({
  getNetworkStatus: vi.fn(),
  applyNetworkConfig: vi.fn(),
  probeReachability: vi.fn(),
  confirmNetworkConfigAt: vi.fn(),
  revertNetworkConfig: vi.fn(),
}))

import {
  getNetworkStatus,
  applyNetworkConfig,
  probeReachability,
  confirmNetworkConfigAt,
  revertNetworkConfig,
} from '@/services/settingsApi'
import { useSettingsStore } from '@/stores/settings'
import { ApiError } from '@/types/api'

const originalLocation = window.location

function stubLocation(overrides: Partial<Location> = {}) {
  Object.defineProperty(window, 'location', {
    writable: true,
    configurable: true,
    value: { protocol: 'http:', port: '', href: '', ...overrides },
  })
}

describe('useSettingsStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.useFakeTimers()
    vi.clearAllMocks()
    stubLocation()
  })

  afterEach(() => {
    vi.useRealTimers()
    Object.defineProperty(window, 'location', { writable: true, configurable: true, value: originalLocation })
  })

  it('loadStatus populates status on success', async () => {
    const status = { interface: 'eth0', current: { cidr: '192.168.1.50/24' }, recoveryAddress: '169.254.1.1' }
    vi.mocked(getNetworkStatus).mockResolvedValue(status)
    const store = useSettingsStore()

    await store.loadStatus()

    expect(store.status).toEqual(status)
    expect(store.statusError).toBeNull()
    expect(store.loadingStatus).toBe(false)
  })

  it('loadStatus sets statusError on failure', async () => {
    vi.mocked(getNetworkStatus).mockRejectedValue(new ApiError({ code: 'APPLY_FAILED', message: 'ip: not found', stage: null, detail: null }, 500))
    const store = useSettingsStore()

    await store.loadStatus()

    expect(store.status).toBeNull()
    expect(store.statusError).toEqual({ code: 'APPLY_FAILED', message: 'ip: not found' })
  })

  it('submit failure sets phase=error and does not schedule polling', async () => {
    vi.mocked(applyNetworkConfig).mockRejectedValue(
      new ApiError({ code: 'SESSIONS_ACTIVE', message: 'stop scans first', stage: null, detail: null }, 409),
    )
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })

    expect(store.phase).toBe('error')
    expect(store.applyError).toEqual({ code: 'SESSIONS_ACTIVE', message: 'stop scans first' })
    expect(probeReachability).not.toHaveBeenCalled()
  })

  it('submit success computes newOrigin from the submitted CIDR and moves to waitingForReconnect', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'unreachable', error: 'TypeError: Failed to fetch', durationMs: 5 })
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })

    expect(store.phase).toBe('waitingForReconnect')
    expect(store.newOrigin).toBe('http://192.168.1.60')
    expect(store.pendingExpiresAt).not.toBeNull()
  })

  it('preserves the current page port when computing newOrigin (dev server case)', async () => {
    stubLocation({ protocol: 'http:', port: '5173' })
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '10.0.0.5/24' }, expiresAt })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'unreachable', error: 'TypeError: Failed to fetch', durationMs: 5 })
    const store = useSettingsStore()

    await store.submit({ cidr: '10.0.0.5/24' })

    expect(store.newOrigin).toBe('http://10.0.0.5:5173')
  })

  it('polls with backoff until the new address answers, then confirms and redirects', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(probeReachability)
      .mockResolvedValueOnce({ outcome: 'unreachable', error: 'TypeError: Failed to fetch', durationMs: 5 })
      .mockResolvedValueOnce({ outcome: 'unreachable', error: 'TypeError: Failed to fetch', durationMs: 5 })
      .mockResolvedValueOnce({ outcome: 'ok', status: 200, durationMs: 5 })
    vi.mocked(confirmNetworkConfigAt).mockResolvedValue(undefined)
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    expect(probeReachability).toHaveBeenCalledTimes(0)

    // Backoff delays are jittered (0.8x-1.2x), so don't assert an exact call count at each
    // fixed checkpoint — just advance well past the worst-case cumulative delay for 3 polls
    // and assert on the end state.
    await vi.advanceTimersByTimeAsync(15_000)

    expect(probeReachability).toHaveBeenCalledTimes(3)
    expect(confirmNetworkConfigAt).toHaveBeenCalledWith('http://192.168.1.60')
    expect(store.phase).toBe('confirmed')
    expect(window.location.href).toBe('http://192.168.1.60')
  })

  it('a confirm failure after reachability surfaces as an error phase', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'ok', status: 200, durationMs: 5 })
    vi.mocked(confirmNetworkConfigAt).mockRejectedValue(new Error('confirm failed with status 409'))
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    await vi.advanceTimersByTimeAsync(2000)

    expect(store.phase).toBe('error')
    expect(store.applyError?.message).toContain('confirm failed')
  })

  it('gives up and reports reverted once past the expiry grace period without reachability', async () => {
    const expiresAt = new Date(Date.now() + 1000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'unreachable', error: 'TypeError: Failed to fetch', durationMs: 5 })
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })

    await vi.advanceTimersByTimeAsync(20_000)

    expect(store.phase).toBe('reverted')
    expect(confirmNetworkConfigAt).not.toHaveBeenCalled()
  })

  it('pollNow triggers an immediate check instead of waiting for the scheduled delay', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'ok', status: 200, durationMs: 5 })
    vi.mocked(confirmNetworkConfigAt).mockResolvedValue(undefined)
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    expect(probeReachability).not.toHaveBeenCalled()

    await store.pollNow()

    expect(probeReachability).toHaveBeenCalledTimes(1)
    expect(store.phase).toBe('confirmed')
  })

  it('dispose stops polling — no further probeReachability calls after it', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'unreachable', error: 'TypeError: Failed to fetch', durationMs: 5 })
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    store.dispose()

    await vi.advanceTimersByTimeAsync(30_000)

    expect(probeReachability).not.toHaveBeenCalled()
  })

  it('reset returns to idle and clears apply state', async () => {
    vi.mocked(applyNetworkConfig).mockRejectedValue(
      new ApiError({ code: 'INVALID_ARGUMENT', message: 'bad cidr', stage: null, detail: null }, 400),
    )
    const store = useSettingsStore()
    await store.submit({ cidr: 'garbage' })
    expect(store.phase).toBe('error')

    store.reset()

    expect(store.phase).toBe('idle')
    expect(store.applyError).toBeNull()
    expect(store.newOrigin).toBeNull()
    expect(store.pendingExpiresAt).toBeNull()
  })

  // THE REGRESSION TEST. A probe that never settles used to stall the reconnect loop completely:
  // attemptPoll awaited it before scheduling the next tick, so one socket hung during the box's
  // address swap consumed the entire 90s confirmation window in a single attempt.
  it('a probe that never resolves does not stall the reconnect loop', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    // First probe hangs forever; every later one answers normally.
    vi.mocked(probeReachability)
      .mockReturnValueOnce(new Promise(() => {}))
      .mockResolvedValue({ outcome: 'ok', status: 200, durationMs: 5 })
    vi.mocked(confirmNetworkConfigAt).mockResolvedValue(undefined)
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    await vi.advanceTimersByTimeAsync(10_000)

    expect(vi.mocked(probeReachability).mock.calls.length).toBeGreaterThan(1)
    expect(confirmNetworkConfigAt).toHaveBeenCalledWith('http://192.168.1.60')
    expect(store.phase).toBe('confirmed')
  })

  // Probes can now overlap, and doConfirm POSTs — it must not be able to fire twice.
  it('overlapping reachable probes only confirm once', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'ok', status: 200, durationMs: 5 })
    vi.mocked(confirmNetworkConfigAt).mockResolvedValue(undefined)
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    await vi.advanceTimersByTimeAsync(20_000)

    expect(confirmNetworkConfigAt).toHaveBeenCalledTimes(1)
  })

  // Even with probes permanently in flight, the deadline still has to be honoured.
  it('gives up past the expiry grace period even while a probe is still hanging', async () => {
    const expiresAt = new Date(Date.now() + 1000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(probeReachability).mockReturnValue(new Promise(() => {}))
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
    })
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    await vi.advanceTimersByTimeAsync(20_000)

    expect(store.phase).toBe('reverted')
    expect(confirmNetworkConfigAt).not.toHaveBeenCalled()
  })

  // The box refusing because it's still holding an earlier change is the one failure the
  // technician can't wait out — it blocks every apply until something clears it, so the UI has
  // to know to offer that.
  it('stuckPending is true when the box reports a pending change', async () => {
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
      pending: { new: { cidr: '192.168.1.77/24' }, expiresAt: new Date().toISOString() },
    })
    const store = useSettingsStore()

    await store.loadStatus()

    expect(store.stuckPending).toBe(true)
  })

  it('stuckPending is true when apply is refused as CHANGE_ALREADY_PENDING', async () => {
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
    })
    vi.mocked(applyNetworkConfig).mockRejectedValue(
      new ApiError({ code: 'CHANGE_ALREADY_PENDING', message: 'already pending', stage: null, detail: null }, 409),
    )
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.77/24' })

    expect(store.phase).toBe('error')
    expect(store.stuckPending).toBe(true)
  })

  it('stuckPending is false when nothing is pending', async () => {
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
    })
    const store = useSettingsStore()

    await store.loadStatus()

    expect(store.stuckPending).toBe(false)
  })

  it('revert clears the pending change and re-reads the box state', async () => {
    vi.mocked(revertNetworkConfig).mockResolvedValue(undefined)
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
    })
    const store = useSettingsStore()

    await store.revert()

    expect(revertNetworkConfig).toHaveBeenCalled()
    expect(store.phase).toBe('idle')
    expect(store.stuckPending).toBe(false)
    expect(getNetworkStatus).toHaveBeenCalled()
  })

  // A revert that cleared the wedge but couldn't restore the old address must still surface as a
  // failure — the box may not be where the technician expects it.
  it('revert failure surfaces as an error but still refreshes status', async () => {
    vi.mocked(revertNetworkConfig).mockRejectedValue(
      new ApiError({ code: 'APPLY_FAILED', message: 'nmcli could not reactivate', stage: null, detail: null }, 500),
    )
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
    })
    const store = useSettingsStore()

    await store.revert()

    expect(store.phase).toBe('error')
    expect(store.applyError).toEqual({ code: 'APPLY_FAILED', message: 'nmcli could not reactivate' })
    expect(getNetworkStatus).toHaveBeenCalled()
  })

  it('revert stops any in-flight reconnect polling', async () => {
    vi.mocked(applyNetworkConfig).mockResolvedValue({
      new: { cidr: '192.168.1.77/24' },
      expiresAt: new Date(Date.now() + 90_000).toISOString(),
    })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'unreachable', error: 'TypeError: Failed to fetch', durationMs: 5 })
    vi.mocked(revertNetworkConfig).mockResolvedValue(undefined)
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
    })
    const store = useSettingsStore()
    await store.submit({ cidr: '192.168.1.77/24' })
    expect(store.phase).toBe('waitingForReconnect')

    await store.revert()

    expect(store.nextPollAt).toBeNull()
    await vi.advanceTimersByTimeAsync(30_000)
    expect(store.phase).toBe('idle')
  })

  // The box's auto-revert runs nmcli too, and can fail exactly the way the apply did. Giving up
  // on polling must re-read the box's real state rather than asserting a revert happened.
  it('giving up on reconnect re-reads status instead of assuming the box reverted', async () => {
    vi.mocked(applyNetworkConfig).mockResolvedValue({
      new: { cidr: '192.168.1.77/24' },
      expiresAt: new Date(Date.now() + 1_000).toISOString(),
    })
    vi.mocked(probeReachability).mockResolvedValue({ outcome: 'unreachable', error: 'TypeError: Failed to fetch', durationMs: 5 })
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
      pending: { new: { cidr: '192.168.1.77/24' }, expiresAt: new Date().toISOString() },
    })
    const store = useSettingsStore()
    await store.submit({ cidr: '192.168.1.77/24' })

    await vi.advanceTimersByTimeAsync(30_000)

    expect(store.phase).toBe('reverted')
    expect(getNetworkStatus).toHaveBeenCalled()
    expect(store.stuckPending).toBe(true)
  })
})
