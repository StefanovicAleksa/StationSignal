import { describe, it, expect, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { setActivePinia, createPinia } from 'pinia'
import { createRouter, createWebHistory } from 'vue-router'

import ReportsView from '@/views/ReportsView.vue'
import { useDevicesStore, type WatchedDevice } from '@/stores/devices'

function device(overrides: Partial<WatchedDevice> & Pick<WatchedDevice, 'key' | 'host'>): WatchedDevice {
  return {
    deviceId: null,
    mmsPort: 102,
    interfaceId: 'eth0',
    phase: 'connected',
    error: null,
    reports: [],
    lastMessageAtMs: null,
    ...overrides,
  }
}

async function mountReports(initialPath = '/reports') {
  const router = createRouter({
    history: createWebHistory(),
    routes: [
      { path: '/', name: 'scan', component: { template: '<div />' } },
      { path: '/devices', name: 'devices', component: { template: '<div />' } },
      { path: '/reports', name: 'reports', component: ReportsView },
    ],
  })
  router.push(initialPath)
  await router.isReady()

  const wrapper = mount(ReportsView, {
    global: { plugins: [router] },
  })
  await flushPromises()
  return { wrapper, router }
}

describe('ReportsView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('shows an empty state when no devices are watched', async () => {
    const { wrapper } = await mountReports()
    expect(wrapper.text()).toContain('No devices being watched.')
  })

  it('renders one tab per watched device and defaults to the most recently added', async () => {
    const store = useDevicesStore()
    store.devices['device-0'] = device({ key: 'device-0', host: '10.0.0.1', deviceId: 1 })
    store.devices['device-1'] = device({ key: 'device-1', host: '10.0.0.2', deviceId: 2 })

    const { wrapper } = await mountReports()

    const tabLabels = wrapper.findAll('button').map((b) => b.text())
    expect(tabLabels.some((t) => t.includes('10.0.0.1'))).toBe(true)
    expect(tabLabels.some((t) => t.includes('10.0.0.2'))).toBe(true)
    expect(wrapper.text()).toContain('10.0.0.2:102')
  })

  it('selects the tab matching the ?device= deep link', async () => {
    const store = useDevicesStore()
    store.devices['device-0'] = device({ key: 'device-0', host: '10.0.0.1', deviceId: 1 })
    store.devices['device-1'] = device({ key: 'device-1', host: '10.0.0.2', deviceId: 2 })

    const { wrapper } = await mountReports('/reports?device=1')

    expect(wrapper.text()).toContain('10.0.0.1:102')
  })

  it('does not remount panels when switching tabs (v-show, not v-if)', async () => {
    const store = useDevicesStore()
    store.devices['device-0'] = device({ key: 'device-0', host: '10.0.0.1', deviceId: 1 })
    store.devices['device-1'] = device({ key: 'device-1', host: '10.0.0.2', deviceId: 2 })

    const { wrapper } = await mountReports('/reports?device=1')

    const tabButtons = wrapper.findAll('button')
    const secondTab = tabButtons.find((b) => b.text().includes('10.0.0.2'))
    await secondTab?.trigger('click')
    await flushPromises()

    // both panels stay mounted in the DOM (one hidden via v-show) rather than being torn down
    expect(wrapper.html()).toContain('10.0.0.1:102')
    expect(wrapper.html()).toContain('10.0.0.2:102')
  })

  it('falls back to another tab when the active device stops being watched', async () => {
    const store = useDevicesStore()
    store.devices['device-0'] = device({ key: 'device-0', host: '10.0.0.1', deviceId: 1 })
    store.devices['device-1'] = device({ key: 'device-1', host: '10.0.0.2', deviceId: 2 })

    const { wrapper } = await mountReports('/reports?device=2')
    expect(wrapper.text()).toContain('10.0.0.2:102')

    delete store.devices['device-1']
    await flushPromises()

    expect(wrapper.text()).toContain('10.0.0.1:102')
  })

  it('falls back to the empty state when the last watched device stops being watched', async () => {
    const store = useDevicesStore()
    store.devices['device-0'] = device({ key: 'device-0', host: '10.0.0.1', deviceId: 1 })

    const { wrapper } = await mountReports('/reports?device=1')
    expect(wrapper.text()).toContain('10.0.0.1:102')

    delete store.devices['device-0']
    await flushPromises()

    expect(wrapper.text()).toContain('No devices being watched.')
  })
})
