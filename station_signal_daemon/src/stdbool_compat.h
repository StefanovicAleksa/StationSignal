#ifndef STDBOOL_COMPAT_H_
#define STDBOOL_COMPAT_H_

/*
 * third_party/include/stdbool.h is a broken vendored shim (meant only for MSVC)
 * that defines `bool` as `int` and omits `true`/`false` entirely, with no include
 * guard. Real gcc builds avoid it via -idirafter on third_party/include, but IDE
 * tooling (e.g. the VS Code C/C++ extension) doesn't reliably honor the same
 * ordering through includePath alone, so <stdbool.h> can still resolve to the
 * broken shim there. Include this instead of <stdbool.h> directly in our own
 * code so both cases are safe regardless of which stdbool.h a given toolchain
 * resolves - it self-heals whether the real header already defined true/false
 * or not.
 */
#include <stdbool.h>

#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif

#endif /* STDBOOL_COMPAT_H_ */
