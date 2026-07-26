import { ref } from 'vue'

const STORAGE_KEY = 'ied-reporter:theme'

const supportsMatchMedia = typeof window.matchMedia === 'function'
const darkMediaQuery = supportsMatchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null

function systemPrefersDark(): boolean {
  return darkMediaQuery?.matches ?? false
}

function readStoredTheme(): 'light' | 'dark' | null {
  const stored = localStorage.getItem(STORAGE_KEY)
  return stored === 'light' || stored === 'dark' ? stored : null
}

function applyTheme(dark: boolean) {
  document.documentElement.classList.toggle('dark', dark)
}

const theme = ref<'light' | 'dark'>(readStoredTheme() ?? (systemPrefersDark() ? 'dark' : 'light'))
applyTheme(theme.value === 'dark')

darkMediaQuery?.addEventListener('change', (event) => {
  if (readStoredTheme()) return
  theme.value = event.matches ? 'dark' : 'light'
  applyTheme(event.matches)
})

function toggle() {
  theme.value = theme.value === 'dark' ? 'light' : 'dark'
  localStorage.setItem(STORAGE_KEY, theme.value)
  applyTheme(theme.value === 'dark')
}

export function useTheme() {
  return { theme, toggle }
}
