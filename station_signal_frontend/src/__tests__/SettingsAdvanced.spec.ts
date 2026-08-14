import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'

vi.mock('@/services/settingsApi', () => ({
  getNetworkStatus: vi.fn(),
  applyNetworkConfig: vi.fn(),
  revertNetworkConfig: vi.fn(),
  probeReachability: vi.fn(),
  confirmNetworkConfigAt: vi.fn(),
  clearLogs: vi.fn(),
}))

import SettingsView from '@/views/SettingsView.vue'
import { getNetworkStatus, clearLogs } from '@/services/settingsApi'
import { ApiError } from '@/types/api'

/**
 * Covers the dev-only Advanced section of the Settings page — the "clear log files" control used
 * to start a test session with a clean capture.
 *
 * On visibility: Vitest always runs as DEV (utils/mode.ts ORs in import.meta.env.DEV), so these
 * assert the section IS present. The prod-hidden branch is a build-time concern and is not
 * assertable here — the same limitation logger.spec.ts documents for the logging gate — so it is
 * verified by building with VITE_STATION_SIGNAL_MODE=prod and grepping the bundle instead. The
 * API's own dev gate is what actually protects a prod box, and that has its own test
 * (rest/logs_test.go).
 */
function mountSettings() {
  return mount(SettingsView, {
    global: { stubs: { teleport: true } },
  })
}

// The section sits behind a Disclosure, so nothing inside it is interactive until that is opened.
// Returns the clear-logs button.
async function openAdvanced(wrapper: ReturnType<typeof mount>) {
  const toggles = wrapper.findAll('button')
  const advancedToggle = toggles.find((b) => b.text().includes('Advanced'))
  expect(advancedToggle, 'expected an "Advanced" disclosure toggle to be rendered').toBeTruthy()
  await advancedToggle!.trigger('click')

  const clearButton = wrapper.findAll('button').find((b) => b.text().includes('Clear log files'))
  expect(clearButton, 'expected the clear-logs button inside the opened Advanced section').toBeTruthy()
  return clearButton!
}

describe('SettingsView advanced section', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
    vi.mocked(getNetworkStatus).mockResolvedValue({
      interface: 'eth0',
      current: { cidr: '192.168.0.10/24' },
      recoveryAddress: '169.254.1.1',
    })
  })

  it('clears the logs and reports how many files and how much was freed', async () => {
    vi.mocked(clearLogs).mockResolvedValue({
      logDir: '/var/log/station_signal',
      clearedCount: 12,
      skippedCount: 0,
      bytesFreed: 14680064,
    })
    const wrapper = mountSettings()
    await flushPromises()

    const button = await openAdvanced(wrapper)
    await button.trigger('click')
    await flushPromises()

    expect(clearLogs).toHaveBeenCalledOnce()
    expect(wrapper.text()).toContain('12')
    // 14680064 bytes is 14 MB — the number an operator actually reads before walking out.
    expect(wrapper.text()).toContain('14 MB')
    expect(wrapper.text()).toContain('/var/log/station_signal')
  })

  // A log this stack cannot truncate (station-signal-api.log is created root-owned by systemd) is
  // information to show, not a failure to hide behind.
  it('says so when some files could not be cleared', async () => {
    vi.mocked(clearLogs).mockResolvedValue({
      logDir: '/var/log/station_signal',
      clearedCount: 11,
      skippedCount: 1,
      bytesFreed: 2048,
    })
    const wrapper = mountSettings()
    await flushPromises()

    const button = await openAdvanced(wrapper)
    await button.trigger('click')
    await flushPromises()

    expect(wrapper.text()).toContain('11')
    expect(wrapper.text()).toContain('could not be cleared')
  })

  it('surfaces the API error message when clearing fails', async () => {
    vi.mocked(clearLogs).mockRejectedValue(
      new ApiError({ code: 'INTERNAL', message: 'read-only filesystem', stage: null, detail: null }, 500),
    )
    const wrapper = mountSettings()
    await flushPromises()

    const button = await openAdvanced(wrapper)
    await button.trigger('click')
    await flushPromises()

    expect(wrapper.text()).toContain('read-only filesystem')
  })

  // The control is deliberately tucked away rather than sitting on the page: it is destructive and
  // only relevant right before a capture, so it must not be one stray click from a technician
  // reading their network settings.
  it('keeps the clear-logs button hidden until Advanced is opened', async () => {
    const wrapper = mountSettings()
    await flushPromises()

    const button = wrapper.findAll('button').find((b) => b.text().includes('Clear log files'))
    // Disclosure uses v-show, so the button exists in the DOM but must not be visible.
    expect(button?.isVisible() ?? false).toBe(false)
  })
})
