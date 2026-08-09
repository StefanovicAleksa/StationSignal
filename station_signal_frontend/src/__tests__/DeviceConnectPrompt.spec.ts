import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { setActivePinia, createPinia } from 'pinia'
import DeviceConnectPrompt from '@/components/device/DeviceConnectPrompt.vue'

vi.mock('@/services/deviceApi', () => ({
  startReporting: vi.fn(),
  stopReporting: vi.fn(),
  stopReportingByAddress: vi.fn(),
  listDevices: vi.fn(),
}))

vi.mock('@/services/deviceSocket', () => ({
  createDeviceSocket: vi.fn(() => ({
    connect: vi.fn(),
    disconnect: vi.fn(),
  })),
}))

import { startReporting } from '@/services/deviceApi'
import { ApiError } from '@/types/api'

function mountPrompt() {
  return mount(DeviceConnectPrompt, {
    global: { stubs: { teleport: true } },
  })
}

function connectButtonInModal(wrapper: ReturnType<typeof mountPrompt>) {
  const button = wrapper.findAll('button').find((b) => b.text() === 'Connect')
  if (!button) throw new Error('no Connect button found in modal')
  return button
}

describe('DeviceConnectPrompt', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.mocked(startReporting).mockResolvedValue({
      deviceId: 1,
      wsPort: 9000,
      mmsAvailable: true,
      gooseAvailable: true,
    })
  })

  it('opens the category modal instead of starting the device when bypassCategoryModal is false', async () => {
    const wrapper = mountPrompt()

    ;(wrapper.vm as unknown as { connect: (...args: unknown[]) => Promise<void> }).connect(
      '10.0.0.5',
      102,
      'eth0',
      undefined,
      undefined,
      undefined,
      false,
    )
    await flushPromises()

    expect(startReporting).not.toHaveBeenCalled()
    expect(wrapper.find('[role="dialog"]').exists()).toBe(true)
  })

  it('starts the device immediately with the default categories when bypassCategoryModal is true', async () => {
    const wrapper = mountPrompt()

    await (wrapper.vm as unknown as { connect: (...args: unknown[]) => Promise<void> }).connect(
      '10.0.0.5',
      102,
      'eth0',
      undefined,
      undefined,
      undefined,
      true,
    )

    expect(startReporting).toHaveBeenCalledTimes(1)
    expect(vi.mocked(startReporting).mock.calls[0]?.[0]).toMatchObject({
      host: '10.0.0.5',
      lnCategories: ['CONTROL', 'OTHER'],
    })
    expect(wrapper.emitted('connected')).toBeTruthy()
  })

  it('starts the device with the categories chosen in the modal once confirmed', async () => {
    const wrapper = mountPrompt()

    ;(wrapper.vm as unknown as { connect: (...args: unknown[]) => Promise<void> }).connect(
      '10.0.0.5',
      102,
      'eth0',
      undefined,
      undefined,
      undefined,
      false,
    )
    await flushPromises()

    await connectButtonInModal(wrapper).trigger('click')
    await flushPromises()

    expect(startReporting).toHaveBeenCalledTimes(1)
    expect(vi.mocked(startReporting).mock.calls[0]?.[0]).toMatchObject({
      host: '10.0.0.5',
      lnCategories: ['CONTROL', 'OTHER'],
    })
    expect(wrapper.emitted('connected')).toBeTruthy()
  })

  it('preserves the chosen categories across a password retry without reopening the modal', async () => {
    vi.mocked(startReporting)
      .mockRejectedValueOnce(new ApiError({ code: 'AUTH_REQUIRED', message: 'auth required', stage: null, detail: null }, 401))
      .mockResolvedValueOnce({ deviceId: 1, wsPort: 9000, mmsAvailable: true, gooseAvailable: true })
    const wrapper = mountPrompt()

    // Bypass to keep this test focused on the password-retry path, not the category modal.
    await (wrapper.vm as unknown as { connect: (...args: unknown[]) => Promise<void> }).connect(
      '10.0.0.5',
      102,
      'eth0',
      undefined,
      undefined,
      undefined,
      true,
    )

    // First call failed with AUTH_REQUIRED - the inline password form should now be showing.
    expect(wrapper.find('#acseAuthPassword').exists()).toBe(true)
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)

    await wrapper.find('#acseAuthPassword').setValue('secret')
    await wrapper.find('form').trigger('submit.prevent')
    await flushPromises()

    expect(startReporting).toHaveBeenCalledTimes(2)
    expect(vi.mocked(startReporting).mock.calls[1]?.[0]).toMatchObject({
      acseAuthPassword: 'secret',
      lnCategories: ['CONTROL', 'OTHER'],
    })
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
    expect(wrapper.emitted('connected')).toBeTruthy()
  })
})
