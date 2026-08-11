<script setup lang="ts">
import { Keyboard } from '@lucide/vue'

import { useI18n } from '@/i18n'

// One entry per available modifier shortcut: the key to render as a <kbd> chip and what holding
// it does. Exists because the Shift-connect shortcut shipped with no UI affordance at all — the
// only way to discover it was to read the source.
defineProps<{
  items: { key: string; text: string }[]
}>()

const { t } = useI18n()
</script>

<template>
  <p class="flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-slate-400 dark:text-slate-500">
    <span class="flex items-center gap-1 font-medium">
      <Keyboard :size="12" />
      {{ t('shortcuts.label') }}
    </span>
    <span v-for="item in items" :key="item.key" class="flex items-center gap-1">
      <kbd
        class="rounded border border-slate-300 bg-slate-50 px-1 py-px font-mono text-[10px] font-medium text-slate-600 dark:border-slate-700 dark:bg-slate-800 dark:text-slate-300"
      >
        {{ item.key }}
      </kbd>
      <span aria-hidden="true">+</span>
      <span>{{ t('shortcuts.click') }}</span>
      <span>— {{ item.text }}</span>
    </span>
  </p>
</template>
