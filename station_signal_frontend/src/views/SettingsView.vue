<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { AlertTriangle, Save, Trash2, Undo2 } from '@lucide/vue'

import { useSettingsStore } from '@/stores/settings'
import { useI18n } from '@/i18n'
import { FALLBACK_PREFIX, isValidIpv4, normalizeAddressInput, prefixOf } from '@/utils/network'
import { isDevMode } from '@/utils/mode'
import { clearLogs } from '@/services/settingsApi'
import { ApiError, type ClearLogsResult } from '@/types/api'
import Panel from '@/components/ui/Panel.vue'
import Button from '@/components/ui/Button.vue'
import Disclosure from '@/components/ui/Disclosure.vue'

const store = useSettingsStore()
const { t } = useI18n()

const cidrInput = ref('')
const gatewayInput = ref('')
const touched = ref(false)

onMounted(async () => {
  await store.loadStatus()
  // Default the gateway field to whatever's currently configured, so leaving it untouched while
  // only changing the address preserves the existing gateway instead of silently clearing it —
  // omitting the field is treated as "no gateway" by the API, which is correct for someone who
  // deliberately wants that, but wrong as an accidental default.
  if (store.status?.current.gateway) {
    gatewayInput.value = store.status.current.gateway
  }
})

onMounted(() => {
  clockTimer = setInterval(() => {
    now.value = Date.now()
  }, 500)
})

onUnmounted(() => {
  if (clockTimer) clearInterval(clockTimer)
  store.dispose()
})

// Dev-only log clearing. Kept as component-local state rather than in the settings store: it is
// a self-contained one-shot action with no bearing on the network-config state machine the store
// exists to run, and it does not survive leaving the page. Mirrors StructureFileUpload.vue.
const clearingLogs = ref(false)
const clearLogsResult = ref<ClearLogsResult | null>(null)
const clearLogsError = ref<string | null>(null)

// Bytes as something a human reads at a glance — the point of showing it at all is a quick "yes,
// that actually wiped 14 MB of last week" before walking out the door.
function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  const units = ['KB', 'MB', 'GB']
  let value = bytes / 1024
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit++
  }
  return `${value < 10 ? value.toFixed(1) : Math.round(value)} ${units[unit]}`
}

const clearLogsMessage = computed(() => {
  const result = clearLogsResult.value
  if (!result) return null
  if (result.clearedCount === 0 && result.skippedCount === 0) {
    return t('settings.advanced.clearedNothing')
  }
  const params = {
    count: String(result.clearedCount),
    size: formatBytes(result.bytesFreed),
    skipped: String(result.skippedCount),
  }
  return result.skippedCount > 0
    ? t('settings.advanced.clearedWithSkipped', params)
    : t('settings.advanced.cleared', params)
})

async function onClearLogs() {
  clearingLogs.value = true
  clearLogsError.value = null
  clearLogsResult.value = null
  try {
    clearLogsResult.value = await clearLogs()
  } catch (err) {
    clearLogsError.value =
      err instanceof ApiError ? err.message : t('settings.advanced.clearFailed')
  } finally {
    clearingLogs.value = false
  }
}

// The prefix a bare address inherits when the technician doesn't type one. Taken from the box's
// own current configuration rather than a hardcoded /24, so a /16 box doesn't silently get a
// wrong-sized subnet from the convenience.
const fallbackPrefix = computed(() => (store.status ? prefixOf(store.status.current.cidr) : null) ?? FALLBACK_PREFIX)

// What would actually be submitted — the full CIDR, prefix appended if it was left off.
const normalizedCidr = computed(() => normalizeAddressInput(cidrInput.value, fallbackPrefix.value))

const cidrError = computed(() => {
  if (!touched.value) return null
  return normalizedCidr.value ? null : t('settings.form.addressInvalid')
})

// Only shown once it resolves to something different from what was typed, i.e. exactly when the
// prefix was filled in on the technician's behalf — echoing back an unchanged value is noise.
const cidrPreview = computed(() => {
  const normalized = normalizedCidr.value
  return normalized && normalized !== cidrInput.value.trim() ? normalized : null
})

const gatewayError = computed(() => {
  if (!touched.value || !gatewayInput.value.trim()) return null
  return isValidIpv4(gatewayInput.value.trim()) ? null : t('settings.form.gatewayInvalid')
})

const isValid = computed(
  () => normalizedCidr.value !== null && (!gatewayInput.value.trim() || isValidIpv4(gatewayInput.value.trim())),
)

