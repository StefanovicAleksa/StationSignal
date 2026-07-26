#!/bin/sh
# Test fixture: a long-lived process that exits promptly on SIGTERM/SIGINT (sleep's own
# default signal handling already does this), used to verify Process.Terminate() actually
# reaches the child. `exec` replaces this shell with sleep in-place (same PID, no forked
# child) so nothing can be left orphaned holding open file descriptors after it exits.
exec sleep 30
