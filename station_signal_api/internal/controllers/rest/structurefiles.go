package rest

import (
	"errors"
	"net/http"

	"station_signal_api/internal/core/structurefiles"
)

// handleUploadStructureFile saves a browsed/dropped SCL/ICD/CID file to local disk and returns
// its path, for the frontend to then pass back as sclFilePath in a later POST /devices call.
// The file is stored on this API's own host, which always runs on the same box as the daemon
// (see station_signal_api/CLAUDE.md's single-box deployment model), so the returned path is one
// the daemon can read directly.
func (a *API) handleUploadStructureFile(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseMultipartForm(0); err != nil {
		a.writeError(w, invalidArgument("not valid multipart/form-data"))
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
