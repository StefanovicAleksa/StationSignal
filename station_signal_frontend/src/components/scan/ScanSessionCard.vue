<script setup lang="ts">
import { computed } from 'vue'
import { Square } from '@lucide/vue'

import type { ScanSession } from '@/stores/scan'
import ScanStatusBanner from './ScanStatusBanner.vue'
import ScanResultsTable from './ScanResultsTable.vue'
import Panel from '@/components/ui/Panel.vue'
import Button from '@/components/ui/Button.vue'

const props = defineProps<{
  session: ScanSession
}>()

const emit = defineEmits<{
  stop: []
  retryNow: []
  connect: [host: string, mmsPort: number, interfaceId: string, sessionKey: string, bypassCategoryModal: boolean]
}>()

const isBusy = computed(() => props.session.phase === 'starting' || props.session.phase === 'stopping')

function handleConnect(host: string, mmsPort: number, bypassCategoryModal: boolean) {
  emit('connect', host, mmsPort, props.session.interfaceId, props.session.key, bypassCategoryModal)
}
</script>

<template>
  <Panel class="flex flex-col gap-3">
    <header class="flex items-center justify-between">
      <div>
        <h2 class="text-sm font-semibold text-slate-800 dark:text-slate-100">
          {{ session.interfaceId }}
          <span class="font-normal text-slate-400 dark:text-slate-500">· port {{ session.mmsPort }}</span>
        </h2>
        <p v-if="session.scanId !== null" class="text-xs text-slate-400 dark:text-slate-500">
          Scan #{{ session.scanId }}
        </p>
      </div>
      <Button
        v-if="session.scanId !== null"
        variant="danger"
        size="sm"
        :icon="Square"
        :disabled="isBusy"
        @click="emit('stop')"
      >
        Stop
      </Button>
    </header>

    <ScanStatusBanner
      :phase="session.phase"
      :error="session.error"
      :connection-interrupted="session.connectionInterrupted"
      :next-retry-at="session.nextRetryAt"
      @retry-now="emit('retryNow')"
    />

    <ScanResultsTable :results="session.results" @connect="handleConnect" />
  </Panel>
</template>
