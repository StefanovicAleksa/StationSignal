<script setup lang="ts">
import { computed, ref, watch } from 'vue'

import { DEFAULT_LN_CATEGORIES, type LnCategory } from '@/types/api'
import Button from '@/components/ui/Button.vue'

const props = defineProps<{
  open: boolean
}>()

const emit = defineEmits<{
  confirm: [categories: LnCategory[] | undefined]
  cancel: []
}>()

interface CategoryOption {
  value: LnCategory
  label: string
  hint: string
}

const categoryOptions: CategoryOption[] = [
  { value: 'CONTROL', label: 'Control', hint: 'Breakers, switches, supervisory control' },
  { value: 'MEASUREMENT', label: 'Measurement', hint: 'Analog and metering values' },
  { value: 'PROTECTION', label: 'Protection', hint: 'Protection relay functions' },
  { value: 'OTHER', label: 'Other', hint: 'System logical nodes and anything uncategorized' },
]

const allSelected = ref(false)
const selected = ref<Set<LnCategory>>(new Set(DEFAULT_LN_CATEGORIES))

function resetToDefault() {
  allSelected.value = false
  selected.value = new Set(DEFAULT_LN_CATEGORIES)
}

// Reset every time the modal transitions from closed to open, so a stale selection from a
// previous connect attempt (confirmed or cancelled) never leaks into the next one.
watch(
  () => props.open,
  (isOpen) => {
    if (isOpen) resetToDefault()
  },
)

function toggleCategory(value: LnCategory) {
  const next = new Set(selected.value)
  if (next.has(value)) next.delete(value)
  else next.add(value)
  selected.value = next
}

const canConnect = computed(() => allSelected.value || selected.value.size > 0)

function handleConfirm() {
  if (!canConnect.value) return
  emit('confirm', allSelected.value ? undefined : Array.from(selected.value))
}

function handleCancel() {
  emit('cancel')
}
</script>

<template>
  <Teleport to="body">
    <div
      v-if="open"
      class="fixed inset-0 z-50 flex items-center justify-center bg-slate-900/50 p-4"
      @click.self="handleCancel"
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-labelledby="connectCategoryModalTitle"
        class="w-full max-w-md rounded-lg border border-slate-300 bg-white p-4 shadow-lg dark:border-slate-800 dark:bg-slate-900"
      >
        <header class="mb-3">
          <h2 id="connectCategoryModalTitle" class="text-base font-semibold text-slate-900 dark:text-slate-50">
            What do you want to connect to?
          </h2>
          <p class="mt-1 text-sm text-slate-500 dark:text-slate-400">
            Choose which Logical Node categories to subscribe to on this device.
          </p>
        </header>

        <label
          class="mb-3 flex items-center gap-2 rounded-md border border-slate-200 bg-slate-50 px-3 py-2 text-sm dark:border-slate-700 dark:bg-slate-800/60"
        >
          <input
            v-model="allSelected"
            type="checkbox"
            class="h-4 w-4 rounded border-slate-300 text-blue-600 focus:ring-blue-500 dark:border-slate-600"
          />
          <span class="font-medium text-slate-700 dark:text-slate-200">Connect to all categories</span>
        </label>

        <div class="flex flex-col gap-2">
          <label
            v-for="option in categoryOptions"
            :key="option.value"
            class="flex items-start gap-2 rounded-md border border-slate-200 px-3 py-2 text-sm dark:border-slate-700"
            :class="allSelected ? 'opacity-50' : ''"
          >
            <input
              type="checkbox"
              :checked="selected.has(option.value)"
              :disabled="allSelected"
              class="mt-0.5 h-4 w-4 rounded border-slate-300 text-blue-600 focus:ring-blue-500 disabled:cursor-not-allowed dark:border-slate-600"
              @change="toggleCategory(option.value)"
            />
            <span class="flex flex-col">
              <span class="font-medium text-slate-700 dark:text-slate-200">{{ option.label }}</span>
              <span class="text-xs text-slate-500 dark:text-slate-400">{{ option.hint }}</span>
            </span>
          </label>
        </div>

        <p v-if="!canConnect" class="mt-3 text-xs text-red-600 dark:text-red-400">
          Select at least one category, or choose "Connect to all categories".
        </p>

        <div class="mt-4 flex justify-end gap-2">
          <Button type="button" variant="secondary" @click="handleCancel">Cancel</Button>
          <Button type="button" variant="primary" :disabled="!canConnect" @click="handleConfirm">Connect</Button>
        </div>
      </div>
    </div>
  </Teleport>
</template>
