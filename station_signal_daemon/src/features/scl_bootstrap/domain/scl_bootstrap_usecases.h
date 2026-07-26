#ifndef SCL_BOOTSTRAP_USECASES_H_
#define SCL_BOOTSTRAP_USECASES_H_

#include "linked_list.h"
#include "features/scl_bootstrap/domain/scl_bootstrap_types.h"

/*
 * Pure logic - no IedConnection/Socket awareness at all here, that's entirely
 * the data layer's job. Takes plain strings/lists so it stays unit-testable.
 */

/*
 * True if the ACSI file-directory-entry name denotes a subdirectory rather
 * than a plain file (named with a trailing '/'). Empirically confirmed
 * (via a throwaway probe against the real, vendored IedServer
 * filestore-backed implementation - FileDirectoryEntry exposes only name/
 * size/lastModified, no explicit isDirectory flag, so this can't be
 * confirmed from the header alone): that reference implementation never
 * actually emits entries like this - IedConnection_getFileDirectory(NULL)
 * against it returns one flat, fully recursive listing of every file
 * anywhere under the served directory, each entry already carrying its full
 * path relative to the root (e.g. "subdir/nested.icd"), with no separate
 * per-directory entries to recurse into. This function (and the
 * recursive-descent it enables in scl_bootstrap_mms_session.c) is kept as a
 * defensive fallback for a server that follows a stricter reading of the
 * IEC 61850-8-1 file directory service and does emit trailing-slash
 * subdirectory markers - but that path is unexercised against this repo's
 * reference simulator, since it doesn't behave that way in practice.
 */
bool
SclBootstrapUseCases_isDirectoryEntry(const char* fileName);

/*
 * True if fileName (a plain file entry, not a directory) has one of the SCL
 * extensions (.icd/.cid/.scd/.ssd/.sed), case-insensitive.
 */
bool
SclBootstrapUseCases_isSclExtension(const char* fileName);

/*
 * Priority rank for extension-based selection when multiple SCL files are
 * found: .cid (0, most specific - an instantiated/configured description of
 * *this* live device) < .icd (1) < .scd (2) < .ssd (3) < .sed (4) < anything
 * else, which is not reached since callers only pass names that already
 * passed SclBootstrapUseCases_isSclExtension. Returns -1 if fileName has no
 * recognized SCL extension.
 */
int
SclBootstrapUseCases_extensionPriority(const char* fileName);

/*
 * Picks one file from a LinkedList of char* candidate filenames/paths (every
 * element assumed to have already passed SclBootstrapUseCases_isSclExtension)
 * by extension priority, then lexicographically within the same priority.
 * Returns a borrowed pointer into one of the list's own elements (caller must
 * copy it before the list is destroyed) or NULL if the list is empty/NULL.
 */
const char*
SclBootstrapUseCases_pickBestSclFile(LinkedList sclFileCandidates);

/*
 * True if hostList is non-NULL, non-empty, and contains no NULL/empty
 * elements.
 */
bool
SclBootstrapUseCases_isHostListValid(LinkedList hostList);

#endif /* SCL_BOOTSTRAP_USECASES_H_ */