const isBusy = computed(
  () =>
    store.phase === 'applying' ||
    store.phase === 'waitingForReconnect' ||
    store.phase === 'confirming' ||
    store.phase === 'reverting',
)

// `Date.now()` is not reactive, so a computed that reads it only re-evaluates when some *other*
// dependency changes — here, only when the store schedules the next poll. The countdown therefore
// rendered the full delay once and then froze, never reaching 0, which made an actively-retrying
// reconnect look like a hung one. A ticking ref is the reactive clock it was missing.
const now = ref(Date.now())
let clockTimer: ReturnType<typeof setInterval> | null = null

const retrySecondsLeft = computed(() => {
  if (!store.nextPollAt) return 0
  return Math.max(0, Math.ceil((store.nextPollAt - now.value) / 1000))
})

function formatTime(at: number) {
  return new Date(at).toLocaleTimeString()
}

const lastProbe = computed(() => store.pollLog[store.pollLog.length - 1] ?? null)

const probeExplanation = computed<Record<string, string>>(() => ({
  ok: t('settings.attempts.ok'),
  'http-error': t('settings.attempts.httpError'),
  'blocked-by-cors': t('settings.attempts.blockedByCors'),
  unreachable: t('settings.attempts.unreachable'),
  timeout: t('settings.attempts.timeout'),
}))

const statusBannerClass = computed(() => {
  switch (store.phase) {
    case 'confirmed':
      return 'border-green-300 bg-green-50 text-green-800 dark:border-green-800 dark:bg-green-900/30 dark:text-green-300'
    case 'reverted':
    case 'error':
      return 'border-red-300 bg-red-50 text-red-800 dark:border-red-800 dark:bg-red-900/30 dark:text-red-300'
    default:
      return 'border-amber-300 bg-amber-50 text-amber-800 dark:border-amber-800 dark:bg-amber-900/30 dark:text-amber-300'
  }
})

function handleSubmit() {
  touched.value = true
  const cidr = normalizedCidr.value
  if (!isValid.value || !cidr) return
  // Always the normalized value — the API requires a complete CIDR, the optional prefix is a
  // browser-side affordance only.
  store.submit({ cidr, gateway: gatewayInput.value.trim() || undefined })
}

function prefillCurrent() {
  if (!store.status) return
  cidrInput.value = store.status.current.cidr
  gatewayInput.value = store.status.current.gateway ?? ''
  touched.value = false
}
</script>

