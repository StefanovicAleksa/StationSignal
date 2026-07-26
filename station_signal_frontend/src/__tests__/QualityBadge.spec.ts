import { describe, it, expect } from 'vitest'
import { mount } from '@vue/test-utils'
import QualityBadge from '@/components/device/QualityBadge.vue'

describe('QualityBadge', () => {
  it('renders the validity label', () => {
    const wrapper = mount(QualityBadge, { props: { validity: 'GOOD' } })
    expect(wrapper.text()).toBe('GOOD')
  })

  it('renders a placeholder when validity is null', () => {
    const wrapper = mount(QualityBadge, { props: { validity: null } })
    expect(wrapper.text()).toBe('—')
  })
})
