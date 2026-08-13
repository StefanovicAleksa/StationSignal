#ifndef STATION_SIGNAL_LOG_H_
#define STATION_SIGNAL_LOG_H_

/*
 * The daemon's only logging facility: four severity macros, each routed to a
 * per-feature append-mode file, with a single runtime threshold read once
 * from STATION_SIGNAL_LOG_LEVEL.
 *
 * Why one file per feature: every log line in this codebase used to be an
 * unconditional fprintf(stderr, ...), and a deployed box inherits all of it
 * into one combined, interleaved append-mode file (the API spawns this
 * daemon and passes its own stdio fds straight through - see the parent
 * repo's station_signal_api/internal/features/supervision/data/process.go).
 * mms_report_client's per-RCB enable sequence alone is 20+ lines per RCB per
 * connect cycle, which drowns out every other feature's handful of lines in
 * that one file - exactly what made a real GOOSE-reception investigation
 * hard to read. Each translation unit now declares which feature it belongs
 * to (SS_LOG_FEATURE, below) and every SS_LOG_* call in that file lands in
 * that feature's own file instead.
 *
 * Threshold: STATION_SIGNAL_LOG_LEVEL = debug | info | warn | error, defaulting
 * to info when unset or unrecognized. Default-quiet is deliberate: a deployed
 * box that somehow loses its environment file should fall back to the quiet
 * setting, not the loud one. The parent repo's deploy/setup.sh writes dev or
 * prod into /etc/station-signal/station-signal.env, and the API exports the
 * matching level into this process's environment when it spawns it.
 *
 * Where: STATION_SIGNAL_LOG_DIR, defaulting to /var/log/station_signal (the
 * directory deploy/setup.sh already creates and chowns for the service
 * user) - auto-created (best-effort, mkdir) if missing, which mainly matters
 * for a fresh dev checkout never touched by that script. Each feature's file
 * is station-signal-<SS_LOG_FEATURE>.log. A file that genuinely can't be
 * opened (bad permissions, unwritable path) falls back to stderr with one
 * warning line - a logging facility that can silently produce nothing is
 * worse than useless.
 *
 * This is header-only on purpose. rebuild_proj.sh compiles src/main.c plus
 * three fixed directory globs, and the 16 test Makefiles each hardcode their
 * own source lists - a src/log.c would have to be added to every one of them
 * to link. A static inline accessor needs no such edit anywhere. It also
 * stays libc-only, deliberately: tests/Makefile links
 * test_orchestration_staging (which includes this header) without -lhal, to
 * prove that file is pure libc I/O - so timestamps here use
 * gettimeofday()/localtime_r(), never hal_time.h.
 *
 * Macros are SS_LOG_*, not LOG_*: third_party/include/logging.h is
 * libiec61850's IEC 61850 LOG *service* header (server-side log control
 * blocks, unrelated to diagnostics), and a bare LOG_DEBUG risks colliding with
 * vendored code we must not touch.
 *
 * The macros append nothing beyond a leading timestamp. Every message keeps
 * its own trailing "\n" exactly as written, so switching a call site between
 * severities never changes what the rest of the line looks like.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

/*
 * Every .c file that wants its own log file defines this - a plain string
 * literal, one value for the whole translation unit - as its very first
 * line, before its own #include "log.h" (an already-cached SS_LOG_FEATURE
 * from a different file earlier in the same compilation unit cannot leak in:
 * each .c file is its own translation unit). Files that don't opt in (main.c,
 * orchestration/scan_orchestration/device_manager wiring) fall back to
 * "daemon" here.
 */
#ifndef SS_LOG_FEATURE
#define SS_LOG_FEATURE "daemon"
#endif

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

/* "YYYY-MM-DD HH:MM:SS.mmm", local wall clock - daemon output previously had
 * no timestamp at all, unlike the API's own slog lines, which made
 * correlating the two (or correlating a line against a real-world event)
 * needlessly hard. */
static inline void
StationSignalLog_formatTimestamp(char* buf, size_t bufSize)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tmBuf;
    localtime_r(&tv.tv_sec, &tmBuf);

    char base[20];
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tmBuf);

    snprintf(buf, bufSize, "%s.%03ld", base, (long) (tv.tv_usec / 1000));
}

/*
 * One cached FILE* per translation unit, keyed implicitly by that TU's own
 * SS_LOG_FEATURE - a compile-time constant, never varies within one file, so
 * no runtime name registry is needed here (each .c file can only ever want
 * its own one sink).
 *
 * Gated purely on sink == NULL, not a separate "attempted" flag: an earlier
 * version of this pattern used a flag, which let a second thread racing the
 * very first call see "attempted" already set and return a not-yet-assigned
 * NULL FILE* - a real risk in this codebase (mms_report_client's supervisor
 * vs. report-callback threads, goose_subscriber's reception vs. liveness
 * threads, both pairs living in the same file). Gating on sink itself means
 * a rare both-NULL race just costs a harmless duplicate fopen instead.
 */
static inline FILE*
StationSignalLog_sink(void)
{
    static FILE* sink = NULL;
    if (sink) return sink;

    const char* dir = getenv("STATION_SIGNAL_LOG_DIR");
    if (!dir || dir[0] == '\0') {
        dir = "/var/log/station_signal";
    }

    /* Best-effort - deploy/setup.sh already creates and chowns this on a
     * real box; this only matters for a fresh/non-deploy-scripted dev
     * checkout. Ignoring the return value is deliberate: EEXIST is the
     * expected common case, and the fopen() below is the real success
     * signal. */
    mkdir(dir, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/station-signal-%s.log", dir, SS_LOG_FEATURE);

    FILE* opened = fopen(path, "a");
    if (!opened) {
        fprintf(stderr,
                "[log] could not open log file '%s' (%s) - falling back to "
                "stderr for '%s' output\n",
                path, strerror(errno), SS_LOG_FEATURE);
        opened = stderr;
    } else {
        /* A freshly-opened regular file is fully-buffered by default,
         * unlike stderr (unbuffered per the C standard) - a SIGKILL,
         * segfault, or power loss (exactly the scenarios worth reading
         * these files for) skips stdio's atexit flush and would silently
         * lose the last buffered lines. Line-buffered flushes on the
         * trailing '\n' every call site already writes, so no per-call
         * fflush is needed. */
        setvbuf(opened, NULL, _IOLBF, 0);
    }

    sink = opened;
    return sink;
}

#define SS_LOG(level, ...)                                       \
    do {                                                         \
        if ((level) >= StationSignalLog_threshold()) {           \
            FILE* ssSink = StationSignalLog_sink();               \
            char ssTsBuf[32];                                     \
            StationSignalLog_formatTimestamp(ssTsBuf, sizeof(ssTsBuf)); \
            fprintf(ssSink, "[%s] ", ssTsBuf);                    \
            fprintf(ssSink, __VA_ARGS__);                         \
        }                                                         \
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
