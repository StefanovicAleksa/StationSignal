import type { en } from './messages/en'

// `en.ts` is declared `as const`, so every leaf is a string *literal* type. A locale file typed
// directly against `typeof en` would therefore only accept the English text back verbatim.
// `Translated` widens every leaf to plain `string` while keeping the exact key structure — which
// is the half that actually needs enforcing.
type Translated<T> = {
  [K in keyof T]: T[K] extends string ? string : Translated<T[K]>
}

export type Messages = Translated<typeof en>

// Every valid dot-path through the catalog, e.g. 'scan.status.active'. Passing anything else to
// `t()` is a compile error, so a renamed or deleted key can't survive as a silently-wrong string.
export type MessageKey = DotPaths<Messages>

type DotPaths<T> = {
  [K in keyof T & string]: T[K] extends string ? K : `${K}.${DotPaths<T[K]>}`
}[keyof T & string]

// Values substituted into a message's `{placeholder}` slots.
export type MessageParams = Record<string, string | number>
