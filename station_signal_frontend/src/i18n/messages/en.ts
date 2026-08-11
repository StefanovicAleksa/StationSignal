// The source of truth for every user-facing string in this app. `sr.ts` is typed against this
// object (see i18n/index.ts's `Messages`), so adding a key here without adding it there is a
// `vue-tsc` error rather than a blank space at runtime.
//
// Conventions:
//   - Namespaces mirror the feature they belong to (`scan.*`, `devices.*`, `settings.*`), with
//     `common.*` reserved for strings genuinely reused across features.
//   - IEC 61850 vocabulary (GOOSE, MMS, RCB, LN, SCL, ICD/CID, CIDR) is protocol vocabulary and
//     stays untranslated in every locale.
//   - Interpolation is `{name}` — see `t()`.
export const en = {
  app: {
    name: 'Station Signal',
  },

  nav: {
    scan: 'Scan',
    devices: 'Devices',
    reports: 'Reports',
    settings: 'Settings',
  },

  theme: {
    switchToLight: 'Switch to light mode',
    switchToDark: 'Switch to dark mode',
  },

  language: {
    switchTo: 'Switch language to {language}',
  },

  common: {
    connect: 'Connect',
    cancel: 'Cancel',
    stop: 'Stop',
    view: 'View',
    clear: 'Clear',
    remove: 'Remove',
    apply: 'Apply',
    retryNow: 'Retry now',
    checkNow: 'Check now',
    loading: 'Loading…',
    none: '(none)',
    empty: '—',
    unexpectedError: 'Unexpected error',
    showAdvanced: 'Show advanced options',
    hideAdvanced: 'Hide advanced options',
    advanced: 'Advanced',
  },

  fields: {
    host: 'Host',
    mmsPort: 'MMS Port',
    interface: 'Interface',
    iedName: 'IED Name',
    optional: 'optional',
    interfacePlaceholder: 'eth0',
    hostPlaceholder: '10.250.99.14',
    interfaceRequired: 'Interface is required.',
    hostRequired: 'Host is required.',
    portRange: 'Port must be an integer between 1 and 65535.',
  },

  shortcuts: {
    label: 'Shortcuts',
    click: 'click',
    connectDefault: 'skip the picker, connect to Control + Other',
    connectAll: 'skip the picker, connect to all categories',
    // Plain-text form of the two above, for a button's `title` tooltip where markup isn't
    // possible. Keep it in step with them.
    connectTooltip: 'Shift-click: connect to Control + Other · Ctrl-click: connect to all categories',
  },

  scan: {
    title: 'Network Scan',
    subtitle: 'Sweep one or more network interfaces for IEDs speaking MMS. Multiple scans can run at once.',
    startScan: 'Start Scan',
    noScans: 'No scans running.',
    scanNumber: 'Scan #{id}',
    portSummary: '· port {port}',
    status: {
      starting: 'Starting scan…',
      active: 'Scan active — listening for discovered hosts.',
      stopping: 'Stopping scan…',
      retryingIn: 'Retrying in {seconds}s…',
      interrupted: 'Connection was interrupted — some results may be missing.',
    },
    results: {
      discovered: 'Discovered',
      empty: 'No hosts discovered yet.',
    },
  },

  devices: {
    title: 'Watched Devices',
    subtitle: 'Devices currently being reported on, updated live.',
    status: 'Status',
    lastMessage: 'Last Message',
    emptyBefore: 'No devices being watched. Connect to one above, or start one from the',
    emptyLink: 'Network Scan',
    emptyAfter: 'page.',
    // One shared set of phase labels — previously duplicated verbatim across DevicesView,
    // ReportsView and DeviceReportPanel, which had already drifted on `interrupted`.
    phase: {
      connecting: 'Connecting…',
      connected: 'Connected',
      interrupted: 'Interrupted',
      interruptedRetrying: 'Interrupted — retrying…',
      stopping: 'Stopping…',
      error: 'Error',
    },
  },

  reports: {
    title: 'Reports',
    subtitle: 'Live GOOSE/MMS reports for every device currently being watched.',
    emptyBefore: 'No devices being watched. Connect to one from the',
    emptyDevicesLink: 'Devices',
    emptyMiddle: 'page, or start one from the',
    emptyScanLink: 'Network Scan',
    emptyAfter: 'page.',
    stopReporting: 'Stop Reporting',
    interfaceAndPhase: 'Interface {interfaceId} — {phase}',
    table: {
      time: 'Time',
      type: 'Type',
      reference: 'Reference',
      value: 'Value',
      quality: 'Quality',
      empty: 'No reports received yet.',
      emptyForFilter: 'No reports match the selected categories.',
      buffered: '{rcb} (buffered)',
    },
    capability: {
      noMms: 'This device has no MMS report control blocks in its SCL — only GOOSE will be shown here.',
      noGoose: 'This device has no GOOSE control blocks in its SCL — only MMS reports will be shown here.',
    },
    filter: {
      label: 'Categories',
      all: 'All',
      hint: 'Filters what this browser shows. It does not change what the device is subscribed to.',
    },
    // Shown when this browser session was attached to a device another session already started —
    // the API shares one physical connection and keeps the creator's own category filter.
    shared: {
      banner:
        'This device was already connected by another session, subscribed to {categories}. You are viewing that shared stream — the categories you picked were not applied. Use the filter below to narrow what you see.',
      allCategories: 'all categories',
    },
  },

  connectPrompt: {
    passwordLabel: '{host} requires a password',
    passwordRequired: 'Password is required.',
    failed: 'Failed to connect to device.',
  },

  categoryModal: {
    title: 'What do you want to connect to?',
    subtitle: 'Choose which Logical Node categories to subscribe to on this device.',
    all: 'Connect to all categories',
    selectAtLeastOne: 'Select at least one category, or choose "Connect to all categories".',
    control: 'Control',
    controlHint: 'Breakers, switches, supervisory control',
    measurement: 'Measurement',
    measurementHint: 'Analog and metering values',
    protection: 'Protection',
    protectionHint: 'Protection relay functions',
    other: 'Other',
    otherHint: 'System logical nodes and anything uncategorized',
  },

  structureFile: {
    label: 'Structure File (SCL/ICD/CID)',
    uploading: 'Uploading…',
    dropHint: 'Drag & drop a structure file here, or click to browse',
    extensions: '.icd, .cid, .scd, .xml',
    uploadFailed: 'Failed to upload structure file.',
    iedNameRequired: 'IED name is required when a structure file path is given.',
  },

  settings: {
    title: 'Settings',
    subtitle:
      "Reconfigure this box's network address. This is a box-wide setting, not scoped to your browser session — it takes effect the same way for every technician using this box.",
    current: {
      heading: 'Current network configuration',
      interface: 'Interface',
      address: 'Address',
      gateway: 'Gateway',
    },
    form: {
      heading: 'Change static IP',
      addressLabel: 'New address',
      addressPlaceholder: '172.16.0.50',
      addressHint: 'The /prefix is optional — {prefix} is assumed if you leave it off.',
      addressPreview: 'Will be applied as {cidr}',
      addressInvalid: 'Enter a valid IPv4 address, e.g. 172.16.0.50 or 172.16.0.50/24.',
      gatewayLabel: 'Gateway',
      gatewayPlaceholder: '192.168.1.1',
      gatewayInvalid: 'Enter a valid IPv4 address, e.g. 192.168.1.1.',
      gatewayAdvanced: 'Gateway',
      prefillCurrent: 'Prefill current',
      provisionalNote:
        "Applying is provisional: if this page can't confirm the box is reachable at the new address within the configured window, the box automatically reverts to its previous configuration on its own — see the recovery note below if that ever needs to happen by hand.",
    },
    phase: {
      applying: 'Applying new network configuration…',
      waitingBefore: 'Applied — waiting for the box to answer at',
      waitingOr: '(or',
      waitingAfter: ').',
      checkingIn: 'Checking again in {seconds}s…',
      autoRevertNote:
        "If this box doesn't confirm reachable in time, it will automatically revert to its previous address on its own — no action needed.",
      confirming: 'Confirming…',
      confirmedBefore: 'Confirmed — redirecting to',
      reverting: 'Clearing the pending change and restoring the previous address…',
      reverted:
        'The box did not confirm reachable in time. It should have reverted to its previous configuration on its own — reload this page to check. If it still doesn\'t respond, use "Clear pending change" below, or the fixed recovery address.',
      alreadyPending:
        "This box is still holding an earlier network change open, so a new one can't be applied yet. If that change isn't one you're waiting on, clear it below.",
    },
    attempts: {
      heading: 'Connection attempts',
      ok: 'answered and allowed this page to read the response',
      httpError: 'answered, but with an error status — the proxy is up and the API behind it may not be',
      blockedByCors: 'answered, but refused this page permission to read it (CORS)',
      unreachable: 'nothing answered at that address',
      timeout: 'accepted the connection but did not answer in time — retrying',
    },
    pending: {
      heading: 'A network change is still pending',
      withAddressBefore: 'This box is holding',
      withAddressAfter: "open, awaiting confirmation. Until it's confirmed or cleared, no new address can be applied.",
      withoutAddress:
        "This box is holding an earlier change open. Until it's cleared, no new address can be applied.",
      clear: 'Clear pending change',
    },
    recovery: {
      heading: 'If this box becomes unreachable',
      bodyBefore: 'Every box permanently carries a fixed recovery address at',
      bodyAfter:
        'independent of whatever address is configured above and never changed by this page. Connect a laptop directly to the box (or via a switch with nothing else on that segment), manually set your own network adapter to a static IP in the same block — e.g. {example}, no gateway needed — then browse to that address.',
    },
  },

  errors: {
    connectionRejected: 'Connection rejected — a password may be required',
  },
} as const
