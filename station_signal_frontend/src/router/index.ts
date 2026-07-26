import { createRouter, createWebHistory } from 'vue-router'

const router = createRouter({
  history: createWebHistory(import.meta.env.BASE_URL),
  routes: [
    { path: '/', name: 'scan', component: () => import('../views/ScanView.vue') },
    { path: '/devices', name: 'devices', component: () => import('../views/DevicesView.vue') },
    { path: '/reports', name: 'reports', component: () => import('../views/ReportsView.vue') },
    {
      path: '/devices/:id',
      name: 'device-detail',
      redirect: (to) => ({ name: 'reports', query: { device: to.params.id } }),
    },
  ],
})

export default router
