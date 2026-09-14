// Package integrity verifies that downloaded audio files are complete and
// decodable. It cross-checks the on-disk byte count against an expected
// Content-Length (when known) and uses TagLib to confirm the file can be
// opened as a valid audio stream with a positive sample rate.
package integrity

import (
	"os"

	"unspok3n/beatportdl/internal/taglib"
)

// Result describes the outcome of verifying a downloaded file.
type Result struct {
	Valid         bool
	FilePath      string
	ExpectedBytes int64
	ActualBytes   int64
	Decodable     bool
	SampleRate    int
	Error         string
}

// Checker verifies downloaded files for byte-accurate and decodable integrity.
// It has no exported fields; a zero-value instance is ready to use via NewChecker.
type Checker struct{}

// NewChecker returns a Checker ready to verify downloads.
func NewChecker() *Checker {
	return &Checker{}
}

// Verify checks that filePath has the expected byte count (when expectedBytes
// is non-negative) and that TagLib can decode it as audio with a positive
// sample rate. A file is Valid only when both the byte check and the decode
// check pass.
func (c *Checker) Verify(filePath string, expectedBytes int64) Result {
	info, err := os.Stat(filePath)
	if err != nil {
		return Result{
			FilePath:      filePath,
			ExpectedBytes: -1,
			Error:         err.Error(),
		}
	}

	actualBytes := info.Size()
	result := Result{
		FilePath:      filePath,
		ExpectedBytes: expectedBytes,
		ActualBytes:   actualBytes,
	}
	byteOK := expectedBytes < 0 || actualBytes == expectedBytes

	file, err := taglib.Read(filePath)
	if err != nil {
		result.Error = "undecodable: " + err.Error()
		return result
	}
	defer file.Close()

	result.SampleRate = file.SampleRate()
	result.Decodable = result.SampleRate > 0
	if !result.Decodable {
		result.Error = "invalid sample rate"
	}
	result.Valid = byteOK && result.Decodable
	if result.Valid {
		result.Error = ""
	}
	return result
}

// VerifyAudio checks only whether filePath is decodable audio, ignoring the
// expected byte count. This is useful for HLS-remuxed outputs (e.g. .m4a)
// where the final Content-Length is unknown.
func (c *Checker) VerifyAudio(filePath string) Result {
	return c.Verify(filePath, -1)
}

// VerifyDownload runs downloadFunc to fetch url into destination (using
// pbPrefix for any progress reporting) and then, when c is non-nil, verifies
// the result against expectedBytes. A nil Checker skips verification and
// reports the download as valid.
func VerifyDownload(downloadFunc func(url, dest, prefix string) error, url, destination, pbPrefix string, expectedBytes int64, c *Checker) (Result, error) {
	if err := downloadFunc(url, destination, pbPrefix); err != nil {
		return Result{FilePath: destination}, err
	}
	if c == nil {
		return Result{
			FilePath:      destination,
			Valid:         true,
			ExpectedBytes: expectedBytes,
		}, nil
	}
	return c.Verify(destination, expectedBytes), nil
}
