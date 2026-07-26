<script setup lang="ts">
import { onMounted } from 'vue'

import { useDevicesStore } from '@/stores/devices'
import AppShell from '@/layouts/AppShell.vue'

const devicesStore = useDevicesStore()

onMounted(() => {
  devicesStore.reconcileOnLoad()
})
</script>

<template>
  <AppShell>
    <RouterView />
  </AppShell>
</template>
