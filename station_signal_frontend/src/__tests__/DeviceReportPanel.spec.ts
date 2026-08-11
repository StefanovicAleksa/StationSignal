import { describe, it, expect, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { setActivePinia, createPinia } from 'pinia'

import DeviceReportPanel from '@/components/device/DeviceReportPanel.vue'
import type { DeviceReport, WatchedDevice } from '@/stores/devices'
import type { LnCategory } from '@/types/api'
import { t } from '@/i18n'

let nextReportId = 0

function report(reference: string, category: LnCategory | null): DeviceReport {
  return {
    id: nextReportId++,
    reportType: 'MMS_REPORT',
    source: { rcbReference: 'LD0/LLN0$BR$brcb01', buffered: true },
    receivedAtMs: 1_000,
    reference,
    value: 1,
    previousValue: 0,
    valueChanged: true,
    quality: null,
    previousQuality: null,
    qualityChanged: false,
    label: null,
    previousLabel: null,
    category,
  }
}

function device(overrides: Partial<WatchedDevice> = {}): WatchedDevice {
  return {
    key: 'device-0',
    deviceId: 1,
    host: '10.0.0.5',
    mmsPort: 102,
    interfaceId: 'eth0',
    phase: 'connected',
    error: null,
    reports: [],
    lastMessageAtMs: null,
    mmsAvailable: true,
    gooseAvailable: true,
    requestedCategories: undefined,
    effectiveCategories: undefined,
    sharedWithDifferentFilter: false,
    ...overrides,
  }
}

function mountPanel(overrides: Partial<WatchedDevice> = {}) {
  return mount(DeviceReportPanel, { props: { device: device(overrides) } })
}

function categoryChip(wrapper: ReturnType<typeof mountPanel>, label: string) {
  const chip = wrapper.findAll('button[aria-pressed]').find((button) => button.text() === label)
  if (!chip) throw new Error(`no category chip found for ${label}`)
  return chip
}

function renderedReferences(wrapper: ReturnType<typeof mountPanel>) {
  return wrapper.findAll('tbody tr td:nth-child(3) span:first-child').map((span) => span.text())
}

describe('DeviceReportPanel category filter', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    nextReportId = 0
  })

  it('shows every category by default', () => {
    const wrapper = mountPanel({
      reports: [report('LD0/CSWI1$ST$Pos$stVal', 'CONTROL'), report('LD0/MMXU1$MX$A$phsA', 'MEASUREMENT')],
    })

    expect(renderedReferences(wrapper)).toEqual(['LD0/MMXU1$MX$A$phsA', 'LD0/CSWI1$ST$Pos$stVal'])
  })

  it('hides a category when its chip is toggled off, and restores it when toggled back on', async () => {
    const wrapper = mountPanel({
      reports: [report('LD0/CSWI1$ST$Pos$stVal', 'CONTROL'), report('LD0/MMXU1$MX$A$phsA', 'MEASUREMENT')],
    })

    await categoryChip(wrapper, t('categoryModal.measurement')).trigger('click')
    expect(renderedReferences(wrapper)).toEqual(['LD0/CSWI1$ST$Pos$stVal'])

    await categoryChip(wrapper, t('categoryModal.measurement')).trigger('click')
    expect(renderedReferences(wrapper)).toHaveLength(2)
  })

  // Hiding a point purely because the daemon couldn't resolve its owning LN would silently drop
  // real data, so an unknown category is never filtered out.
  it('always shows a point whose category is unknown', async () => {
    const wrapper = mountPanel({
      reports: [report('LD0/CSWI1$ST$Pos$stVal', 'CONTROL'), report('LD0/UNKNOWN$ST$x', null)],
    })

    await categoryChip(wrapper, t('categoryModal.control')).trigger('click')

    expect(renderedReferences(wrapper)).toEqual(['LD0/UNKNOWN$ST$x'])
  })

  // "Nothing has arrived yet" and "everything that arrived is filtered out" otherwise look
  // identical, which makes the filter look broken.
  it('distinguishes an empty stream from a fully-filtered one', async () => {
    const empty = mountPanel({ reports: [] })
    expect(empty.find('tbody tr td').text()).toBe(t('reports.table.empty'))

    const filtered = mountPanel({ reports: [report('LD0/CSWI1$ST$Pos$stVal', 'CONTROL')] })
    await categoryChip(filtered, t('categoryModal.control')).trigger('click')
    expect(filtered.find('tbody tr td').text()).toBe(t('reports.table.emptyForFilter'))
  })
})

describe('DeviceReportPanel shared-device banner', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    nextReportId = 0
  })

  it('stays hidden when this session started the device itself', () => {
    const wrapper = mountPanel({ requestedCategories: ['CONTROL'], effectiveCategories: ['CONTROL'] })

    expect(wrapper.text()).not.toContain(t('reports.shared.banner', { categories: '' }).slice(0, 40))
  })

  it("names the running device's own categories when this session was attached to it", () => {
    const wrapper = mountPanel({
      requestedCategories: ['MEASUREMENT'],
      effectiveCategories: ['CONTROL', 'OTHER'],
      sharedWithDifferentFilter: true,
    })

    const text = wrapper.text()
    expect(text).toContain(t('categoryModal.control'))
    expect(text).toContain(t('categoryModal.other'))
  })

  it('describes an unfiltered running device as all categories', () => {
    const wrapper = mountPanel({
      requestedCategories: ['CONTROL'],
      effectiveCategories: undefined,
      sharedWithDifferentFilter: true,
    })

    expect(wrapper.text()).toContain(t('reports.shared.allCategories'))
  })
})
