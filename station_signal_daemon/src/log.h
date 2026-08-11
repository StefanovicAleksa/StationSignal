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

#endif /* STATION_SIGNAL_LOG_H_ */
