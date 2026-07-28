import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'

vi.mock('@/services/settingsApi', () => ({
  getNetworkStatus: vi.fn(),
  applyNetworkConfig: vi.fn(),
  isReachableAt: vi.fn(),
  confirmNetworkConfigAt: vi.fn(),
}))

import { getNetworkStatus, applyNetworkConfig, isReachableAt, confirmNetworkConfigAt } from '@/services/settingsApi'
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
    expect(isReachableAt).not.toHaveBeenCalled()
  })

  it('submit success computes newOrigin from the submitted CIDR and moves to waitingForReconnect', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(isReachableAt).mockResolvedValue(false)
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
    vi.mocked(isReachableAt).mockResolvedValue(false)
    const store = useSettingsStore()

    await store.submit({ cidr: '10.0.0.5/24' })

    expect(store.newOrigin).toBe('http://10.0.0.5:5173')
  })

  it('polls with backoff until the new address answers, then confirms and redirects', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(isReachableAt).mockResolvedValueOnce(false).mockResolvedValueOnce(false).mockResolvedValueOnce(true)
    vi.mocked(confirmNetworkConfigAt).mockResolvedValue(undefined)
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    expect(isReachableAt).toHaveBeenCalledTimes(0)

    // Backoff delays are jittered (0.8x-1.2x), so don't assert an exact call count at each
    // fixed checkpoint — just advance well past the worst-case cumulative delay for 3 polls
    // and assert on the end state.
    await vi.advanceTimersByTimeAsync(15_000)

    expect(isReachableAt).toHaveBeenCalledTimes(3)
    expect(confirmNetworkConfigAt).toHaveBeenCalledWith('http://192.168.1.60')
    expect(store.phase).toBe('confirmed')
    expect(window.location.href).toBe('http://192.168.1.60')
  })

  it('a confirm failure after reachability surfaces as an error phase', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(isReachableAt).mockResolvedValue(true)
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
    vi.mocked(isReachableAt).mockResolvedValue(false)
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })

    await vi.advanceTimersByTimeAsync(20_000)

    expect(store.phase).toBe('reverted')
    expect(confirmNetworkConfigAt).not.toHaveBeenCalled()
  })

  it('pollNow triggers an immediate check instead of waiting for the scheduled delay', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(isReachableAt).mockResolvedValue(true)
    vi.mocked(confirmNetworkConfigAt).mockResolvedValue(undefined)
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    expect(isReachableAt).not.toHaveBeenCalled()

    await store.pollNow()

    expect(isReachableAt).toHaveBeenCalledTimes(1)
    expect(store.phase).toBe('confirmed')
  })

  it('dispose stops polling — no further isReachableAt calls after it', async () => {
    const expiresAt = new Date(Date.now() + 90_000).toISOString()
    vi.mocked(applyNetworkConfig).mockResolvedValue({ new: { cidr: '192.168.1.60/24' }, expiresAt })
    vi.mocked(isReachableAt).mockResolvedValue(false)
    const store = useSettingsStore()

    await store.submit({ cidr: '192.168.1.60/24' })
    store.dispose()

    await vi.advanceTimersByTimeAsync(30_000)

    expect(isReachableAt).not.toHaveBeenCalled()
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
})
