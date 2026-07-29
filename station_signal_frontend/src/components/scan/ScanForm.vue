<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { Play } from '@lucide/vue'
import Button from '@/components/ui/Button.vue'
import { useSettingsStore } from '@/stores/settings'

const props = withDefaults(
  defineProps<{
    disabled?: boolean
  }>(),
  { disabled: false },
)

const emit = defineEmits<{
  start: [interfaceId: string, mmsPort: number]
}>()

// Defaults to the box's own active network interface (from the Settings page's network status)
// rather than a hardcoded name — a technician almost always wants to scan on the interface the
// box is actually configured on, and a fixed default here previously leaked a dev machine's own
// interface name into the shipped app.
const settingsStore = useSettingsStore()

const interfaceId = ref('')
const mmsPortInput = ref('102')
const touched = ref(false)

onMounted(async () => {
  if (!settingsStore.status) {
    await settingsStore.loadStatus()
  }
  if (!interfaceId.value && settingsStore.status?.interface) {
    interfaceId.value = settingsStore.status.interface
  }
})

const interfaceError = computed(() => {
  if (!touched.value) return null
  return interfaceId.value.trim().length === 0 ? 'Interface is required.' : null
})

const portError = computed(() => {
  if (!touched.value) return null
  const value = Number(mmsPortInput.value)
  if (!Number.isInteger(value) || value < 1 || value > 65535) {
    return 'Port must be an integer between 1 and 65535.'
  }
  return null
})

const isValid = computed(() => {
  const value = Number(mmsPortInput.value)
  return (
    interfaceId.value.trim().length > 0 &&
    Number.isInteger(value) &&
    value >= 1 &&
    value <= 65535
  )
})

function handleSubmit() {
  touched.value = true
  if (!isValid.value) return
  emit('start', interfaceId.value.trim(), Number(mmsPortInput.value))
  interfaceId.value = settingsStore.status?.interface ?? ''
  touched.value = false
}
</script>

<template>
  <form class="flex flex-wrap items-end gap-4" @submit.prevent="handleSubmit">
    <div class="flex min-w-40 flex-1 flex-col gap-1">
      <label for="interfaceId" class="text-sm font-medium text-slate-700 dark:text-slate-300">Interface</label>
      <input
        id="interfaceId"
        v-model="interfaceId"
        type="text"
        placeholder="eth0"
        :disabled="props.disabled"
        class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
      <p v-if="interfaceError" class="text-xs text-red-600 dark:text-red-400">{{ interfaceError }}</p>
    </div>

    <div class="flex min-w-32 flex-1 flex-col gap-1">
      <label for="mmsPort" class="text-sm font-medium text-slate-700 dark:text-slate-300">MMS Port</label>
      <input
        id="mmsPort"
        v-model="mmsPortInput"
        type="number"
        min="1"
        max="65535"
        step="1"
        :disabled="props.disabled"
        class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
      <p v-if="portError" class="text-xs text-red-600 dark:text-red-400">{{ portError }}</p>
    </div>

    <Button type="submit" variant="primary" :icon="Play" :disabled="props.disabled">Start Scan</Button>
  </form>
</template>
