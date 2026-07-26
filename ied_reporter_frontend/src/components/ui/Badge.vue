<script setup lang="ts">
import { computed } from 'vue'

const props = withDefaults(
  defineProps<{
    tone?: 'success' | 'error' | 'warning' | 'neutral' | 'muted'
  }>(),
  { tone: 'neutral' },
)

const toneClasses: Record<string, string> = {
  success: 'bg-emerald-100 text-emerald-800 dark:bg-emerald-900/40 dark:text-emerald-300',
  error: 'bg-red-100 text-red-800 dark:bg-red-900/40 dark:text-red-300',
  warning: 'bg-amber-100 text-amber-800 dark:bg-amber-900/40 dark:text-amber-300',
  neutral: 'bg-slate-200 text-slate-700 dark:bg-slate-800 dark:text-slate-300',
  muted: 'bg-slate-100 text-slate-400 dark:bg-slate-800/60 dark:text-slate-500',
}

const badgeClass = computed(() => toneClasses[props.tone])
</script>

<template>
  <span class="rounded px-1.5 py-0.5 text-xs font-medium" :class="badgeClass"><slot /></span>
</template>
