import { describe, it, expect, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { setActivePinia, createPinia } from 'pinia'

import ScanResultsTable from '@/components/scan/ScanResultsTable.vue'
import { useDevicesStore, type DevicePhase, type WatchedDevice } from '@/stores/devices'
import type { ScanResultMessage } from '@/types/api'
import { t } from '@/i18n'

function result(host: string, discoveredAtMs: number): ScanResultMessage {
  return { schemaVersion: 1, type: 'SCAN_RESULT', scanId: 1, host, mmsPort: 102, discoveredAtMs }
}

// Puts a device into the store at 10.0.0.1:102 so the matching scan row can derive its state.
function watchDevice(phase: DevicePhase, deviceId: number | null = 7) {
  const store = useDevicesStore()
  const device: WatchedDevice = {
    key: 'device-0',
    deviceId,
    host: '10.0.0.1',
    mmsPort: 102,
    interfaceId: 'eth0',
    phase,
    error: null,
    reports: [],
    lastMessageAtMs: null,
    mmsAvailable: true,
    gooseAvailable: true,
    requestedCategories: undefined,
    effectiveCategories: undefined,
    sharedWithDifferentFilter: false,
  }
  store.devices['device-0'] = device
}

function mountTable(results: ScanResultMessage[]) {
  return mount(ScanResultsTable, { props: { results } })
}

describe('ScanResultsTable', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('renders the most recently discovered host first', () => {
    const wrapper = mountTable([result('10.0.0.1', 100), result('10.0.0.2', 200), result('10.0.0.3', 300)])

    const hosts = wrapper.findAll('tbody tr td:first-child').map((td) => td.text())
    expect(hosts).toEqual(['10.0.0.3', '10.0.0.2', '10.0.0.1'])
  })

  describe('modifier presets', () => {
    it('emits the ask preset on a plain click', async () => {
      const wrapper = mountTable([result('10.0.0.1', 100)])

      await wrapper.find('tbody tr button').trigger('click')

      expect(wrapper.emitted('connect')).toEqual([['10.0.0.1', 102, 'ask']])
    })

    it('emits the default preset when Shift is held', async () => {
      const wrapper = mountTable([result('10.0.0.1', 100)])

      await wrapper.find('tbody tr button').trigger('click', { shiftKey: true })

      expect(wrapper.emitted('connect')).toEqual([['10.0.0.1', 102, 'default']])
    })

    it('emits the all preset when Ctrl is held', async () => {
      const wrapper = mountTable([result('10.0.0.1', 100)])

      await wrapper.find('tbody tr button').trigger('click', { ctrlKey: true })

      expect(wrapper.emitted('connect')).toEqual([['10.0.0.1', 102, 'all']])
    })

    it('emits the all preset when Cmd is held, for macOS', async () => {
      const wrapper = mountTable([result('10.0.0.1', 100)])

      await wrapper.find('tbody tr button').trigger('click', { metaKey: true })

      expect(wrapper.emitted('connect')).toEqual([['10.0.0.1', 102, 'all']])
    })
  })

  // Rows are a pure function of the devices store rather than being deleted on connect, so a
  // stop puts the Connect button back with no explicit re-add path — see the scan store spec's
  // own note on why the previous delete-on-connect approach lost the row permanently.
  describe('row state derived from the devices store', () => {
    it('offers Connect when nothing is watching that host', () => {
      const wrapper = mountTable([result('10.0.0.1', 100)])

      expect(wrapper.find('tbody tr button').text()).toBe(t('common.connect'))
      expect(wrapper.find('tbody tr span.rounded').exists()).toBe(false)
    })

    it('offers View plus a phase badge while the device is connecting', () => {
      watchDevice('connecting')
      const wrapper = mountTable([result('10.0.0.1', 100)])

      expect(wrapper.find('tbody tr button').text()).toBe(t('common.view'))
      expect(wrapper.find('tbody tr').text()).toContain(t('devices.phase.connecting'))
    })

    it('offers View once connected', () => {
      watchDevice('connected')
      const wrapper = mountTable([result('10.0.0.1', 100)])

      expect(wrapper.find('tbody tr button').text()).toBe(t('common.view'))
      expect(wrapper.find('tbody tr').text()).toContain(t('devices.phase.connected'))
    })

    it('emits view rather than connect for a watched host', async () => {
      watchDevice('connected')
      const wrapper = mountTable([result('10.0.0.1', 100)])

      await wrapper.find('tbody tr button').trigger('click')

      expect(wrapper.emitted('view')).toEqual([['10.0.0.1', 102]])
      expect(wrapper.emitted('connect')).toBeUndefined()
    })

    it('offers no action while the device is stopping', () => {
      watchDevice('stopping')
      const wrapper = mountTable([result('10.0.0.1', 100)])

      expect(wrapper.find('tbody tr button').exists()).toBe(false)
      expect(wrapper.find('tbody tr').text()).toContain(t('devices.phase.stopping'))
    })

    // An errored device isn't an active connection, so the row goes back to offering Connect and
    // the attempt can be retried in place — matching the devices store's own retry rule.
    it('offers Connect again for an errored device', () => {
      watchDevice('error')
      const wrapper = mountTable([result('10.0.0.1', 100)])

      expect(wrapper.find('tbody tr button').text()).toBe(t('common.connect'))
    })

    it('leaves other hosts unaffected', () => {
      watchDevice('connected')
      const wrapper = mountTable([result('10.0.0.1', 100), result('10.0.0.2', 200)])

      const buttons = wrapper.findAll('tbody tr button').map((button) => button.text())
      // Newest first: 10.0.0.2 (unwatched) then 10.0.0.1 (watched).
      expect(buttons).toEqual([t('common.connect'), t('common.view')])
    })
  })
})
