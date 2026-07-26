import { describe, it, expect, vi } from 'vitest'

vi.mock('@/services/apiClient', () => ({
  apiClient: {
    get: vi.fn(),
    post: vi.fn(),
    delete: vi.fn(),
  },
}))

import { apiClient } from '@/services/apiClient'
import { startScan, stopScan, listScans } from '@/services/scanApi'

describe('scanApi', () => {
  it('startScan posts to /scans with the request body', async () => {
    vi.mocked(apiClient.post).mockResolvedValue({ scanId: 1 })
    const result = await startScan({ interfaceId: 'eth0', mmsPort: 102 })
    expect(apiClient.post).toHaveBeenCalledWith('/scans', { interfaceId: 'eth0', mmsPort: 102 })
    expect(result).toEqual({ scanId: 1 })
  })

  it('stopScan deletes /scans/{id}', async () => {
    vi.mocked(apiClient.delete).mockResolvedValue({ scanId: 1 })
    const result = await stopScan(1)
    expect(apiClient.delete).toHaveBeenCalledWith('/scans/1')
    expect(result).toEqual({ scanId: 1 })
  })

  it('listScans gets /scans', async () => {
    vi.mocked(apiClient.get).mockResolvedValue([])
    const result = await listScans()
    expect(apiClient.get).toHaveBeenCalledWith('/scans')
    expect(result).toEqual([])
  })
})
