import { describe, it, expect, vi } from 'vitest'
import { createRouter, createWebHistory } from 'vue-router'
import { createPinia } from 'pinia'

vi.mock('@/services/deviceApi', () => ({
  startReporting: vi.fn(),
  stopReporting: vi.fn(),
  listDevices: vi.fn().mockResolvedValue([]),
}))

import { mount } from '@vue/test-utils'
import App from '../App.vue'

describe('App', () => {
  it('mounts and renders the router view', async () => {
    const router = createRouter({
      history: createWebHistory(),
      routes: [
        { path: '/', name: 'scan', component: { template: '<div>scan page</div>' } },
        { path: '/devices', name: 'devices', component: { template: '<div>devices page</div>' } },
        { path: '/reports', name: 'reports', component: { template: '<div>reports page</div>' } },
      ],
    })
    router.push('/')
    await router.isReady()

    const wrapper = mount(App, {
      global: { plugins: [createPinia(), router] },
    })

    expect(wrapper.text()).toContain('scan page')
    expect(wrapper.text()).toContain('Reports')
  })
})
