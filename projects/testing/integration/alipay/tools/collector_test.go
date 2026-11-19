package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

const testRun = "0123456789abcdef0123456789abcdef"
const testCase = "alipay/binding-code"

func row(kind string, fields map[string]any) string {
	return rowForRun(testRun, kind, fields)
}
func rowForRun(run, kind string, fields map[string]any) string {
	fields["run_id"] = run
	fields["case_id"] = testCase
	data, _ := json.Marshal(fields)
	return "H2_ALIPAY_" + kind + " " + string(data) + "\n"
}

func TestSuccessiveBindingCaptures(t *testing.T) {
	runs := []string{"112ac280cda01f4cd59e4150e2be4230", "8e87288ec670948426d95202777216b2"}
	var input strings.Builder
	for index, run := range runs {
		input.WriteString(rowForRun(run, "META", map[string]any{"source": "independent", "private_raw": false}))
		for sequence := 1; sequence <= index+1; sequence++ {
			input.WriteString(rowForRun(run, "EVENT", map[string]any{
				"source": "runtime_independent", "seq": sequence, "call_id": sequence,
				"operation": "independent.binding_query", "boundary": "independent",
			}))
		}
		input.WriteString(rowForRun(run, "RESULT", map[string]any{"binding_state": 3}))
		input.WriteString(rowForRun(run, "END", map[string]any{"events": index + 1, "complete": true}))
		fmt.Fprintf(&input, "H2_ALIPAY_E2E_SUMMARY run_id=%s replay=false\n", run)
		fmt.Fprintf(&input, "H2_ALIPAY_E2E_SUMMARY run_id=%s replay=true\n", run)
	}
	out := dir(t)
	if err := capture(strings.NewReader(input.String()), out, opts(false)); err != nil {
		t.Fatal(err)
	}
	for index, run := range runs {
		folder := filepath.Join(out, run+"-alipay--binding-code")
		data, err := os.ReadFile(filepath.Join(folder, "integrity.json"))
		if err != nil {
			t.Fatal(err)
		}
		var status struct {
			Complete bool `json:"events_complete"`
			Count    int  `json:"event_count"`
			Error    bool `json:"capture_error"`
		}
		if err := json.Unmarshal(data, &status); err != nil {
			t.Fatal(err)
		}
		if !status.Complete || status.Count != index+1 || status.Error {
			t.Fatalf("capture %s integrity: %s", run, data)
		}
		metadata, err := os.ReadFile(filepath.Join(folder, "run.json"))
		if err != nil {
			t.Fatal(err)
		}
		var identity struct {
			Run string `json:"run_id"`
		}
		if err := json.Unmarshal(metadata, &identity); err != nil || identity.Run != run {
			t.Fatalf("capture identity: %s, %v", metadata, err)
		}
	}
}

