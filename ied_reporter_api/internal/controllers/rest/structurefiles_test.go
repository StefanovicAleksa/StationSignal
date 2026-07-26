package rest

import (
	"bytes"
	"encoding/json"
	"errors"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"ied_reporter_api/internal/core/daemonproto"
	"ied_reporter_api/internal/core/structurefiles"
)

func multipartUploadRequest(t *testing.T, fieldName, filename string, content []byte) *http.Request {
	t.Helper()
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	if fieldName != "" {
		part, err := writer.CreateFormFile(fieldName, filename)
		require.NoError(t, err)
		_, err = part.Write(content)
		require.NoError(t, err)
	}
	require.NoError(t, writer.Close())

	req := httptest.NewRequest(http.MethodPost, "/structure-files", &body)
	req.Header.Set("Content-Type", writer.FormDataContentType())
	return req
}

func TestHandleUploadStructureFile_Success(t *testing.T) {
	files := &mockStructureFileStore{path: "/var/lib/ied_reporter_api/structure_files/abc123-device.icd"}
	api := New(&mockReportingService{}, &mockScanningService{}, &mockDaemonSupervisor{}, &mockDaemonStatus{}, files, nil)
	mux := Router(api)

	req := multipartUploadRequest(t, "file", "device.icd", []byte("<SCL/>"))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	require.Equal(t, http.StatusCreated, rec.Code)
	var body map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &body))
	assert.Equal(t, files.path, body["path"])
	assert.Equal(t, "device.icd", files.gotOriginalName)
}

func TestHandleUploadStructureFile_MissingFileField(t *testing.T) {
	api := New(&mockReportingService{}, &mockScanningService{}, &mockDaemonSupervisor{}, &mockDaemonStatus{}, &mockStructureFileStore{}, nil)
	mux := Router(api)

	req := multipartUploadRequest(t, "", "", nil)
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusBadRequest, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrInvalidArgument)
}

func TestHandleUploadStructureFile_UnsupportedExtension(t *testing.T) {
	files := &mockStructureFileStore{err: structurefiles.ErrUnsupportedExtension}
	api := New(&mockReportingService{}, &mockScanningService{}, &mockDaemonSupervisor{}, &mockDaemonStatus{}, files, nil)
	mux := Router(api)

	req := multipartUploadRequest(t, "file", "device.exe", []byte("data"))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusBadRequest, rec.Code)
	assertErrorCode(t, rec, daemonproto.ErrInvalidArgument)
}

func TestHandleUploadStructureFile_StoreFailure(t *testing.T) {
	files := &mockStructureFileStore{err: errors.New("disk full")}
	api := New(&mockReportingService{}, &mockScanningService{}, &mockDaemonSupervisor{}, &mockDaemonStatus{}, files, nil)
	mux := Router(api)

	req := multipartUploadRequest(t, "file", "device.icd", []byte("data"))
	rec := httptest.NewRecorder()
	mux.ServeHTTP(rec, req)

	assert.Equal(t, http.StatusInternalServerError, rec.Code)
}
