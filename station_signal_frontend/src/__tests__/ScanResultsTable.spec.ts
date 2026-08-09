import { describe, it, expect } from 'vitest'
import { mount } from '@vue/test-utils'
import ScanResultsTable from '@/components/scan/ScanResultsTable.vue'
import type { ScanResultMessage } from '@/types/api'

function result(host: string, discoveredAtMs: number): ScanResultMessage {
  return { schemaVersion: 1, type: 'SCAN_RESULT', scanId: 1, host, mmsPort: 102, discoveredAtMs }
}

describe('ScanResultsTable', () => {
  it('renders the most recently discovered host first', () => {
    const wrapper = mount(ScanResultsTable, {
      props: {
        results: [result('10.0.0.1', 100), result('10.0.0.2', 200), result('10.0.0.3', 300)],
      },
    })

    const hosts = wrapper.findAll('tbody tr td:first-child').map((td) => td.text())
    expect(hosts).toEqual(['10.0.0.3', '10.0.0.2', '10.0.0.1'])
  })

  it('emits connect with bypassCategoryModal=false on a plain click', async () => {
    const wrapper = mount(ScanResultsTable, { props: { results: [result('10.0.0.1', 100)] } })

    await wrapper.find('tbody tr button').trigger('click')

    expect(wrapper.emitted('connect')).toEqual([['10.0.0.1', 102, false]])
  })

  it('emits connect with bypassCategoryModal=true when Shift is held', async () => {
    const wrapper = mount(ScanResultsTable, { props: { results: [result('10.0.0.1', 100)] } })

    await wrapper.find('tbody tr button').trigger('click', { shiftKey: true })

    expect(wrapper.emitted('connect')).toEqual([['10.0.0.1', 102, true]])
  })
})