func TestRejectsReusedCaptureIdentityAfterEnd(t *testing.T) {
	snapshot := meta(false) + eventRow(1, nil) +
		row("END", map[string]any{"events": 1, "complete": true})
	err := capture(strings.NewReader(snapshot+snapshot), dir(t), opts(false))
	if err == nil || !strings.Contains(err.Error(), "trace record follows its terminal") {
		t.Fatalf("reused capture identity: %v", err)
	}
}
func meta(private bool) string {
	return row("META", map[string]any{"source": "independent", "private_raw": private})
}
func opts(private bool) captureOptions {
	return captureOptions{Private: private, GitSHA: strings.Repeat("a", 40)}
}
func dir(t *testing.T) string { return filepath.Join(t.TempDir(), "capture") }
func eventRow(seq int, raw []byte) string {
	hash := sha256.Sum256(raw)
	return row("EVENT", map[string]any{"source": "runtime_independent", "seq": seq, "call_id": seq,
		"parent_id": 0, "connection_id": 0, "operation": "qr.rendered.fragment", "boundary": "application_code",
		"item": 0, "length": len(raw), "captured": len(raw), "sha256": hex.EncodeToString(hash[:])})
}
func TestPrivateBytesAndCodeAreExact(t *testing.T) {
	raw := []byte("controlled-code-fixture")
	input := meta(true) + eventRow(1, raw) + row("BYTES", map[string]any{"seq": 1, "hex": hex.EncodeToString(raw)}) +
		row("RESULT", map[string]any{"length": len(raw), "qr_visible": true, "code_rc": 0}) +
		row("END", map[string]any{"events": 1, "complete": true, "dropped": 0})
	out := dir(t)
	if err := capture(strings.NewReader(input), out, opts(true)); err != nil {
		t.Fatal(err)
	}
	run := filepath.Join(out, testRun+"-alipay--binding-code")
	got, err := os.ReadFile(filepath.Join(run, "binding-code.txt"))
	if err != nil || string(got) != string(raw) {
		t.Fatalf("code bytes: %q %v", got, err)
	}
	index, err := os.ReadFile(filepath.Join(run, "call-00000001-conn-00000000-seq-00000001.bin"))
	if err != nil || string(index) != string(raw) {
		t.Fatal("raw bytes changed")
	}
	if runtime.GOOS != "windows" {
		info, err := os.Stat(filepath.Join(run, "binding-code.txt"))
		if err != nil || info.Mode().Perm() != 0600 {
			t.Fatalf("private file permissions: %v %v", info, err)
		}
	}
	data, err := os.ReadFile(filepath.Join(run, "events.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	for _, line := range strings.Split(strings.TrimSpace(string(data)), "\n") {
		if !json.Valid([]byte(line)) {
			t.Fatalf("bad event envelope %s", line)
		}
	}
}
func TestRejectsPrivateWithoutOptIn(t *testing.T) {
	if err := capture(strings.NewReader(meta(true)), dir(t), opts(false)); err == nil {
		t.Fatal("private capture accepted")
	}
}

func TestIndependentSourceRemainsExplicit(t *testing.T) {
	hash := sha256.Sum256(nil)
	input := row("META", map[string]any{"source": "independent", "private_raw": false}) +
		row("EVENT", map[string]any{
			"source": "runtime_independent", "seq": 1, "call_id": 1, "parent_id": 0,
			"operation": "independent.binding_code.return", "boundary": "independent",
			"length": 0, "captured": 0, "sha256": hex.EncodeToString(hash[:]),
		}) + row("END", map[string]any{"events": 1, "complete": true, "dropped": 0})
	out := dir(t)
	if err := capture(strings.NewReader(input), out, opts(false)); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(out, testRun+"-alipay--binding-code", "events.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), `"source":"runtime_independent"`) ||
		strings.Contains(string(data), `"source":"runtime_product"`) {
		t.Fatal("independent evidence was relabeled")
	}
}

func TestIntegrityRejectsOrderSourceAndMissingEnd(t *testing.T) {
	for name, input := range map[string]string{
		"gap":          meta(false) + eventRow(2, nil),
		"wrong-source": row("META", map[string]any{"source": "controlled"}),
		"missing-end":  meta(false) + eventRow(1, nil),
		"tampered":     meta(true) + eventRow(1, []byte("a")) + row("BYTES", map[string]any{"seq": 1, "hex": "62"}),
		"traversal":    "H2_ALIPAY_META {\"run_id\":\"../bad\",\"case_id\":\"alipay/binding-code\"}\n",
	} {
		t.Run(name, func(t *testing.T) {
			if err := capture(strings.NewReader(input), dir(t), opts(true)); err == nil {
				t.Fatal("invalid trace accepted")
			}
		})
	}
}
func TestSummaryAndTruncationRemainDistinct(t *testing.T) {
	input := meta(false) + eventRow(1, nil) + row("END", map[string]any{"events": 1, "complete": true, "dropped": 0})
	out := dir(t)
	if err := capture(strings.NewReader(input), out, opts(false)); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(out, testRun+"-alipay--binding-code", "integrity.json"))
	if err != nil {
		t.Fatal(err)
	}
	var status map[string]any
	if err = json.Unmarshal(data, &status); err != nil {
		t.Fatal(err)
	}
	if status["events_complete"] != true || status["raw_complete"] != false {
		t.Fatal(fmt.Sprint(status))
	}
}

func TestFailureFinalizesIncompleteArtifact(t *testing.T) {
	out := dir(t)
	err := capture(strings.NewReader(meta(false)+eventRow(1, nil)), out, opts(false))
	if err == nil {
		t.Fatal("missing end accepted")
	}
	data, readErr := os.ReadFile(filepath.Join(out, testRun+"-alipay--binding-code", "integrity.json"))
	if readErr != nil {
		t.Fatal(readErr)
	}
	var status map[string]any
	if err := json.Unmarshal(data, &status); err != nil {
		t.Fatal(err)
	}
	if status["events_complete"] != false || status["raw_complete"] != false || status["capture_error"] != true {
		t.Fatal("failed export was labeled complete")
	}
}

func TestDiagnosticSamplingIsExplicit(t *testing.T) {
	diagnostics := map[string]any{"limit": 16,
		"vendor_logs": map[string]any{"seen": 10000, "summarized": 9984}}
	input := meta(false) + eventRow(1, nil) + row("END", map[string]any{
		"events": 1, "complete": true, "dropped": 0, "diagnostics": diagnostics})
	out := dir(t)
	if err := capture(strings.NewReader(input), out, opts(false)); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(out, testRun+"-alipay--binding-code", "integrity.json"))
	if err != nil {
		t.Fatal(err)
	}
	var status struct {
		Complete    bool `json:"events_complete"`
		Diagnostics struct {
			Limit  int                            `json:"limit"`
			Vendor struct{ Seen, Summarized int } `json:"vendor_logs"`
		} `json:"diagnostics"`
	}
	if err := json.Unmarshal(data, &status); err != nil {
		t.Fatal(err)
	}
	if !status.Complete || status.Diagnostics.Limit != 16 ||
		status.Diagnostics.Vendor.Seen != 10000 || status.Diagnostics.Vendor.Summarized != 9984 {
		t.Fatalf("diagnostic summary lost: %s", data)
	}
}
