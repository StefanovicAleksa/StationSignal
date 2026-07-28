<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { AlertTriangle, Save } from '@lucide/vue'

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

onUnmounted(() => {
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

const isBusy = computed(() => store.phase === 'applying' || store.phase === 'waitingForReconnect' || store.phase === 'confirming')

const retrySecondsLeft = computed(() => {
  if (!store.nextPollAt) return 0
  return Math.max(0, Math.ceil((store.nextPollAt - Date.now()) / 1000))
})

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
      </template>

      <p v-else-if="store.phase === 'confirming'">Confirming…</p>
      <p v-else-if="store.phase === 'confirmed'">
        Confirmed — redirecting to <span class="font-mono">{{ store.newOrigin }}</span
        >…
      </p>

      <p v-else-if="store.phase === 'reverted'">
        The box did not confirm reachable in time and should have automatically reverted to its previous configuration.
        Reload this page — if it still doesn't respond, see the fixed recovery address below.
      </p>

      <p v-else-if="store.phase === 'error' && store.applyError">{{ store.applyError.message }} ({{ store.applyError.code }})</p>
    </div>

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