<template>
  <div class="flex flex-col gap-6">
    <header>
      <h1 class="text-xl font-semibold text-slate-900 dark:text-slate-50">{{ t('settings.title') }}</h1>
      <p class="text-sm text-slate-500 dark:text-slate-400">{{ t('settings.subtitle') }}</p>
    </header>

    <Panel>
      <template #header>
        <h2 class="text-sm font-semibold text-slate-800 dark:text-slate-200">{{ t('settings.current.heading') }}</h2>
      </template>
      <p v-if="store.loadingStatus" class="text-sm text-slate-400 dark:text-slate-500">{{ t('common.loading') }}</p>
      <p v-else-if="store.statusError" class="text-sm text-red-600 dark:text-red-400">
        {{ store.statusError.message }} ({{ store.statusError.code }})
      </p>
      <dl v-else-if="store.status" class="grid grid-cols-[max-content_1fr] gap-x-4 gap-y-1 text-sm">
        <dt class="text-slate-500 dark:text-slate-400">{{ t('settings.current.interface') }}</dt>
        <dd class="font-mono text-slate-900 dark:text-slate-100">{{ store.status.interface }}</dd>
        <dt class="text-slate-500 dark:text-slate-400">{{ t('settings.current.address') }}</dt>
        <dd class="font-mono text-slate-900 dark:text-slate-100">{{ store.status.current.cidr }}</dd>
        <dt class="text-slate-500 dark:text-slate-400">{{ t('settings.current.gateway') }}</dt>
        <dd class="font-mono text-slate-900 dark:text-slate-100">
          {{ store.status.current.gateway ?? t('common.none') }}
        </dd>
      </dl>
    </Panel>

    <Panel>
      <template #header>
        <h2 class="text-sm font-semibold text-slate-800 dark:text-slate-200">{{ t('settings.form.heading') }}</h2>
      </template>

      <form class="flex flex-col gap-4" @submit.prevent="handleSubmit">
        <!-- Inputs and buttons share one row; every hint/error line lives in the row *below* it.
             Keeping them inside the input's own column made `items-end` align the buttons to the
             bottom of the hint text instead of the input, leaving them visibly low. -->
        <div class="flex flex-col gap-1">
          <div class="flex flex-wrap items-end gap-4">
            <div class="flex min-w-48 flex-1 flex-col gap-1">
              <label for="cidr" class="text-sm font-medium text-slate-700 dark:text-slate-300">
                {{ t('settings.form.addressLabel') }}
              </label>
              <input
                id="cidr"
                v-model="cidrInput"
                type="text"
                :placeholder="t('settings.form.addressPlaceholder')"
                :disabled="isBusy"
                class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
              />
            </div>

            <Button type="submit" variant="primary" :icon="Save" :disabled="isBusy">{{ t('common.apply') }}</Button>
            <Button type="button" variant="secondary" :disabled="isBusy || !store.status" @click="prefillCurrent">
              {{ t('settings.form.prefillCurrent') }}
            </Button>
          </div>

          <p v-if="cidrError" class="text-xs text-red-600 dark:text-red-400">{{ cidrError }}</p>
          <p v-else-if="cidrPreview" class="text-xs text-slate-500 dark:text-slate-400">
            {{ t('settings.form.addressPreview', { cidr: cidrPreview }) }}
          </p>
          <p v-else class="text-xs text-slate-400 dark:text-slate-500">
            {{ t('settings.form.addressHint', { prefix: `/${fallbackPrefix}` }) }}
          </p>
        </div>

        <!-- Gateway is prefilled from the box's current configuration on mount and almost never
             edited, so it starts collapsed — force-opened whenever it's invalid so an error can
             never hide behind the toggle. -->
        <Disclosure :label="t('settings.form.gatewayAdvanced')" :force-open="Boolean(gatewayError)">
          <div class="flex min-w-48 flex-1 flex-col gap-1">
            <label for="gateway" class="text-sm font-medium text-slate-700 dark:text-slate-300">
              {{ t('settings.form.gatewayLabel') }}
            </label>
            <input
              id="gateway"
              v-model="gatewayInput"
              type="text"
              :placeholder="t('settings.form.gatewayPlaceholder')"
              :disabled="isBusy"
              class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
            />
            <p v-if="gatewayError" class="text-xs text-red-600 dark:text-red-400">{{ gatewayError }}</p>
          </div>
        </Disclosure>
      </form>

      <p class="mt-3 text-xs text-slate-500 dark:text-slate-400">{{ t('settings.form.provisionalNote') }}</p>
    </Panel>

    <div v-if="store.phase !== 'idle'" class="rounded-md border px-3 py-3 text-sm" :class="statusBannerClass">
      <p v-if="store.phase === 'applying'">{{ t('settings.phase.applying') }}</p>

      <template v-else-if="store.phase === 'waitingForReconnect'">
        <p>
          {{ t('settings.phase.waitingBefore') }} <span class="font-mono">{{ store.newOrigin }}</span>
          {{ t('settings.phase.waitingOr') }}
          <a class="underline" href="http://stationsignal.local">stationsignal.local</a
          >{{ t('settings.phase.waitingAfter') }}
        </p>
        <div class="mt-1 flex items-center gap-2">
          <span v-if="store.nextPollAt">{{ t('settings.phase.checkingIn', { seconds: retrySecondsLeft }) }}</span>
          <Button variant="ghost" size="sm" @click="store.pollNow">{{ t('common.checkNow') }}</Button>
        </div>
        <p class="mt-2 text-xs">{{ t('settings.phase.autoRevertNote') }}</p>

        <!-- Each attempt and its outcome, because "still waiting" on its own can't distinguish a
             box that never came up from one that came up and refused this page. -->
        <div v-if="store.pollLog.length" class="mt-3 border-t border-amber-300/40 pt-2 dark:border-amber-800/60">
          <p class="text-xs font-semibold">{{ t('settings.attempts.heading') }}</p>
          <ul class="mt-1 space-y-0.5 font-mono text-xs">
            <li v-for="attempt in [...store.pollLog].reverse()" :key="attempt.at">
              {{ formatTime(attempt.at) }} — {{ attempt.outcome
              }}<template v-if="attempt.status"> ({{ attempt.status }})</template> · {{ attempt.durationMs }}ms<template
                v-if="attempt.error"
              >
                · {{ attempt.error }}</template
              >
            </li>
          </ul>
          <p v-if="lastProbe" class="mt-1 text-xs">{{ probeExplanation[lastProbe.outcome] }}</p>

        </div>
      </template>

      <p v-else-if="store.phase === 'confirming'">{{ t('settings.phase.confirming') }}</p>
      <p v-else-if="store.phase === 'confirmed'">
        {{ t('settings.phase.confirmedBefore') }} <span class="font-mono">{{ store.newOrigin }}</span
        >…
      </p>

      <p v-else-if="store.phase === 'reverting'">{{ t('settings.phase.reverting') }}</p>

      <template v-else-if="store.phase === 'reverted'">
        <p>{{ t('settings.phase.reverted') }}</p>
      </template>

      <p v-else-if="store.phase === 'error' && store.applyError">
        <template v-if="store.applyError.code === 'CHANGE_ALREADY_PENDING'">
          {{ t('settings.phase.alreadyPending') }}
        </template>
        <template v-else>{{ store.applyError.message }} ({{ store.applyError.code }})</template>
      </p>
    </div>

    <!-- A change the box refuses to let go of is the one failure mode that can't be waited out:
         until it's cleared, every apply is rejected. Surfacing it as its own panel with a direct
         action is what keeps it from needing shell access to the box to resolve. -->
    <Panel v-if="store.stuckPending">
      <template #header>
        <h2 class="flex items-center gap-1.5 text-sm font-semibold text-slate-800 dark:text-slate-200">
          <AlertTriangle :size="14" />
          {{ t('settings.pending.heading') }}
        </h2>
      </template>
      <p class="text-sm text-slate-600 dark:text-slate-400">
        <template v-if="store.status?.pending">
          {{ t('settings.pending.withAddressBefore') }}
          <span class="font-mono">{{ store.status.pending.new.cidr }}</span>
          {{ t('settings.pending.withAddressAfter') }}
        </template>
        <template v-else>{{ t('settings.pending.withoutAddress') }}</template>
      </p>
      <div class="mt-3">
        <Button variant="secondary" :icon="Undo2" :disabled="isBusy" @click="store.revert">
          {{ t('settings.pending.clear') }}
        </Button>
      </div>
    </Panel>

    <Panel>
      <template #header>
        <h2 class="flex items-center gap-1.5 text-sm font-semibold text-slate-800 dark:text-slate-200">
          <AlertTriangle :size="14" />
          {{ t('settings.recovery.heading') }}
        </h2>
      </template>
      <p class="text-sm text-slate-600 dark:text-slate-400">
        {{ t('settings.recovery.bodyBefore') }}
        <span class="font-mono">http://{{ store.status?.recoveryAddress ?? '169.254.1.1' }}</span
        >,
        {{ t('settings.recovery.bodyAfter', { example: '169.254.1.2/24' }) }}
      </p>
    </Panel>

    <!-- Dev builds only. isDevMode folds to a literal at build time (see utils/mode.ts), so this
         never renders in a prod bundle — though, being a template v-if rather than an `if` in
         script, the markup itself is still shipped. That is fine: the API does not register
         POST /api/settings/logs/clear at all in prod, so the endpoint is a 404 there regardless of
         what any bundle contains. This v-if decides what a technician is shown; it is not the
         security boundary. -->
    <Panel v-if="isDevMode">
      <template #header>
        <h2 class="text-sm font-semibold text-slate-800 dark:text-slate-200">
          {{ t('settings.advanced.heading') }}
        </h2>
      </template>
      <!-- No label prop: Disclosure falls back to the shared "Advanced" wording, matching the
           three other disclosures in this app in both locales. -->
      <Disclosure>
        <div class="flex w-full flex-col gap-2">
          <p class="text-sm text-slate-600 dark:text-slate-400">
            {{ t('settings.advanced.clearLogsDescription') }}
          </p>
          <div class="flex items-center gap-3">
            <Button
              variant="danger"
              :icon="Trash2"
              :disabled="clearingLogs"
              @click="onClearLogs"
            >
              {{ clearingLogs ? t('settings.advanced.clearing') : t('settings.advanced.clearLogs') }}
            </Button>
            <span v-if="clearLogsMessage" class="text-sm text-slate-600 dark:text-slate-400">
              {{ clearLogsMessage }}
            </span>
            <span v-else-if="clearLogsError" class="text-sm text-red-600 dark:text-red-400">
              {{ clearLogsError }}
            </span>
          </div>
          <p v-if="clearLogsResult" class="font-mono text-xs text-slate-400 dark:text-slate-500">
            {{ t('settings.advanced.logDir') }}: {{ clearLogsResult.logDir }}
          </p>
        </div>
      </Disclosure>
    </Panel>
  </div>
</template>
