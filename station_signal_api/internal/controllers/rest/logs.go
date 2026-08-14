package rest

import (
	"net/http"
)

// handleClearLogs empties this stack's on-box log files.
//
// It exists for capture hygiene rather than disk space: with no logrotate anywhere in front of
// these files, a capture pulled off a box after a day of bench work has to be separated from the
// session that actually matters by reading timestamps. Clearing immediately before an on-site test
// run means the logs that come back contain only that run.
//
// Dev-only — Router does not register this route at all in prod. POST rather than DELETE, matching
// every other mutating route here, so a stray link or cross-origin GET can't fire it.
//
// A partial clear is a 200, not an error: the response reports skippedCount and the caller decides
// what to say about it. Clearing eleven of twelve logs is a far better outcome than refusing to
// clear any because one file was owned by someone else, and the only realistic skip
// (station-signal-api.log, created root-owned by systemd) is exactly the case the privileged
// helper already tries to cover.
func (a *API) handleClearLogs(w http.ResponseWriter, r *http.Request) {
	result, err := a.logFiles.Clear()
	if err != nil {
		a.writeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, result)
}
