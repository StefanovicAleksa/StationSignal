import { describe, it, expect, vi, afterEach } from 'vitest'

import { logger } from '@/utils/logger'

// The gate is read once at module load from import.meta.env, so these tests assert the behaviour
// of the mode Vitest itself runs in (DEV), not both branches — the prod branch is a build-time
// concern, verified by grepping the built bundle rather than at runtime here.
describe('logger', () => {
  afterEach(() => {
    vi.restoreAllMocks()
  })

  it('passes debug through under a dev build', () => {
    const spy = vi.spyOn(console, 'debug').mockImplementation(() => {})

    logger.debug('probe', { attempt: 1 })

    expect(spy).toHaveBeenCalledWith('probe', { attempt: 1 })
  })

  it('always passes warn and error through, since those report a real fault', () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {})
    const error = vi.spyOn(console, 'error').mockImplementation(() => {})

    logger.warn('degraded')
    logger.error('broken')

    expect(warn).toHaveBeenCalledWith('degraded')
    expect(error).toHaveBeenCalledWith('broken')
  })
})
