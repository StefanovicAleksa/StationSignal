#include <stdlib.h>
#include "features/scl_bootstrap/data/scl_bootstrap_mms_session.h"
#include "features/scl_bootstrap/data/scl_bootstrap_auth.h"
#include "features/scl_bootstrap/data/scl_bootstrap_file_download.h"
#include "features/scl_bootstrap/domain/scl_bootstrap_usecases.h"
#include "features/scl_bootstrap/utils/scl_bootstrap_utils.h"
#include "iec61850_client.h"

/*
 * Recursively browses directoryName (NULL for root), returning a LinkedList
 * of owned char* full paths for every entry matching the SCL extension
 * allowlist, found within depthRemaining levels (depthRemaining=1 means
 * "this directory only, don't recurse into subdirectories"). *err reports the
 * outcome of the browse call at this level only - a failure recursing into
 * one subdirectory does not abort browsing of its siblings.
 */
static LinkedList
browseDirectoryRecursive(IedConnection conn, const char* directoryName, int depthRemaining, IedClientError* err) {
    LinkedList matches = LinkedList_create();
    *err = IED_ERROR_OK;

    LinkedList entries = IedConnection_getFileDirectory(conn, err, directoryName);
    if (*err != IED_ERROR_OK) {
        if (entries) LinkedList_destroyDeep(entries, (LinkedListValueDeleteFunction) FileDirectoryEntry_destroy);
        return matches;
    }

    LinkedList element = LinkedList_getNext(entries);
    while (element) {
        FileDirectoryEntry entry = (FileDirectoryEntry) LinkedList_getData(element);
        const char* name = FileDirectoryEntry_getFileName(entry);

        if (SclBootstrapUseCases_isDirectoryEntry(name)) {
            if (depthRemaining > 1) {
                char* subDir = SclBootstrapUtils_joinPath(directoryName, name);
                IedClientError subErr = IED_ERROR_OK;
                LinkedList subMatches = browseDirectoryRecursive(conn, subDir, depthRemaining - 1, &subErr);

                LinkedList subElement = LinkedList_getNext(subMatches);
                while (subElement) {
                    LinkedList_add(matches, LinkedList_getData(subElement));
                    subElement = LinkedList_getNext(subElement);
                }
                LinkedList_destroyStatic(subMatches);
                free(subDir);
            }
        } else if (SclBootstrapUseCases_isSclExtension(name)) {
            LinkedList_add(matches, SclBootstrapUtils_joinPath(directoryName, name));
        }

        element = LinkedList_getNext(element);
    }

    LinkedList_destroyDeep(entries, (LinkedListValueDeleteFunction) FileDirectoryEntry_destroy);
    return matches;
}

/*
 * Runs connect -> browse -> pick -> download once, on a fresh connection,
 * optionally with password auth configured. Fills in result's status/
 * lastMmsError and, on success, fileName/fileData/fileSize. Returns true if
 * the specific outcome was an access-control rejection (at any stage) -
 * signals to the caller that an auth retry might help.
 */
static bool
runSequenceOnce(SclBootstrapResult* result, const SclBootstrapConfig* config, const char* password) {
    result->status = SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED;
    result->lastMmsError = IED_ERROR_OK;

    IedConnection conn = IedConnection_create();
    if (!conn) return false;

    if (password) SclBootstrapAuth_configurePasswordAuth(conn, password);

    if (config->mmsConnectTimeoutMs > 0) IedConnection_setConnectTimeout(conn, config->mmsConnectTimeoutMs);
    if (config->mmsRequestTimeoutMs > 0) IedConnection_setRequestTimeout(conn, config->mmsRequestTimeoutMs);

    IedClientError err = IED_ERROR_OK;
    IedConnection_connect(conn, &err, result->host, result->port);

    if (err != IED_ERROR_OK) {
        result->lastMmsError = err;
        /* Empirically confirmed (not documented in the vendored headers):
         * an IedServer-side AcseAuthenticator rejection surfaces to the
         * client as IED_ERROR_CONNECTION_REJECTED at the ACSE/association
         * level, not IED_ERROR_ACCESS_DENIED (that code is only ever seen
         * later, from MMS-level file-service responses in the browse/
         * download stages below). Both are treated as "might be worth an
         * auth retry" here. */
        bool wasAccessDenied = (err == IED_ERROR_ACCESS_DENIED || err == IED_ERROR_CONNECTION_REJECTED);
        if (wasAccessDenied) result->status = SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED;
        IedConnection_destroy(conn);
        return wasAccessDenied;
    }

    IedClientError browseErr = IED_ERROR_OK;
    LinkedList matches = browseDirectoryRecursive(conn, NULL, config->maxBrowseDepth, &browseErr);

    if (browseErr != IED_ERROR_OK) {
        result->lastMmsError = browseErr;
        bool wasAccessDenied = (browseErr == IED_ERROR_ACCESS_DENIED);
        result->status = wasAccessDenied ? SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED : SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED;
        LinkedList_destroyDeep(matches, free);
        IedConnection_close(conn);
        IedConnection_destroy(conn);
        return wasAccessDenied;
    }

    const char* chosen = SclBootstrapUseCases_pickBestSclFile(matches);
    if (!chosen) {
        result->status = SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND;
        LinkedList_destroyDeep(matches, free);
        IedConnection_close(conn);
        IedConnection_destroy(conn);
        return false;
    }

    char* chosenCopy = SclBootstrapUtils_safeStringDup(chosen);
    LinkedList_destroyDeep(matches, free);

    SclBootstrapFileDownloadBuffer buffer;
    SclBootstrapFileDownloadBuffer_init(&buffer);

    IedClientError downloadErr = IED_ERROR_OK;
    IedConnection_getFile(conn, &downloadErr, chosenCopy, SclBootstrapFileDownload_handler, &buffer);

    if (downloadErr != IED_ERROR_OK) {
        result->lastMmsError = downloadErr;
        bool wasAccessDenied = (downloadErr == IED_ERROR_ACCESS_DENIED);
        result->status = wasAccessDenied ? SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED : SCL_BOOTSTRAP_CANDIDATE_DOWNLOAD_FAILED;
        SclBootstrapFileDownloadBuffer_free(&buffer);
        free(chosenCopy);
        IedConnection_close(conn);
        IedConnection_destroy(conn);
        return wasAccessDenied;
    }

    result->status = SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED;
    result->fileName = chosenCopy;
    result->fileData = buffer.data;
    result->fileSize = buffer.size;

    IedConnection_close(conn);
    IedConnection_destroy(conn);
    return false;
}

void
SclBootstrapMmsSession_run(SclBootstrapResult* result, const SclBootstrapConfig* config,
        const char* ownedAuthPassword) {
    result->authWasAttempted = false;

    bool accessDenied = runSequenceOnce(result, config, NULL);

    if (accessDenied && ownedAuthPassword) {
        result->authWasAttempted = true;
        runSequenceOnce(result, config, ownedAuthPassword);
    }
}
