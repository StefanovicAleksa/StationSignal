#ifndef SCL_BOOTSTRAP_UTILS_H_
#define SCL_BOOTSTRAP_UTILS_H_

/*
 * Small reusable string helpers shared by the data layer (MMS session/TCP
 * probe) and the service layer. No IedConnection/Socket awareness here.
 */

/* NULL-safe strdup - returns NULL if s is NULL. Caller owns the result (free). */
char*
SclBootstrapUtils_safeStringDup(const char* s);

/*
 * Joins a parent directory path with a child entry name into the next
 * argument to pass to IedConnection_getFileDirectory/getFile. If parentDir is
 * NULL or empty, returns a copy of entryName. Otherwise returns a copy of
 * parentDir immediately followed by entryName (no separator inserted -
 * subdirectory entry names already carry their own trailing '/', per the
 * ACSI file-directory convention - see scl_bootstrap_usecases.h's
 * isDirectoryEntry doc comment). Caller owns the result (free). Returns NULL
 * if entryName is NULL or on allocation failure.
 */
char*
SclBootstrapUtils_joinPath(const char* parentDir, const char* entryName);

#endif /* SCL_BOOTSTRAP_UTILS_H_ */
