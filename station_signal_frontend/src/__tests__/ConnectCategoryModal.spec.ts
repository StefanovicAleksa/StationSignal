import { describe, it, expect } from 'vitest'
import { mount } from '@vue/test-utils'
import ConnectCategoryModal from '@/components/device/ConnectCategoryModal.vue'

// <Teleport> moves its content to document.body by default, which VTU's wrapper.find can't
// see - stub it so the dialog renders inline and stays queryable, same as any other component.
function mountModal(open: boolean) {
  return mount(ConnectCategoryModal, {
    props: { open },
    global: { stubs: { teleport: true } },
  })
}

function checkboxFor(wrapper: ReturnType<typeof mountModal>, label: string) {
  const labelEl = wrapper.findAll('label').find((l) => l.text().includes(label))
  if (!labelEl) throw new Error(`no label found for "${label}"`)
  return labelEl.find('input[type="checkbox"]')
}

function connectButton(wrapper: ReturnType<typeof mountModal>) {
  const button = wrapper.findAll('button').find((b) => b.text() === 'Connect')
  if (!button) throw new Error('no Connect button found')
  return button
}

function cancelButton(wrapper: ReturnType<typeof mountModal>) {
  const button = wrapper.findAll('button').find((b) => b.text() === 'Cancel')
  if (!button) throw new Error('no Cancel button found')
  return button
}

describe('ConnectCategoryModal', () => {
  it('defaults to Control and Other checked, Measurement/Protection unchecked, "all" off', () => {
    const wrapper = mountModal(true)

    expect((checkboxFor(wrapper, 'Control').element as HTMLInputElement).checked).toBe(true)
    expect((checkboxFor(wrapper, 'Other').element as HTMLInputElement).checked).toBe(true)
    expect((checkboxFor(wrapper, 'Measurement').element as HTMLInputElement).checked).toBe(false)
    expect((checkboxFor(wrapper, 'Protection').element as HTMLInputElement).checked).toBe(false)
    expect((checkboxFor(wrapper, 'Connect to all categories').element as HTMLInputElement).checked).toBe(false)
  })

  it('emits confirm with the default categories when Connect is clicked immediately', async () => {
    const wrapper = mountModal(true)

    await connectButton(wrapper).trigger('click')

    expect(wrapper.emitted('confirm')).toEqual([[['CONTROL', 'OTHER']]])
  })

  it('emits confirm(undefined) and disables the per-category checkboxes when "all" is toggled on', async () => {
    const wrapper = mountModal(true)

    await checkboxFor(wrapper, 'Connect to all categories').setValue(true)
    expect((checkboxFor(wrapper, 'Control').element as HTMLInputElement).disabled).toBe(true)

    await connectButton(wrapper).trigger('click')

    expect(wrapper.emitted('confirm')).toEqual([[undefined]])
  })

  it('supports selecting multiple categories together (e.g. Control + Protection)', async () => {
    const wrapper = mountModal(true)

    await checkboxFor(wrapper, 'Protection').setValue(true)
    await connectButton(wrapper).trigger('click')

    expect(wrapper.emitted('confirm')).toEqual([[['CONTROL', 'OTHER', 'PROTECTION']]])
  })

  it('disables Connect once every category is unchecked and "all" is off', async () => {
    const wrapper = mountModal(true)

    await checkboxFor(wrapper, 'Control').setValue(false)
    await checkboxFor(wrapper, 'Other').setValue(false)

    expect((connectButton(wrapper).element as HTMLButtonElement).disabled).toBe(true)
    await connectButton(wrapper).trigger('click')
    expect(wrapper.emitted('confirm')).toBeUndefined()
  })

  it('emits cancel and nothing else when Cancel is clicked', async () => {
    const wrapper = mountModal(true)

    await cancelButton(wrapper).trigger('click')

    expect(wrapper.emitted('cancel')).toHaveLength(1)
    expect(wrapper.emitted('confirm')).toBeUndefined()
  })

  it('resets to the default selection each time it reopens', async () => {
    const wrapper = mountModal(true)
    await checkboxFor(wrapper, 'Connect to all categories').setValue(true)
    expect((checkboxFor(wrapper, 'Connect to all categories').element as HTMLInputElement).checked).toBe(true)

    await wrapper.setProps({ open: false })
    await wrapper.setProps({ open: true })

    expect((checkboxFor(wrapper, 'Connect to all categories').element as HTMLInputElement).checked).toBe(false)
    expect((checkboxFor(wrapper, 'Control').element as HTMLInputElement).checked).toBe(true)
    expect((checkboxFor(wrapper, 'Other').element as HTMLInputElement).checked).toBe(true)
  })

  it('renders nothing when closed', () => {
    const wrapper = mountModal(false)
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
  })
})
