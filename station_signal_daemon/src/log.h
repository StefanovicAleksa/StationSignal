#ifndef STATION_SIGNAL_LOG_H_
#define STATION_SIGNAL_LOG_H_

/*
 * The daemon's only logging facility: four severity macros over stderr, with a
 * single runtime threshold read once from STATION_SIGNAL_LOG_LEVEL.
 *
 * Why this exists: every log line in this codebase used to be an unconditional
 * fprintf(stderr, ...), and a deployed box inherits all of it into one
 * append-mode file (the API spawns this daemon and passes its own stdio fds
 * straight through - see the parent repo's
 * station_signal_api/internal/features/supervision/data/process.go). Several
 * paths here are deliberately, heavily verbose for diagnosis - the per-RCB
 * enable sequence alone is 20+ lines per RCB per connect cycle - which is the
 * right posture while commissioning a device and the wrong one for a Pi
 * running unattended in a substation for months. See CHANGELOG.md's note on
 * that log volume being a temporary diagnostic posture.
 *
 * Threshold: STATION_SIGNAL_LOG_LEVEL = debug | info | warn | error, defaulting
 * to info when unset or unrecognized. Default-quiet is deliberate: a deployed
 * box that somehow loses its environment file should fall back to the quiet
 * setting, not the loud one. The parent repo's deploy/setup.sh writes dev or
 * prod into /etc/station-signal/station-signal.env, and the API exports the
 * matching level into this process's environment when it spawns it.
 *
 * This is header-only on purpose. rebuild_proj.sh compiles src/main.c plus
 * three fixed directory globs, and the 16 test Makefiles each hardcode their
 * own source lists - a src/log.c would have to be added to every one of them
 * to link. A static inline accessor needs no such edit anywhere.
 *
 * Macros are SS_LOG_*, not LOG_*: third_party/include/logging.h is
 * libiec61850's IEC 61850 LOG *service* header (server-side log control
 * blocks, unrelated to diagnostics), and a bare LOG_DEBUG risks colliding with
 * vendored code we must not touch.
 *
 * The macros append nothing. Every message keeps its own trailing "\n" exactly
 * as written, so switching a call site between severities never changes what
 * the line looks like.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SS_LOG_LEVEL_DEBUG = 0,
    SS_LOG_LEVEL_INFO = 1,
    SS_LOG_LEVEL_WARN = 2,
    SS_LOG_LEVEL_ERROR = 3
} StationSignalLogLevel;

/*
 * Reads STATION_SIGNAL_LOG_LEVEL on first use and caches it. The cache is one
 * static per translation unit rather than a single shared symbol (the whole
 * point of staying header-only); every copy resolves the same environment
 * variable to the same value, so the duplication costs an int per TU and
 * nothing else. Two threads racing the first call both compute and store that
 * same value, which is why no lock is needed here.
 */
static inline StationSignalLogLevel StationSignalLog_threshold(void)
{
    static int cached = -1;

    if (cached < 0) {
        const char* raw = getenv("STATION_SIGNAL_LOG_LEVEL");

        if (raw == NULL) {
            cached = (int) SS_LOG_LEVEL_INFO;
        } else if (strcmp(raw, "debug") == 0) {
            cached = (int) SS_LOG_LEVEL_DEBUG;
        } else if (strcmp(raw, "warn") == 0) {
            cached = (int) SS_LOG_LEVEL_WARN;
        } else if (strcmp(raw, "error") == 0) {
            cached = (int) SS_LOG_LEVEL_ERROR;
        } else {
            /* Includes "info" and anything unrecognized - an unparseable value
             * must not silently turn logging off. */
            cached = (int) SS_LOG_LEVEL_INFO;
        }
    }

    return (StationSignalLogLevel) cached;
}

#define SS_LOG(level, ...)                                  \
    do {                                                    \
        if ((level) >= StationSignalLog_threshold()) {      \
            fprintf(stderr, __VA_ARGS__);                   \
        }                                                   \
    } while (0)

/* Trace: per-member/per-step detail, timings, wire-level diagnostics. Silent
 * in a production deployment. */
#define SS_LOG_DEBUG(...) SS_LOG(SS_LOG_LEVEL_DEBUG, __VA_ARGS__)

/* Lifecycle and per-cycle outcomes: startup/shutdown, one line per scan sweep,
 * the per-connect "N enabled, N not needed, N FAILED" summary. */
#define SS_LOG_INFO(...) SS_LOG(SS_LOG_LEVEL_INFO, __VA_ARGS__)

/* Degraded but continuing: something was skipped, refused, or fell back, and
 * the operation carried on anyway. */
#define SS_LOG_WARN(...) SS_LOG(SS_LOG_LEVEL_WARN, __VA_ARGS__)

/* The operation failed. */
#define SS_LOG_ERROR(...) SS_LOG(SS_LOG_LEVEL_ERROR, __VA_ARGS__)

/*
 * TEMPORARY diagnostic sink - remove once GOOSE reception silence is
 * root-caused (same investigation the [GOOSE_DIAG] line in
 * goose_subscriber_connection.c already exists for - see that file and
 * docs/features/goose_subscriber.md's Known Limitations section).
 *
 * Everything routed through SS_LOG_GOOSE goes to its own append-mode file
 * instead of stderr, so it survives independent of STATION_SIGNAL_LOG_LEVEL's
 * stderr stream and can be tailed on its own without scrolling through the
 * per-RCB MMS enable volume that dominates the combined log on any station
 * with more than a couple dozen RCBs. Gated on the same debug threshold as
 * SS_LOG_DEBUG (dev mode only) - this is diagnostic trace, not a steady-state
 * feature.
 *
 * Path: STATION_SIGNAL_GOOSE_LOG_FILE, defaulting to
 * /var/log/station_signal/station-signal-goose.log (same directory
 * deploy/setup.sh already creates and chowns for the combined log). Falls
 * back to stderr if that path can't be opened (wrong permissions, missing
 * directory on a non-deployed dev checkout, etc.) - a diagnostic tool that
 * can silently produce nothing is worse than useless.
 */
static inline FILE*
StationSignalLog_gooseSink(void)
{
    static FILE* sink = NULL;
    static int attempted = 0;

    if (!attempted) {
        attempted = 1;

        const char* path = getenv("STATION_SIGNAL_GOOSE_LOG_FILE");
        if (!path || path[0] == '\0') {
            path = "/var/log/station_signal/station-signal-goose.log";
        }

        sink = fopen(path, "a");
        if (!sink) {
            fprintf(stderr,
                    "[log] could not open GOOSE diagnostic log file '%s' (%s) - "
                    "falling back to stderr for SS_LOG_GOOSE output\n",
                    path, strerror(errno));
            sink = stderr;
        }
    }

    return sink;
}

#define SS_LOG_GOOSE(...)                                            \
    do {                                                             \
        if (SS_LOG_LEVEL_DEBUG >= StationSignalLog_threshold()) {    \
            FILE* ssGooseSink = StationSignalLog_gooseSink();        \
            fprintf(ssGooseSink, __VA_ARGS__);                       \
            fflush(ssGooseSink);                                     \
        }                                                            \
    } while (0)

#endif /* STATION_SIGNAL_LOG_H_ */
