package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

const maxInput = 16 << 20
const maxRecords = 4096
const maxRaw = 1 << 20

var shaPattern = regexp.MustCompile(`^[0-9a-fA-F]{40}$`)
var runPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)
var casePattern = regexp.MustCompile(`^alipay/[a-z0-9-]{1,48}$`)

type captureOptions struct {
	Private           bool
	GitSHA            string
	Dirty             bool
	Package, Renderer string
}
type event struct {
	Diagnostics json.RawMessage `json:"diagnostics"`
	Run         string          `json:"run_id"`
	Case        string          `json:"case_id"`
	Seq         uint32          `json:"seq"`
	Call        uint32          `json:"call_id"`
	Parent      uint32          `json:"parent_id"`
	Connection  uint32          `json:"connection_id"`
	Operation   string          `json:"operation"`
	Boundary    string          `json:"boundary"`
	Source      string          `json:"source"`
	Item        uint32          `json:"item"`
	Length      int             `json:"length"`
	Captured    int             `json:"captured"`
	SHA         string          `json:"sha256"`
	Hex         string          `json:"hex"`
	Complete    bool            `json:"complete"`
	Dropped     uint32          `json:"dropped"`
	Truncated   uint64          `json:"truncated_bytes"`
	Events      int             `json:"events"`
	Private     bool            `json:"private_raw"`
}
type runCapture struct {
	diagnostics json.RawMessage
	dir         string
	file        *os.File
	meta        bool
	ended       bool
	complete    bool
	dropped     uint32
	truncated   uint64
	count       int
	last        uint32
	lastCall    uint32
	roots       map[uint32]uint32
	pending     *event
	code        []byte
	qr          bool
	private     bool
}

func writeJSON(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0600)
}
func recordType(line string) (string, []byte, bool) {
	for _, kind := range []string{"META", "EVENT", "BYTES", "RESULT", "END"} {
		marker := "H2_ALIPAY_" + kind + " "
		if i := strings.Index(line, marker); i >= 0 {
			return kind, []byte(line[i+len(marker):]), true
		}
	}
	return "", nil, false
}

