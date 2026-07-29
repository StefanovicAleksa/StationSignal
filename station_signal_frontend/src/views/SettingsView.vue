<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { AlertTriangle, Save, Undo2 } from '@lucide/vue'

import { useSettingsStore } from '@/stores/settings'
import Panel from '@/components/ui/Panel.vue'
import Button from '@/components/ui/Button.vue'

const store = useSettingsStore()

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

const cidrPattern = /^(\d{1,3}\.){3}\d{1,3}\/([1-9]|[12]\d|30)$/
const gatewayPattern = /^(\d{1,3}\.){3}\d{1,3}$/

const cidrError = computed(() => {
  if (!touched.value) return null
  return cidrPattern.test(cidrInput.value.trim()) ? null : 'Enter a valid IPv4 address with prefix, e.g. 192.168.1.50/24.'
})

const gatewayError = computed(() => {
  if (!touched.value || !gatewayInput.value.trim()) return null
  return gatewayPattern.test(gatewayInput.value.trim()) ? null : 'Enter a valid IPv4 address, e.g. 192.168.1.1.'
})

const isValid = computed(
  () => cidrPattern.test(cidrInput.value.trim()) && (!gatewayInput.value.trim() || gatewayPattern.test(gatewayInput.value.trim())),
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

const probeExplanation: Record<string, string> = {
  ok: 'answered and allowed this page to read the response',
  'http-error': 'answered, but with an error status — the proxy is up and the API behind it may not be',
  'blocked-by-cors': 'answered, but refused this page permission to read it (CORS)',
  unreachable: 'nothing answered at that address',
  timeout: 'accepted the connection but did not answer in time — retrying',
}

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
  if (!isValid.value) return
  store.submit({ cidr: cidrInput.value.trim(), gateway: gatewayInput.value.trim() || undefined })
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
      <h1 class="text-xl font-semibold text-slate-900 dark:text-slate-50">Settings</h1>
      <p class="text-sm text-slate-500 dark:text-slate-400">
        Reconfigure this box's network address. This is a box-wide setting, not scoped to your browser session — it takes
        effect the same way for every technician using this box.
      </p>
    </header>

    <Panel>
      <template #header>
        <h2 class="text-sm font-semibold text-slate-800 dark:text-slate-200">Current network configuration</h2>
      </template>
      <p v-if="store.loadingStatus" class="text-sm text-slate-400 dark:text-slate-500">Loading…</p>
      <p v-else-if="store.statusError" class="text-sm text-red-600 dark:text-red-400">
        {{ store.statusError.message }} ({{ store.statusError.code }})
      </p>
      <dl v-else-if="store.status" class="grid grid-cols-[max-content_1fr] gap-x-4 gap-y-1 text-sm">
        <dt class="text-slate-500 dark:text-slate-400">Interface</dt>
        <dd class="font-mono text-slate-900 dark:text-slate-100">{{ store.status.interface }}</dd>
        <dt class="text-slate-500 dark:text-slate-400">Address</dt>
        <dd class="font-mono text-slate-900 dark:text-slate-100">{{ store.status.current.cidr }}</dd>
        <dt class="text-slate-500 dark:text-slate-400">Gateway</dt>
        <dd class="font-mono text-slate-900 dark:text-slate-100">{{ store.status.current.gateway ?? '(none)' }}</dd>
      </dl>
    </Panel>

    <Panel>
      <template #header>
        <h2 class="text-sm font-semibold text-slate-800 dark:text-slate-200">Change static IP</h2>
      </template>

      <form class="flex flex-wrap items-end gap-4" @submit.prevent="handleSubmit">
        <div class="flex min-w-48 flex-1 flex-col gap-1">
          <label for="cidr" class="text-sm font-medium text-slate-700 dark:text-slate-300">New address (CIDR)</label>
          <input
            id="cidr"
            v-model="cidrInput"
            type="text"
            placeholder="192.168.1.50/24"
            :disabled="isBusy"
            class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
          />
          <p v-if="cidrError" class="text-xs text-red-600 dark:text-red-400">{{ cidrError }}</p>
        </div>

        <div class="flex min-w-48 flex-1 flex-col gap-1">
          <label for="gateway" class="text-sm font-medium text-slate-700 dark:text-slate-300">Gateway (optional)</label>
          <input
            id="gateway"
            v-model="gatewayInput"
            type="text"
            placeholder="192.168.1.1"
            :disabled="isBusy"
            class="w-full rounded-md border border-slate-300 bg-white px-3 py-2 text-sm shadow-sm focus:border-blue-500 focus:outline-none disabled:bg-slate-100 disabled:text-slate-400 dark:border-slate-700 dark:bg-slate-900 dark:text-slate-100 dark:disabled:bg-slate-800 dark:disabled:text-slate-500"
          />
          <p v-if="gatewayError" class="text-xs text-red-600 dark:text-red-400">{{ gatewayError }}</p>
        </div>

        <Button type="submit" variant="primary" :icon="Save" :disabled="isBusy">Apply</Button>
        <Button type="button" variant="secondary" :disabled="isBusy || !store.status" @click="prefillCurrent">
          Prefill current
        </Button>
      </form>

      <p class="mt-3 text-xs text-slate-500 dark:text-slate-400">
        Applying is provisional: if this page can't confirm the box is reachable at the new address within the configured
        window, the box automatically reverts to its previous configuration on its own — see the recovery note below if
        that ever needs to happen by hand.
      </p>
    </Panel>

    <div v-if="store.phase !== 'idle'" class="rounded-md border px-3 py-3 text-sm" :class="statusBannerClass">
      <p v-if="store.phase === 'applying'">Applying new network configuration…</p>

      <template v-else-if="store.phase === 'waitingForReconnect'">
        <p>
          Applied — waiting for the box to answer at <span class="font-mono">{{ store.newOrigin }}</span> (or
          <a class="underline" href="http://stationsignal.local">stationsignal.local</a>).
        </p>
        <div class="mt-1 flex items-center gap-2">
          <span v-if="store.nextPollAt">Checking again in {{ retrySecondsLeft }}s…</span>
          <Button variant="ghost" size="sm" @click="store.pollNow">Check now</Button>
        </div>
        <p class="mt-2 text-xs">
          If this box doesn't confirm reachable in time, it will automatically revert to its previous address on its own
          — no action needed.
        </p>

        <!-- Each attempt and its outcome, because "still waiting" on its own can't distinguish a
             box that never came up from one that came up and refused this page. -->
        <div v-if="store.pollLog.length" class="mt-3 border-t border-amber-300/40 pt-2 dark:border-amber-800/60">
          <p class="text-xs font-semibold">Connection attempts</p>
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

      <p v-else-if="store.phase === 'confirming'">Confirming…</p>
      <p v-else-if="store.phase === 'confirmed'">
        Confirmed — redirecting to <span class="font-mono">{{ store.newOrigin }}</span
        >…
      </p>

      <p v-else-if="store.phase === 'reverting'">Clearing the pending change and restoring the previous address…</p>

      <template v-else-if="store.phase === 'reverted'">
        <p>
          The box did not confirm reachable in time. It should have reverted to its previous configuration on its own —
          reload this page to check. If it still doesn't respond, use "Clear pending change" below, or the fixed
          recovery address.
        </p>
      </template>

      <p v-else-if="store.phase === 'error' && store.applyError">
        <template v-if="store.applyError.code === 'CHANGE_ALREADY_PENDING'">
          This box is still holding an earlier network change open, so a new one can't be applied yet. If that change
          isn't one you're waiting on, clear it below.
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
          A network change is still pending
        </h2>
      </template>
      <p class="text-sm text-slate-600 dark:text-slate-400">
        <template v-if="store.status?.pending">
          This box is holding
          <span class="font-mono">{{ store.status.pending.new.cidr }}</span> open, awaiting confirmation. Until it's
          confirmed or cleared, no new address can be applied.
        </template>
        <template v-else>
          This box is holding an earlier change open. Until it's cleared, no new address can be applied.
        </template>
      </p>
      <div class="mt-3">
        <Button variant="secondary" :icon="Undo2" :disabled="isBusy" @click="store.revert">Clear pending change</Button>
      </div>
    </Panel>

    <Panel>
      <template #header>
        <h2 class="flex items-center gap-1.5 text-sm font-semibold text-slate-800 dark:text-slate-200">
          <AlertTriangle :size="14" />
          If this box becomes unreachable
        </h2>
      </template>
      <p class="text-sm text-slate-600 dark:text-slate-400">
        Every box permanently carries a fixed recovery address at
        <span class="font-mono">http://{{ store.status?.recoveryAddress ?? '169.254.1.1' }}</span>, independent of
        whatever address is configured above and never changed by this page. Connect a laptop directly to the box (or
        via a switch with nothing else on that segment), manually set your own network adapter to a static IP in the
        same block — e.g. <span class="font-mono">169.254.1.2/24</span>, no gateway needed — then browse to that
        address.
      </p>
    </Panel>
  </div>
</template>
