import { computed } from 'vue'

import { useI18n } from '@/i18n'
import type { DevicePhase } from '@/stores/devices'

export type PhaseTone = 'success' | 'error' | 'warning' | 'neutral'

/**
 * One source of truth for how a device's phase is worded and coloured.
 *
 * Previously four separate call sites carried their own literal `Record<string, string>` map —
 * DevicesView, ReportsView, DeviceReportPanel and (newly) the scan results table — and they had
 * already drifted: DeviceReportPanel said "Interrupted — retrying…" where the others said
 * "Interrupted". That difference is deliberate and is preserved as `detailedLabel`, but it is now
 * an explicit choice rather than an accident of copy-paste.
 */
export function useDevicePhaseLabel() {
  const { t } = useI18n()

  const label = computed<Record<DevicePhase, string>>(() => ({
    connecting: t('devices.phase.connecting'),
    connected: t('devices.phase.connected'),
    interrupted: t('devices.phase.interrupted'),
    stopping: t('devices.phase.stopping'),
    error: t('devices.phase.error'),
  }))

  // For the view actually showing the live stream, where "we're retrying" is the fact the
  // technician needs; a list view only has room for the state itself.
  const detailedLabel = computed<Record<DevicePhase, string>>(() => ({
    ...label.value,
    interrupted: t('devices.phase.interruptedRetrying'),
  }))

  const tone: Record<DevicePhase, PhaseTone> = {
    connecting: 'neutral',
    connected: 'success',
    interrupted: 'warning',
    stopping: 'neutral',
    error: 'error',
  }

  return { label, detailedLabel, tone }
}
