// Package session gives every browser talking to this API its own identity, so scans and
// device-reporting sessions started by one browser aren't visible to or stoppable by another.
// Identity is a plain cookie: no login, no server-side session store beyond what each feature's
// own data layer already tags records with (see scanning/reporting's SessionID fields) — this
// package only mints/reads the cookie itself.
package session

import (
	"context"
	"net/http"

	"github.com/google/uuid"
)

// CookieName is the cookie this package mints and reads.
const CookieName = "station_signal_session"

type contextKey struct{}

// Middleware ensures every request carries a session cookie, minting one if absent, and makes
// the resolved session ID available to downstream handlers via FromContext. Mounted once on the
// shared router in rest.Router, this also covers the websocket endpoints controllers/ws mounts
// onto that same router (see main.go) — no separate wiring needed there in production.
//
// The cookie is deliberately browser-lifetime (no Expires/MaxAge): scans and device-reporting
// sessions are runtime-only and don't survive a box reboot anyway, so there's no value in a
// session outliving the browser tab. No Secure flag either — this deployment is plain HTTP only
// (see deploy/README.md), and Secure would silently stop the cookie from ever being sent.
func Middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := ""
		if c, err := r.Cookie(CookieName); err == nil && c.Value != "" {
			id = c.Value
		} else {
			id = uuid.NewString()
			http.SetCookie(w, &http.Cookie{
				Name:     CookieName,
				Value:    id,
				Path:     "/",
				HttpOnly: true,
				SameSite: http.SameSiteLaxMode,
			})
		}
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), contextKey{}, id)))
	})
}

// FromContext returns the session ID Middleware resolved for this request, or "" if
// Middleware never ran (e.g. a test router that didn't wire it in).
func FromContext(ctx context.Context) string {
	id, _ := ctx.Value(contextKey{}).(string)
	return id
}
