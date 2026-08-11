/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_API_BASE_URL?: string
  // "dev" or "prod" — the deployment mode this bundle was built for, set by deploy/setup.sh and
  // read by src/utils/logger.ts to decide whether debug logging reaches the console.
  readonly VITE_STATION_SIGNAL_MODE?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
