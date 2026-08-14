import { describe, it, expect, vi, afterEach } from 'vitest'

vi.mock('@/services/apiClient', () => ({
  apiClient: {
    get: vi.fn(),
    post: vi.fn(),
    delete: vi.fn(),
  },
}))

import { apiClient } from '@/services/apiClient'
import {
  getNetworkStatus,
  applyNetworkConfig,
  probeReachability,
  confirmNetworkConfigAt,
  revertNetworkConfig,
  clearLogs,
} from '@/services/settingsApi'

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
    const pending = {
      new: { cidr: '192.168.1.60/24' },
      expiresAt: '2026-01-01T00:00:00Z',
      remainingSeconds: 90,
    }
    vi.mocked(apiClient.post).mockResolvedValue(pending)

    const result = await applyNetworkConfig({ cidr: '192.168.1.60/24' })

    expect(apiClient.post).toHaveBeenCalledWith('/settings/network', { cidr: '192.168.1.60/24' })
    expect(result).toEqual(pending)
  })

  it('probeReachability reports ok for a 2xx response from the given origin, bypassing apiClient', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response('{}', { status: 200 }))
    vi.stubGlobal('fetch', fetchMock)

    const result = await probeReachability('http://192.168.1.60')

    expect(result.outcome).toBe('ok')
    expect(result.status).toBe(200)
    expect(fetchMock).toHaveBeenCalledWith(
      'http://192.168.1.60/api/health',
      expect.objectContaining({ cache: 'no-store', signal: expect.anything() }),
    )
    expect(apiClient.get).not.toHaveBeenCalled()
  })

  // A reverse proxy that is up while the API behind it is down looks nothing like an address
  // that never came up, and the reconnect loop needs to be able to say which it saw.
  it('probeReachability reports http-error for a non-2xx response', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('', { status: 502 })))

    const result = await probeReachability('http://192.168.1.60')

    expect(result.outcome).toBe('http-error')
    expect(result.status).toBe(502)
  })

  // The distinction that matters most: the box answered, but refused this page permission to
  // read the response. Indistinguishable from "unreachable" to a plain fetch, which is why an
  // IP change that visibly worked could still poll forever without ever confirming.
  it('probeReachability reports blocked-by-cors when the opaque re-probe succeeds', async () => {
    const fetchMock = vi
      .fn()
      .mockRejectedValueOnce(new TypeError('Failed to fetch'))
      // A real opaque response has type 'opaque' and status 0, which the Response constructor
      // refuses to build. The probe never inspects this response — resolving at all is the whole
      // signal — so a stand-in is faithful to what the code actually depends on.
      .mockResolvedValueOnce({ type: 'opaque', status: 0, ok: false } as Response)
    vi.stubGlobal('fetch', fetchMock)

    const result = await probeReachability('http://192.168.1.60')

    expect(result.outcome).toBe('blocked-by-cors')
    expect(result.error).toContain('Failed to fetch')
    expect(fetchMock).toHaveBeenLastCalledWith(
      'http://192.168.1.60/api/health',
      expect.objectContaining({ mode: 'no-cors', signal: expect.anything() }),
    )
  })

  it('probeReachability reports unreachable when even the opaque re-probe fails', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('Failed to fetch')))

    const result = await probeReachability('http://192.168.1.60')

    expect(result.outcome).toBe('unreachable')
    expect(result.error).toContain('Failed to fetch')
  })

  // THE ONE THAT MATTERS. A probe opened while the box swaps its address hangs until the
  // browser's own connection timeout (~90s in Firefox) — longer than the entire confirmation
  // window it is supposed to be polling within. Every probe must be bounded.
  it('probeReachability reports timeout when the request exceeds its deadline', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new DOMException('signal timed out', 'TimeoutError')))

    const result = await probeReachability('http://192.168.1.60')

    expect(result.outcome).toBe('timeout')
    expect(result.error).toContain('TimeoutError')
  })

  // A timeout has already settled the question the opaque re-probe exists to answer, so paying
  // for it again would double the worst-case cost of an attempt.
  it('probeReachability skips the opaque re-probe on the timeout path', async () => {
    const fetchMock = vi.fn().mockRejectedValue(new DOMException('signal timed out', 'TimeoutError'))
    vi.stubGlobal('fetch', fetchMock)

    await probeReachability('http://192.168.1.60')

    expect(fetchMock).toHaveBeenCalledTimes(1)
  })

  it('confirmNetworkConfigAt posts to /settings/network/confirm at the given origin', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response('{}', { status: 200 }))
    vi.stubGlobal('fetch', fetchMock)

    await confirmNetworkConfigAt('http://192.168.1.60')

    expect(fetchMock).toHaveBeenCalledWith('http://192.168.1.60/api/settings/network/confirm', { method: 'POST' })
  })

  it('confirmNetworkConfigAt throws on a non-2xx response', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('', { status: 409 })))

    await expect(confirmNetworkConfigAt('http://192.168.1.60')).rejects.toThrow('409')
  })

  // Unlike confirm, revert always targets the page's own origin: it's used when the new address
  // never came up, so the box is still answering where it was.
  it('revertNetworkConfig posts to /settings/network/revert', async () => {
    vi.mocked(apiClient.post).mockResolvedValue(undefined)

    await revertNetworkConfig()

    expect(apiClient.post).toHaveBeenCalledWith('/settings/network/revert')
  })

  it('clearLogs posts to /settings/logs/clear and returns what was cleared', async () => {
    vi.mocked(apiClient.post).mockResolvedValue({
      logDir: '/var/log/station_signal',
      clearedCount: 12,
      skippedCount: 0,
      bytesFreed: 14680064,
    })

    const result = await clearLogs()

    expect(apiClient.post).toHaveBeenCalledWith('/settings/logs/clear')
    expect(result.clearedCount).toBe(12)
    expect(result.bytesFreed).toBe(14680064)
  })
})
