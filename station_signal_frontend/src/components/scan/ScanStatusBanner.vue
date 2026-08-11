<script setup lang="ts">
import { computed } from 'vue'
import { RefreshCw } from '@lucide/vue'
import type { ScanError, ScanPhase } from '@/stores/scan'
import Button from '@/components/ui/Button.vue'
import { useI18n } from '@/i18n'

const props = defineProps<{
  phase: ScanPhase
  error: ScanError | null
  connectionInterrupted: boolean
  nextRetryAt: number | null
}>()

const emit = defineEmits<{
  retryNow: []
}>()

const { t } = useI18n()

const retrySecondsLeft = computed(() => {
  if (!props.nextRetryAt) return 0
  return Math.max(0, Math.ceil((props.nextRetryAt - Date.now()) / 1000))
})

const statusText = computed(() => {
  switch (props.phase) {
    case 'starting':
      return t('scan.status.starting')
    case 'active':
      return t('scan.status.active')
    case 'stopping':
      return t('scan.status.stopping')
    default:
      return null
  }
})
</script>

<template>
  <div class="flex flex-col gap-2">
    <p v-if="statusText" class="text-sm text-slate-600 dark:text-slate-400">{{ statusText }}</p>

    <div
      v-if="error"
      class="rounded-md border px-3 py-2 text-sm"
      :class="
        error.retryable
          ? 'border-amber-300 bg-amber-50 text-amber-800 dark:border-amber-800 dark:bg-amber-900/30 dark:text-amber-300'
          : 'border-red-300 bg-red-50 text-red-800 dark:border-red-800 dark:bg-red-900/30 dark:text-red-300'
      "
    >
      <p>{{ error.message }} ({{ error.code }})</p>
      <div v-if="error.retryable" class="mt-1 flex items-center gap-2">
        <span v-if="nextRetryAt">{{ t('scan.status.retryingIn', { seconds: retrySecondsLeft }) }}</span>
        <Button variant="ghost" size="sm" :icon="RefreshCw" @click="emit('retryNow')">{{ t('common.retryNow') }}</Button>
      </div>
    </div>

    <p v-if="connectionInterrupted" class="text-xs text-amber-700 dark:text-amber-400">
      {{ t('scan.status.interrupted') }}
    </p>
  </div>
</template>
