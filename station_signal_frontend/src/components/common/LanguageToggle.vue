<script setup lang="ts">
import { computed } from 'vue'
import { Languages } from '@lucide/vue'

import { useI18n } from '@/i18n'

const { locale, t, setLocale, LOCALES } = useI18n()

const current = computed(() => LOCALES.find((entry) => entry.value === locale.value) ?? LOCALES[0])

// Two locales, so "the other one" is unambiguous — a single toggle button, matching the theme
// toggle beside it, rather than a dropdown. Add a third locale and this needs to become a menu.
const next = computed(() => LOCALES.find((entry) => entry.value !== locale.value) ?? LOCALES[0])

const label = computed(() => t('language.switchTo', { language: next.value.label }))
</script>

<template>
  <button
    type="button"
    class="flex h-8 items-center gap-1 rounded-md px-1.5 text-slate-500 hover:bg-slate-100 hover:text-slate-900 dark:text-slate-400 dark:hover:bg-slate-800 dark:hover:text-slate-100"
    :aria-label="label"
    :title="label"
    @click="setLocale(next.value)"
  >
    <Languages :size="16" />
    <span class="text-xs font-semibold">{{ current.short }}</span>
  </button>
</template>
