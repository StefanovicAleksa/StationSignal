import { describe, it, expect, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import StructureFileUpload from '@/components/device/StructureFileUpload.vue'

vi.mock('@/services/structureFileApi', () => ({
  uploadStructureFile: vi.fn(),
}))

import { uploadStructureFile } from '@/services/structureFileApi'

function fileInputOf(wrapper: ReturnType<typeof mount>) {
  return wrapper.find('input[type="file"]')
}

async function selectFile(wrapper: ReturnType<typeof mount>, file: File) {
  const input = fileInputOf(wrapper)
  Object.defineProperty(input.element, 'files', { value: [file], writable: false })
  await input.trigger('change')
  await flushPromises()
}

describe('StructureFileUpload', () => {
  it('uploads a file selected via the hidden file input and emits its path', async () => {
    vi.mocked(uploadStructureFile).mockResolvedValue({ path: '/data/structure_files/abc-device.icd' })
    const wrapper = mount(StructureFileUpload, { props: { disabled: false } })

    await selectFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))

    expect(wrapper.emitted('update:path')).toEqual([['/data/structure_files/abc-device.icd']])
    expect(wrapper.text()).toContain('device.icd')
  })

  it('uploads a file dropped onto the dropzone', async () => {
    vi.mocked(uploadStructureFile).mockResolvedValue({ path: '/data/structure_files/abc-device.icd' })
    const wrapper = mount(StructureFileUpload, { props: { disabled: false } })
    const file = new File(['<SCL/>'], 'dropped.icd', { type: 'text/xml' })

    await wrapper.find('div.border-dashed').trigger('drop', { dataTransfer: { files: [file] } })
    await flushPromises()

    expect(uploadStructureFile).toHaveBeenCalledWith(file)
    expect(wrapper.emitted('update:path')).toEqual([['/data/structure_files/abc-device.icd']])
  })

  it('shows an error and emits undefined when the upload fails', async () => {
    vi.mocked(uploadStructureFile).mockRejectedValue(new Error('boom'))
    const wrapper = mount(StructureFileUpload, { props: { disabled: false } })

    await selectFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))

    expect(wrapper.emitted('update:path')).toEqual([[undefined]])
    expect(wrapper.text()).toContain('Failed to upload structure file.')
  })

  it('clears the selection and emits undefined when Remove is clicked', async () => {
    vi.mocked(uploadStructureFile).mockResolvedValue({ path: '/data/structure_files/abc-device.icd' })
    const wrapper = mount(StructureFileUpload, { props: { disabled: false } })
    await selectFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))

    await wrapper.find('button').trigger('click')

    const emissions = wrapper.emitted('update:path')
    expect(emissions).toBeDefined()
    expect(emissions![emissions!.length - 1]).toEqual([undefined])
    expect(wrapper.text()).not.toContain('device.icd')
  })
})
