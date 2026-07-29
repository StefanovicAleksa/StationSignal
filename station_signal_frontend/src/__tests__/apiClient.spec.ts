import { describe, it, expect, vi, afterEach } from 'vitest'
import { apiClient } from '@/services/apiClient'
import { ApiError } from '@/types/api'

describe('apiClient', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
  })

  it('parses a successful JSON response', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue(
        new Response(JSON.stringify({ scanId: 1 }), { status: 201 }),
      ),
    )

    const result = await apiClient.post<{ scanId: number }>('/scans', { interfaceId: 'eth0' })
    expect(result).toEqual({ scanId: 1 })
  })

  // Every REST resource is mounted under /api (see station_signal_api's rest.Router) so nginx
  // can proxy the whole subtree without colliding with frontend SPA page routes of the same
  // name (e.g. /devices, /settings) — apiClient must add that prefix on every call.
  it('prefixes every request path with /api', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response('{}', { status: 200 }))
    vi.stubGlobal('fetch', fetchMock)

    await apiClient.get('/devices')

    expect(fetchMock).toHaveBeenCalledWith(
      expect.stringMatching(/\/api\/devices$/),
      expect.anything(),
    )
  })

  it('throws a typed ApiError for a non-2xx error envelope', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue(
        new Response(
          JSON.stringify({ error: { code: 'HOST_ALREADY_RUNNING', message: 'already running', stage: null, detail: null } }),
          { status: 409 },
        ),
      ),
    )

    await expect(apiClient.post('/scans', {})).rejects.toMatchObject({
      code: 'HOST_ALREADY_RUNNING',
      httpStatus: 409,
    })
    await expect(apiClient.post('/scans', {})).rejects.toBeInstanceOf(ApiError)
  })

  it('falls back to a generic error when the error body is malformed', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue(new Response('not json', { status: 500, statusText: 'Internal Server Error' })),
    )

    await expect(apiClient.get('/scans')).rejects.toMatchObject({ code: 'UNKNOWN', httpStatus: 500 })
  })

  it('throws a NETWORK_ERROR-coded ApiError when fetch() itself rejects', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('Failed to fetch')))

    await expect(apiClient.get('/health')).rejects.toMatchObject({ code: 'NETWORK_ERROR', httpStatus: 0 })
    await expect(apiClient.get('/health')).rejects.toBeInstanceOf(ApiError)
  })
})
