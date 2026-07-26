import { describe, it, expect, vi } from 'vitest'

vi.mock('@/services/apiClient', () => ({
  apiClient: {
    get: vi.fn(),
    post: vi.fn(),
    postForm: vi.fn(),
    delete: vi.fn(),
  },
}))

import { apiClient } from '@/services/apiClient'
import { uploadStructureFile } from '@/services/structureFileApi'

describe('structureFileApi', () => {
  it('uploadStructureFile posts the file as multipart form data to /structure-files', async () => {
    vi.mocked(apiClient.postForm).mockResolvedValue({ path: '/data/structure_files/abc-device.icd' })
    const file = new File(['<SCL/>'], 'device.icd', { type: 'text/xml' })

    const result = await uploadStructureFile(file)

    expect(apiClient.postForm).toHaveBeenCalledTimes(1)
    const [path, form] = vi.mocked(apiClient.postForm).mock.calls[0]!
    expect(path).toBe('/structure-files')
    expect(form).toBeInstanceOf(FormData)
    expect((form as FormData).get('file')).toBe(file)
    expect(result).toEqual({ path: '/data/structure_files/abc-device.icd' })
  })
})
