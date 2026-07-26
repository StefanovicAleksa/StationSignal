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
})
