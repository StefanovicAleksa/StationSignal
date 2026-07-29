import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import ManualConnectForm from '@/components/device/ManualConnectForm.vue'

vi.mock('@/services/structureFileApi', () => ({
  uploadStructureFile: vi.fn(),
}))

vi.mock('@/services/settingsApi', () => ({
  getNetworkStatus: vi.fn(),
  applyNetworkConfig: vi.fn(),
  probeReachability: vi.fn(),
  confirmNetworkConfigAt: vi.fn(),
  revertNetworkConfig: vi.fn(),
}))

import { uploadStructureFile } from '@/services/structureFileApi'
import { getNetworkStatus } from '@/services/settingsApi'

function fill(wrapper: ReturnType<typeof mount>, id: string, value: string) {
  return wrapper.find(`#${id}`).setValue(value)
}

async function selectStructureFile(wrapper: ReturnType<typeof mount>, file: File) {
  const input = wrapper.find('input[type="file"]')
  Object.defineProperty(input.element, 'files', { value: [file], writable: false })
  await input.trigger('change')
  await flushPromises()
}

// The interface field defaults from the box's own network status (see ManualConnectForm.vue's
// onMounted), which resolves asynchronously — flush once after mounting so tests see it settled.
async function mountForm() {
  const wrapper = mount(ManualConnectForm, { props: { disabled: false } })
  await flushPromises()
  return wrapper
}

describe('ManualConnectForm', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.1.50/24' },
      recoveryAddress: '169.254.1.1',
    })
  })

  it("prefills the interface field with the box's active network interface", async () => {
    const wrapper = await mountForm()
    expect((wrapper.find('#manualInterfaceId').element as HTMLInputElement).value).toBe('eth0')
  })

  it('emits connect with undefined iedName/sclFilePath when left blank', async () => {
    const wrapper = await mountForm()
    await fill(wrapper, 'manualHost', '10.0.0.5')
    await wrapper.find('form').trigger('submit.prevent')

    expect(wrapper.emitted('connect')).toEqual([['10.0.0.5', 102, 'eth0', undefined, undefined]])
  })

  it('emits connect with iedName and the uploaded structure file path when both are provided', async () => {
    vi.mocked(uploadStructureFile).mockResolvedValue({ path: '/data/structure_files/abc-device.icd' })
    const wrapper = await mountForm()

    await fill(wrapper, 'manualHost', '10.0.0.5')
    await fill(wrapper, 'manualIedName', 'IED1')
    await selectStructureFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))
    await wrapper.find('form').trigger('submit.prevent')

    expect(uploadStructureFile).toHaveBeenCalledWith(expect.objectContaining({ name: 'device.icd' }))
    expect(wrapper.emitted('connect')).toEqual([
      ['10.0.0.5', 102, 'eth0', 'IED1', '/data/structure_files/abc-device.icd'],
    ])
  })

  it('blocks submit and shows a validation error when a structure file is uploaded without an iedName', async () => {
    vi.mocked(uploadStructureFile).mockResolvedValue({ path: '/data/structure_files/abc-device.icd' })
    const wrapper = await mountForm()

    await fill(wrapper, 'manualHost', '10.0.0.5')
    await selectStructureFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))
    await wrapper.find('form').trigger('submit.prevent')

    expect(wrapper.emitted('connect')).toBeUndefined()
    expect(wrapper.text()).toContain('IED name is required when a structure file path is given.')
  })

  it('clears sclFilePath when the upload fails', async () => {
    vi.mocked(uploadStructureFile).mockRejectedValue(new Error('upload failed'))
    const wrapper = await mountForm()

    await fill(wrapper, 'manualHost', '10.0.0.5')
    await fill(wrapper, 'manualIedName', 'IED1')
    await selectStructureFile(wrapper, new File(['<SCL/>'], 'device.icd', { type: 'text/xml' }))
    await wrapper.find('form').trigger('submit.prevent')

    expect(wrapper.emitted('connect')).toEqual([['10.0.0.5', 102, 'eth0', 'IED1', undefined]])
  })
})