// capture is an offline collector. It never opens a serial port, changes
// credentials or contacts a service. Raw byte files retain the observed bytes;
// HTTP/RPC content is not inferred from wire fragments or formatted SDK logs.
func capture(input io.Reader, output string, options captureOptions) (returnErr error) {
	if !shaPattern.MatchString(options.GitSHA) {
		return errors.New("invalid firmware Git SHA")
	}
	if err := os.Mkdir(output, 0700); err != nil {
		return fmt.Errorf("create a new output directory: %w", err)
	}
	captures := map[string]*runCapture{}
	received, rawCount, records := 0, 0, 0
	host := map[string]any{"collected_utc": time.Now().UTC(), "build_git_sha": options.GitSHA,
		"build_dirty": options.Dirty, "build_identity_source": "host_supplied_not_runtime_attested",
		"private_enabled": options.Private, "plaintext_http_available": false}
	if options.Package != "" {
		file, err := os.Open(options.Package)
		if err != nil {
			return err
		}
		hash := sha256.New()
		_, hashErr := io.Copy(hash, file)
		closeErr := file.Close()
		if hashErr != nil {
			return hashErr
		}
		if closeErr != nil {
			return closeErr
		}
		host["package_sha256"] = hex.EncodeToString(hash.Sum(nil))
	}
	if err := writeJSON(filepath.Join(output, "host.json"), host); err != nil {
		return err
	}
	defer func() {
		for _, run := range captures {
			if err := run.file.Close(); err != nil && returnErr == nil {
				returnErr = err
			}
		}
		for _, run := range captures {
			complete := returnErr == nil && run.ended && run.complete && run.pending == nil
			status := map[string]any{"events_complete": complete,
				"trace_end_received": run.ended, "dropped": run.dropped, "truncated_bytes": run.truncated,
				"event_count": run.count, "capture_error": returnErr != nil,
				"e2e_acceptance":  "use independent H2_E2E_CASE, canonical SUMMARY and first matching replay",
				"raw_complete":    run.private && complete && run.truncated == 0,
				"qr_file_written": run.qr}
			if len(run.diagnostics) != 0 {
				status["diagnostics"] = run.diagnostics
			}
			if err := writeJSON(filepath.Join(run.dir, "integrity.json"), status); err != nil && returnErr == nil {
				returnErr = err
			}
		}
	}()
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 4096), 8192)
	for scanner.Scan() {
		line := scanner.Text()
		received += len(line) + 1
		if received > maxInput {
			return errors.New("input byte limit exceeded")
		}
		kind, data, ok := recordType(line)
		if !ok {
			continue
		}
		records++
		if records > maxRecords {
			return errors.New("trace record limit exceeded")
		}
		var e event
		if err := json.Unmarshal(data, &e); err != nil {
			return fmt.Errorf("invalid %s JSON: %w", kind, err)
		}
		if !runPattern.MatchString(e.Run) || !casePattern.MatchString(e.Case) {
			return errors.New("invalid runtime run/case identity")
		}
		key := e.Run + "-" + strings.ReplaceAll(e.Case, "/", "--")
		run := captures[key]
		if run == nil {
			if kind != "META" {
				return errors.New("trace data precedes run metadata")
			}
			dir := filepath.Join(output, key)
			if err := os.Mkdir(dir, 0700); err != nil {
				return err
			}
			file, err := os.OpenFile(filepath.Join(dir, "events.jsonl"), os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
			if err != nil {
				return err
			}
			run = &runCapture{dir: dir, file: file, roots: map[uint32]uint32{}}
			captures[key] = run
		}
		if run.ended {
			return errors.New("trace record follows its terminal")
		}
		if run.pending != nil && kind != "BYTES" {
			return errors.New("raw fragment missing after event")
		}
		switch kind {
		case "META":
			if run.meta {
				return errors.New("duplicate runtime metadata")
			}
			if e.Source != "vendor_sdk" && e.Source != "independent" {
				return errors.New("unsupported runtime capture source")
			}
			run.meta = true
			run.private = e.Private
			if e.Private && !options.Private {
				return errors.New("private runtime trace requires explicit -private")
			}
			if err := os.WriteFile(filepath.Join(run.dir, "run.json"), append(data, '\n'), 0600); err != nil {
				return err
			}
		case "EVENT":
			if (e.Source != "runtime_vendor_sdk" && e.Source != "runtime_product" && e.Source != "runtime_independent") || e.Seq != run.last+1 || e.Call == 0 || e.Parent >= e.Call || e.Captured < 0 || e.Captured > 128 || e.Length < e.Captured {
				return errors.New("invalid event order, source, identity or length")
			}
			if e.Boundary == "wire_below_vendor_tls" && e.Connection == 0 {
				return errors.New("wire event lacks connection identity")
			}
			if e.Captured > 0 && !options.Private {
				return errors.New("raw event requires -private")
			}
			if e.Call <= run.lastCall {
				return errors.New("non-monotonic call identity")
			}
			root := e.Call
			if e.Parent != 0 {
				var known bool
				root, known = run.roots[e.Parent]
				if !known {
					return errors.New("orphan call parent")
				}
			}
			run.roots[e.Call] = root
			run.lastCall = e.Call
			run.last = e.Seq
			run.count++
			if e.Captured > 0 {
				copy := e
				run.pending = &copy
			}
		case "BYTES":
			p := run.pending
			if !options.Private || p == nil || e.Seq != p.Seq {
				return errors.New("unmatched private fragment")
			}
			raw, err := hex.DecodeString(e.Hex)
			if err != nil || len(raw) != p.Captured {
				return errors.New("invalid raw fragment")
			}
			rawCount += len(raw)
			if rawCount > maxRaw {
				return errors.New("raw capture limit exceeded")
			}
			if p.Captured == p.Length {
				digest := sha256.Sum256(raw)
				if hex.EncodeToString(digest[:]) != p.SHA {
					return errors.New("fragment digest mismatch")
				}
			}
			name := fmt.Sprintf("call-%08d-conn-%08d-seq-%08d.bin", p.Call, p.Connection, p.Seq)
			if err := os.WriteFile(filepath.Join(run.dir, name), raw, 0600); err != nil {
				return err
			}
			index := map[string]any{"file": name, "request_id": run.roots[p.Call], "parent_id": p.Parent, "call_id": p.Call, "connection_id": p.Connection,
				"seq": p.Seq, "boundary": p.Boundary, "operation": p.Operation, "item": p.Item,
				"length": p.Length, "captured": p.Captured, "truncated": p.Captured != p.Length,
				"sha256_observed": p.SHA,
				"sha256_captured": fmt.Sprintf("%x", sha256.Sum256(raw)), "content_type": "unknown; observed bytes only", "reassembly": "ordered syscall fragments, not HTTP messages"}
			if err := writeJSON(filepath.Join(run.dir, strings.TrimSuffix(name, ".bin")+".json"), index); err != nil {
				return err
			}
			if p.Operation == "qr.rendered.fragment" {
				if p.Boundary != "application_code" || int(p.Item) != len(run.code) || p.Length != p.Captured || len(run.code)+len(raw) >= 256 {
					return errors.New("invalid QR code fragment order or length")
				}
				run.code = append(run.code, raw...)
			}
			run.pending = nil
		case "RESULT":
			var result struct {
				Length int  `json:"length"`
				QR     bool `json:"qr_visible"`
				CodeRC int  `json:"code_rc"`
			}
			if err := json.Unmarshal(data, &result); err != nil {
				return err
			}
			if options.Private && result.QR && result.CodeRC == 0 {
				if result.Length != len(run.code) || len(run.code) == 0 || bytes.IndexByte(run.code, 0) >= 0 {
					return errors.New("displayed QR lacks complete raw code evidence")
				}
				path := filepath.Join(run.dir, "binding-code.txt")
				if err := os.WriteFile(path, run.code, 0600); err != nil {
					return err
				}
				if options.Renderer != "" {
					if !filepath.IsAbs(options.Renderer) {
						return errors.New("QR renderer must be an absolute executable path")
					}
					svg, err := exec.Command(options.Renderer, path).Output()
					if err != nil {
						return fmt.Errorf("QR renderer: %w", err)
					}
					if len(svg) > 1<<20 {
						return errors.New("QR renderer output too large")
					}
					if err := os.WriteFile(filepath.Join(run.dir, "binding-code.svg"), svg, 0600); err != nil {
						return err
					}
					run.qr = true
				}
			}
		case "END":
			run.diagnostics = e.Diagnostics
			run.ended = true
			run.dropped = e.Dropped
			run.truncated = e.Truncated
			run.complete = e.Complete && e.Dropped == 0 && e.Events == run.count
		}
		// Preserve event metadata verbatim in an envelope. Private hex remains only
		// in the explicit local capture; raw binary is separately indexed above.
		envelope := append([]byte(`{"type":"`+kind+`","record":`), data...)
		envelope = append(envelope, '}', '\n')
		if _, err := run.file.Write(envelope); err != nil {
			return err
		}
	}
	if err := scanner.Err(); err != nil {
		return err
	}
	if len(captures) == 0 {
		return errors.New("no Alipay runtime records found")
	}
	for _, run := range captures {
		if !run.ended || !run.complete || run.pending != nil {
			return errors.New("incomplete runtime trace; inspect local integrity.json")
		}
	}
	return nil
}
