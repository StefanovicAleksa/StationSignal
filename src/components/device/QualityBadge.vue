<script setup lang="ts">
import { computed } from 'vue'
import type { Quality } from '@/types/api'
import Badge from '@/components/ui/Badge.vue'

const props = defineProps<{
  validity: Quality['validity'] | null
}>()

const toneByValidity: Record<Quality['validity'], 'success' | 'error' | 'warning' | 'neutral'> = {
  GOOD: 'success',
  INVALID: 'error',
  QUESTIONABLE: 'warning',
  RESERVED: 'neutral',
}

const tone = computed(() => (props.validity ? toneByValidity[props.validity] : 'muted'))
const label = computed(() => props.validity ?? '—')
</script>

<template>
  <Badge :tone="tone">{{ label }}</Badge>
</template>
