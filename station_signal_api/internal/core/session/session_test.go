package session

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func newTestHandler(got *string) http.Handler {
	return Middleware(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		*got = FromContext(r.Context())
		w.WriteHeader(http.StatusOK)
	}))
}

func TestMiddleware_MintsCookieWhenAbsent(t *testing.T) {
	var got string
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	rec := httptest.NewRecorder()

	newTestHandler(&got).ServeHTTP(rec, req)

	require.NotEmpty(t, got, "a session ID should have been minted and put in context")
	cookies := rec.Result().Cookies()
	require.Len(t, cookies, 1)
	assert.Equal(t, CookieName, cookies[0].Name)
	assert.Equal(t, got, cookies[0].Value)
	assert.True(t, cookies[0].HttpOnly)
	assert.Zero(t, cookies[0].MaxAge, "cookie must be browser-lifetime, not persistent")
	assert.True(t, cookies[0].Expires.IsZero(), "cookie must be browser-lifetime, not persistent")
}

func TestMiddleware_ReusesExistingCookie(t *testing.T) {
	var got string
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	req.AddCookie(&http.Cookie{Name: CookieName, Value: "existing-session-id"})
	rec := httptest.NewRecorder()

	newTestHandler(&got).ServeHTTP(rec, req)

	assert.Equal(t, "existing-session-id", got)
	assert.Empty(t, rec.Result().Cookies(), "an already-valid cookie should not be re-minted")
}

func TestMiddleware_DifferentRequestsWithoutACookieGetDifferentSessions(t *testing.T) {
	var first, second string
	newTestHandler(&first).ServeHTTP(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, "/", nil))
	newTestHandler(&second).ServeHTTP(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, "/", nil))

	assert.NotEqual(t, first, second)
}

func TestFromContext_NoMiddlewareReturnsEmpty(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/", nil)

	assert.Empty(t, FromContext(req.Context()))
}
