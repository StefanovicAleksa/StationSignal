package rest

import (
	"errors"
	"net/http"

	"ied_reporter_api/internal/core/structurefiles"
)

// maxStructureFileBytes caps uploaded SCL/ICD/CID files well above any realistic size (these
// are XML device models, typically kilobytes to a few megabytes) to bound memory/disk use from
// a single upload.
const maxStructureFileBytes = 20 << 20 // 20 MiB

// handleUploadStructureFile saves a browsed/dropped SCL/ICD/CID file to local disk and returns
// its path, for the frontend to then pass back as sclFilePath in a later POST /devices call.
// The file is stored on this API's own host, which always runs on the same box as the daemon
// (see ied_reporter_api/CLAUDE.md's single-box deployment model), so the returned path is one
// the daemon can read directly.
func (a *API) handleUploadStructureFile(w http.ResponseWriter, r *http.Request) {
	r.Body = http.MaxBytesReader(w, r.Body, maxStructureFileBytes)
	if err := r.ParseMultipartForm(maxStructureFileBytes); err != nil {
		a.writeError(w, invalidArgument("upload too large or not valid multipart/form-data"))
		return
	}

	file, header, err := r.FormFile("file")
	if err != nil {
		a.writeError(w, invalidArgument(`missing "file" field in upload`))
		return
	}
	defer file.Close()

	path, err := a.structureFiles.Save(header.Filename, file)
	if err != nil {
		if errors.Is(err, structurefiles.ErrUnsupportedExtension) {
			a.writeError(w, invalidArgument(err.Error()))
			return
		}
		a.writeError(w, err)
		return
	}

	writeJSON(w, http.StatusCreated, map[string]any{"path": path})
}
