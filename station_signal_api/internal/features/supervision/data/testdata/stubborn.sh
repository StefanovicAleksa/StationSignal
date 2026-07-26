#!/bin/sh
# Test fixture: a long-lived process that ignores SIGTERM/SIGINT, used to verify
# Process.Kill() (SIGKILL, which cannot be trapped or ignored) escalates past a Terminate()
# that the child chooses not to honor. Ignored signal dispositions survive exec, so `exec`ing
# into sleep after the trap keeps this a single process (same PID, no forked child) — nothing
# can be left orphaned holding open file descriptors after it exits.
trap '' TERM INT
exec sleep 30
