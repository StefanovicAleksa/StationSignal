import { describe, it, expect, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import ManualConnectForm from '@/components/device/ManualConnectForm.vue'

vi.mock('@/services/structureFileApi', () => ({
  uploadStructureFile: vi.fn(),
}))

import { uploadStructureFile } from '@/services/structureFileApi'

function fill(wrapper: ReturnType<typeof mount>, id: string, value: string) {
  return wrapper.find(`#${id}`).setValue(value)
}

async function selectStructureFile(wrapper: ReturnType<typeof mount>, file: File) {
  const input = wrapper.find('input[type="file"]')
  Object.defineProperty(input.element, 'files', { value: [file], writable: false })
  await input.trigger('change')
  await flushPromises()
}

describe('ManualConnectForm', () => {
  it('prefills the interface field with enp34s0', () => {
    const wrapper = mount(ManualConnectForm, { props: { disabled: false } })
    expect((wrapper.find('#manualInterfaceId').element as HTMLInputElement).value).toBe('enp34s0')
  })

  it('emits connect with undefined iedName/sclFilePath when left blank', async () => {
    const wrapper = mount(ManualConnectForm, { props: { disabled: false } })
    await fill(wrapper, 'manualHost', '10.0.0.5')
    await wrapper.find('form').trigger('submit.prevent')

    expect(wrapper.emitted('connect')).toEqual([['10.0.0.5', 102, 'enp34s0', undefined, undefined]])
  })

  it('emits connect with iedName and the uploaded structure file path when both are provided', async () => {
    vi.mocked(uploadStructureFile).mockResolvedValue({ path: '/data/structure_files/abc-device.icd' })
    const wrapper = mount(ManualConnectForm, { props: { disabled: false } })

    await fill(wrapper, 'manualHost', '10.0.0.5')
    await fill(wrapper, 'manualIedName', 'IED1')
    await selectStructureFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))
    await wrapper.find('form').trigger('submit.prevent')

    expect(uploadStructureFile).toHaveBeenCalledWith(expect.objectContaining({ name: 'device.icd' }))
    expect(wrapper.emitted('connect')).toEqual([
      ['10.0.0.5', 102, 'enp34s0', 'IED1', '/data/structure_files/abc-device.icd'],
    ])
  })

  it('blocks submit and shows a validation error when a structure file is uploaded without an iedName', async () => {
    vi.mocked(uploadStructureFile).mockResolvedValue({ path: '/data/structure_files/abc-device.icd' })
    const wrapper = mount(ManualConnectForm, { props: { disabled: false } })

    await fill(wrapper, 'manualHost', '10.0.0.5')
    await selectStructureFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))
    await wrapper.find('form').trigger('submit.prevent')

    expect(wrapper.emitted('connect')).toBeUndefined()
    expect(wrapper.text()).toContain('IED name is required when a structure file path is given.')
  })

  it('clears sclFilePath when the upload fails', async () => {
    vi.mocked(uploadStructureFile).mockRejectedValue(new Error('upload failed'))
    const wrapper = mount(ManualConnectForm, { props: { disabled: false } })

    await fill(wrapper, 'manualHost', '10.0.0.5')
    await fill(wrapper, 'manualIedName', 'IED1')
    await selectStructureFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))
    await wrapper.find('form').trigger('submit.prevent')

    expect(wrapper.emitted('connect')).toEqual([['10.0.0.5', 102, 'enp34s0', 'IED1', undefined]])
  })
})
