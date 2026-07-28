import { describe, it, expect, vi, afterEach } from 'vitest'

vi.mock('@/services/apiClient', () => ({
  apiClient: {
    get: vi.fn(),
    post: vi.fn(),
    delete: vi.fn(),
  },
}))

import { apiClient } from '@/services/apiClient'
import { getNetworkStatus, applyNetworkConfig, isReachableAt, confirmNetworkConfigAt } from '@/services/settingsApi'

describe('settingsApi', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    vi.clearAllMocks()
  })

  it('getNetworkStatus gets /settings/network', async () => {
    const status = { interface: 'eth0', current: { cidr: '192.168.1.50/24' }, recoveryAddress: '169.254.1.1' }
    vi.mocked(apiClient.get).mockResolvedValue(status)

    const result = await getNetworkStatus()

    expect(apiClient.get).toHaveBeenCalledWith('/settings/network')
    expect(result).toEqual(status)
  })

  it('applyNetworkConfig posts to /settings/network with the request body', async () => {
    const pending = { new: { cidr: '192.168.1.60/24' }, expiresAt: '2026-01-01T00:00:00Z' }
    vi.mocked(apiClient.post).mockResolvedValue(pending)

    const result = await applyNetworkConfig({ cidr: '192.168.1.60/24' })

    expect(apiClient.post).toHaveBeenCalledWith('/settings/network', { cidr: '192.168.1.60/24' })
    expect(result).toEqual(pending)
  })

  it('isReachableAt returns true for a 2xx response from the given origin, bypassing apiClient', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response('{}', { status: 200 }))
    vi.stubGlobal('fetch', fetchMock)

    const result = await isReachableAt('http://192.168.1.60')

    expect(result).toBe(true)
    expect(fetchMock).toHaveBeenCalledWith('http://192.168.1.60/health')
    expect(apiClient.get).not.toHaveBeenCalled()
  })

  it('isReachableAt returns false for a non-2xx response', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('', { status: 503 })))

    const result = await isReachableAt('http://192.168.1.60')

    expect(result).toBe(false)
  })

  it('isReachableAt returns false (not throw) when fetch itself rejects', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('Failed to fetch')))

    const result = await isReachableAt('http://192.168.1.60')

    expect(result).toBe(false)
  })

  it('confirmNetworkConfigAt posts to /settings/network/confirm at the given origin', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response('{}', { status: 200 }))
    vi.stubGlobal('fetch', fetchMock)

    await confirmNetworkConfigAt('http://192.168.1.60')

    expect(fetchMock).toHaveBeenCalledWith('http://192.168.1.60/settings/network/confirm', { method: 'POST' })
  })

  it('confirmNetworkConfigAt throws on a non-2xx response', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('', { status: 409 })))

    await expect(confirmNetworkConfigAt('http://192.168.1.60')).rejects.toThrow('409')
  })
})
