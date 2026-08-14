#!/usr/bin/env bash
# Privileged helper for the Settings page's dev-only "clear log files" action. The second (and
# only other) place this box's unprivileged station-signal user can reach root, via the narrowly
# scoped sudoers rule in deploy/sudoers/station-signal-clearlogs.
#
# WHY IT NEEDS ROOT AT ALL, for exactly one file. station_signal_api empties the log directory
# itself and needs no help for the daemon's own station-signal-<feature>.log files — the daemon
# creates those as station-signal, so the API can truncate them directly. But
# station-signal-api.log is created by *systemd*, i.e. by PID 1 as root, via the unit's
# StandardOutput=append:, so it ends up root-owned and the API cannot touch it. Left behind, it
# would carry the previous session's noise into the next capture, which defeats the point of
# clearing at all (see internal/core/logfiles).
#
# TAKES NO ARGUMENTS, BY DESIGN. The directory below is hardcoded, so nothing user-supplied or
# API-supplied ever reaches this root context: the worst this script can do, however it is
# invoked, is empty that one directory's own log files. That is what makes granting it NOPASSWD
# acceptable — unlike station-signal-netconfig.sh, which does take arguments and therefore has to
# do its own strict validation, there is simply no input here to validate.
#
# TRUNCATES, NEVER DELETES. Every writer holds its file open for its whole process lifetime: the
# daemon via fopen(path, "a") cached in a static (src/log.h, with up to four handles on one file
# since that header is `static inline` per translation unit), and systemd via append:. Unlinking
# would leave all of them writing into an orphaned inode — logging silently stops, the disk space
# is never reclaimed, and nothing reports an error. `: >` truncates in place, which is safe here
# precisely because every one of those handles is in append mode: the next write lands at offset 0
# with no run of NUL padding in front of it.
#
# Install as /opt/station_signal/bin/station-signal-clearlogs.sh, root-owned, mode 0700 (see
# deploy/setup.sh).
set -euo pipefail

# Hardcoded on purpose — see the header. Must match internal/core/logfiles.StandardDir and the
# daemon's own compiled-in default in src/log.h.
LOG_DIR="/var/log/station_signal"

if [ ! -d "$LOG_DIR" ]; then
    # Nothing has been logged yet. Not an error.
    echo "CLEARED=0"
    exit 0
fi

cleared=0
for f in "$LOG_DIR"/station-signal-*.log; do
    # The glob is literal when nothing matches.
    [ -e "$f" ] || continue
    [ -f "$f" ] || continue
    if : > "$f" 2>/dev/null; then
        cleared=$((cleared + 1))
    fi
done

echo "CLEARED=${cleared}"
