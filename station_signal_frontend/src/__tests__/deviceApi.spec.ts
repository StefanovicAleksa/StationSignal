import { describe, it, expect, vi } from 'vitest'

vi.mock('@/services/apiClient', () => ({
  apiClient: {
    get: vi.fn(),
    post: vi.fn(),
    delete: vi.fn(),
  },
}))

import { apiClient } from '@/services/apiClient'
import { startReporting, stopReporting, listDevices } from '@/services/deviceApi'

describe('deviceApi', () => {
  it('startReporting posts to /devices with the request body', async () => {
    vi.mocked(apiClient.post).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    const result = await startReporting({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })
    expect(apiClient.post).toHaveBeenCalledWith('/devices', { host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0' })
    expect(result).toEqual({ deviceId: 1, wsPort: 9000 })
  })

  it('startReporting forwards acseAuthPassword when retrying a password-protected device', async () => {
    vi.mocked(apiClient.post).mockResolvedValue({ deviceId: 1, wsPort: 9000 })
    await startReporting({ host: '10.0.0.5', mmsPort: 102, interfaceId: 'eth0', acseAuthPassword: 'secret123' })
    expect(apiClient.post).toHaveBeenCalledWith('/devices', {
      host: '10.0.0.5',
      mmsPort: 102,
      interfaceId: 'eth0',
      acseAuthPassword: 'secret123',
    })
  })

  it('stopReporting deletes /devices/{id}', async () => {
    vi.mocked(apiClient.delete).mockResolvedValue({ deviceId: 1 })
    const result = await stopReporting(1)
    expect(apiClient.delete).toHaveBeenCalledWith('/devices/1')
    expect(result).toEqual({ deviceId: 1 })
  })

  it('listDevices gets /devices', async () => {
    vi.mocked(apiClient.get).mockResolvedValue([])
    const result = await listDevices()
    expect(apiClient.get).toHaveBeenCalledWith('/devices')
    expect(result).toEqual([])
  })
})
