<script setup lang="ts">
import { ref, computed } from 'vue'
import { Play } from '@lucide/vue'
import Button from '@/components/ui/Button.vue'

const props = withDefaults(
  defineProps<{
    disabled?: boolean
  }>(),
  { disabled: false },
)

const emit = defineEmits<{
  start: [interfaceId: string, mmsPort: number]
}>()

const DEFAULT_INTERFACE_ID = 'enp34s0'

const interfaceId = ref(DEFAULT_INTERFACE_ID)
const mmsPortInput = ref('102')
const touched = ref(false)

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
  interfaceId.value = DEFAULT_INTERFACE_ID
  touched.value = false
}
</script>

<template>
  <form class="flex flex-wrap items-end gap-4" @submit.prevent="handleSubmit">
    <div class="flex flex-col gap-1">
      <label for="interfaceId" class="text-sm font-medium text-slate-700 dark:text-slate-300">Interface</label>
      <input
        id="interfaceId"
        v-model="interfaceId"
        type="text"
        placeholder="enp34s0"
        :disabled="props.disabled"
        class="w-40 rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
      <p v-if="interfaceError" class="text-xs text-red-600 dark:text-red-400">{{ interfaceError }}</p>
    </div>

    <div class="flex flex-col gap-1">
      <label for="mmsPort" class="text-sm font-medium text-slate-700 dark:text-slate-300">MMS Port</label>
      <input
        id="mmsPort"
        v-model="mmsPortInput"
        type="number"
        min="1"
        max="65535"
        step="1"
        :disabled="props.disabled"
        class="w-32 rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
      />
      <p v-if="portError" class="text-xs text-red-600 dark:text-red-400">{{ portError }}</p>
    </div>

    <Button type="submit" variant="primary" :icon="Play" :disabled="props.disabled">Start Scan</Button>
  </form>
</template>
