#ifndef SCL_BOOTSTRAP_TYPES_H_
#define SCL_BOOTSTRAP_TYPES_H_

#include <stdint.h>
#include "stdbool_compat.h"
#include "linked_list.h"
#include "iec61850_client.h"

/*
 * Domain vocabulary for this feature IS libiec61850's MMS-client vocabulary
 * (IedClientError) - same convention as mms_report_client's domain layer:
 * this data genuinely is the feature's domain, not swappable infrastructure.
 *
 * Unlike ied_model/mms_report_client/goose_subscriber, this feature has no
 * IedModelHandle dependency anywhere - it does not load SCL, does not know
 * about RCBs/GoCBs, and does not enable reporting. Its only job is: given a
 * host list, find which ones speak MMS, and for those, fetch their own SCL
 * file (.icd/.cid/.scd/.ssd/.sed) over the standard MMS file services. What
 * happens to the retrieved bytes afterward (e.g. staging to a temp file and
 * handing it to IedModel_loadFromFile) is a wiring-layer decision made by
 * whatever calls this feature - not this feature's concern.
 */

typedef enum {
    SCL_BOOTSTRAP_OK = 0,
    SCL_BOOTSTRAP_ERR_INVALID_ARGUMENT,
    SCL_BOOTSTRAP_ERR_OUT_OF_MEMORY
} SclBootstrapError;

typedef enum {
    /* phase 1 TCP probe never got a connection within tcpProbeTimeoutMs, or
     * the connection was actively refused. */
    SCL_BOOTSTRAP_CANDIDATE_NO_MMS_SERVER = 0,

    /* TCP was reachable, but the actual MMS association (COTP/session/
     * presentation/ACSE/MMS-initiate, driven by IedConnection_connect) was
     * rejected, timed out, or a later browse/download step failed for a
     * reason other than access control. */
    SCL_BOOTSTRAP_CANDIDATE_MMS_CONNECT_FAILED,

    /* Associated fine, browsed the directory tree, nothing matched the SCL
     * extension allowlist anywhere within maxBrowseDepth. */
    SCL_BOOTSTRAP_CANDIDATE_NO_SCL_FILE_FOUND,

    /* The server refused access due to authentication/authorization, and
     * either no acseAuthPassword was configured, or the one password-auth
     * retry also failed. Covers two distinct rejection points, both folded
     * into this one status: the connect itself being refused at the ACSE
     * level (observed empirically as IED_ERROR_CONNECTION_REJECTED when a
     * server-side AcseAuthenticator rejects the association - not documented
     * in the vendored headers), or a later browse/GetFile call being
     * rejected with IED_ERROR_ACCESS_DENIED at the MMS/file-service level.
     * lastMmsError carries whichever of the two was actually observed. */
    SCL_BOOTSTRAP_CANDIDATE_ACCESS_DENIED,

    /* A file was located and access was authorized, but GetFile itself
     * errored or was interrupted mid-transfer. */
    SCL_BOOTSTRAP_CANDIDATE_DOWNLOAD_FAILED,

    /* Success - fileName/fileData/fileSize are populated. */
    SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED
} SclBootstrapCandidateStatus;

/*
 * One candidate's outcome. Always present in the result list returned by
 * SclBootstrap_scanAndFetch, one per input host, in input order - including
 * the ones that didn't pan out, so callers get a complete picture.
 */
typedef struct {
    char* host;                  /* owned copy of the input candidate */
    int port;
    SclBootstrapCandidateStatus status;
    IedClientError lastMmsError; /* diagnostics only - IED_ERROR_OK if not applicable */
    bool authWasAttempted;       /* true if a password-auth retry connection was made */

    /* Only meaningful when status == SCL_BOOTSTRAP_CANDIDATE_FILE_RETRIEVED. */
    char* fileName;               /* owned copy, server-side path as returned by directory browse */
    uint8_t* fileData;            /* owned buffer, raw bytes exactly as received */
    uint32_t fileSize;
} SclBootstrapResult;

typedef void (*SclBootstrapProgressCallback)(void* userParam, const SclBootstrapResult* result);

typedef struct {
    uint32_t tcpProbeTimeoutMs;      /* default 500 - phase 1 per-candidate bound */
    int      maxConcurrentTcpProbes; /* default 16 - phase 1 sliding-window width */
    uint32_t mmsConnectTimeoutMs;    /* default 3000 - phase 2 association bound, 0 = library default */
    uint32_t mmsRequestTimeoutMs;    /* default 0 = library default - browse/getFile request bound */
    int      maxBrowseDepth;         /* default 2 - root directory plus one level of subdirectories */
    const char* acseAuthPassword;    /* NULL = never attempt ACSE_AUTH_PASSWORD retry; copied internally */
} SclBootstrapConfig;

/*
 * Internal representation. Defined here (rather than hidden behind an
 * additional internal-only header) because every file within this feature
 * needs field access, mirroring ied_model/mms_report_client's struct
 * s*Handle convention: opacity is enforced by which header is exposed
 * (service/scl_bootstrap_api.h is the only public one), not by hiding the
 * struct.
 */
struct sSclBootstrapHandle {
    SclBootstrapConfig config;
    char* ownedAuthPassword; /* owned copy of config.acseAuthPassword, or NULL - stays valid
                                for the handle's whole lifetime, independent of whatever
                                buffer the caller originally passed in */

    SclBootstrapProgressCallback progressCallback;
    void* progressCallbackParam;
};

typedef struct sSclBootstrapHandle* SclBootstrapHandle;

#endif /* SCL_BOOTSTRAP_TYPES_H_ */
