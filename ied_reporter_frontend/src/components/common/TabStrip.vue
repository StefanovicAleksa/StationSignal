<script setup lang="ts">
export interface TabStripItem {
  id: string
  label: string
  meta?: string
}

defineProps<{
  tabs: TabStripItem[]
  activeId: string | null
}>()

const emit = defineEmits<{
  select: [id: string]
}>()
</script>

<template>
  <div class="flex flex-wrap gap-1 border-b border-slate-300 dark:border-slate-800">
    <button
      v-for="tab in tabs"
      :key="tab.id"
      type="button"
      class="flex flex-col items-start rounded-t-md border border-b-0 px-3 py-2 text-sm"
      :class="
        tab.id === activeId
          ? 'border-slate-300 bg-white font-medium text-slate-900 dark:border-slate-800 dark:bg-slate-900 dark:text-slate-100'
          : 'border-transparent text-slate-500 hover:text-slate-700 dark:text-slate-500 dark:hover:text-slate-300'
      "
      @click="emit('select', tab.id)"
    >
      <span class="font-mono">{{ tab.label }}</span>
      <span v-if="tab.meta" class="text-xs font-normal text-slate-400 dark:text-slate-500">{{ tab.meta }}</span>
    </button>
  </div>
</template>
