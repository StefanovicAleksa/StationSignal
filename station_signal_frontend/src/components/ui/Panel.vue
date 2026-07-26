<script setup lang="ts">
withDefaults(
  defineProps<{
    padded?: boolean
  }>(),
  { padded: true },
)
</script>

<template>
  <section
    class="rounded-lg border border-slate-300 bg-white dark:border-slate-800 dark:bg-slate-900"
    :class="padded ? 'p-4' : ''"
  >
    <header v-if="$slots.header" class="mb-3 flex items-center justify-between gap-2">
      <slot name="header" />
    </header>
    <slot />
  </section>
</template>
