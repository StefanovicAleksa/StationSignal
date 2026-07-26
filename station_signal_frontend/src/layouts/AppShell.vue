<script setup lang="ts">
import { Radar, Server, FileText, Sun, Moon } from '@lucide/vue'

import { useTheme } from '@/composables/useTheme'

const { theme, toggle } = useTheme()

const navItems = [
  { to: { name: 'scan' }, label: 'Scan', icon: Radar },
  { to: { name: 'devices' }, label: 'Devices', icon: Server },
  { to: { name: 'reports' }, label: 'Reports', icon: FileText },
]
</script>

<template>
  <div class="min-h-screen bg-slate-100 text-slate-900 dark:bg-slate-950 dark:text-slate-100">
    <header
      class="sticky top-0 z-10 border-b border-slate-300 bg-white dark:border-slate-800 dark:bg-slate-900"
    >
      <div class="flex h-12 items-center gap-4 px-4">
        <span class="font-mono text-sm font-semibold tracking-tight text-slate-700 dark:text-slate-200">
          Station Signal
        </span>

        <nav class="flex items-stretch gap-1 border-l border-slate-300 pl-4 dark:border-slate-700">
          <RouterLink
            v-for="item in navItems"
            :key="item.label"
            :to="item.to"
            class="flex items-center gap-1.5 rounded-md px-3 py-1.5 text-sm font-medium text-slate-600 hover:bg-slate-100 hover:text-slate-900 dark:text-slate-400 dark:hover:bg-slate-800 dark:hover:text-slate-100"
            active-class="!bg-blue-600 !text-white dark:!bg-blue-600 dark:!text-white"
          >
            <component :is="item.icon" :size="15" />
            {{ item.label }}
          </RouterLink>
        </nav>

        <div class="ml-auto flex items-center">
          <button
            type="button"
            class="flex h-8 w-8 items-center justify-center rounded-md text-slate-500 hover:bg-slate-100 hover:text-slate-900 dark:text-slate-400 dark:hover:bg-slate-800 dark:hover:text-slate-100"
            :aria-label="theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'"
            @click="toggle"
          >
            <Sun v-if="theme === 'dark'" :size="16" />
            <Moon v-else :size="16" />
          </button>
        </div>
      </div>
    </header>

    <main class="px-4 py-4 sm:px-6 sm:py-6">
      <div class="mx-auto flex max-w-[1600px] flex-col gap-4">
        <slot />
      </div>
    </main>
  </div>
</template>
