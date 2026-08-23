package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestGenerateAndCheck(t *testing.T) {
	t.Parallel()
	output := filepath.Join(t.TempDir(), "generated")
	options := generateOptions{inputPath: "testdata/embedded_subset.openapi.json", outputPath: output, packageName: "fixture"}
	if err := generate(options); err != nil {
		t.Fatal(err)
	}
	firstHeader, err := os.ReadFile(filepath.Join(output, "h2_fixture_api.h"))
	if err != nil {
		t.Fatal(err)
	}
	firstManifest, err := os.ReadFile(filepath.Join(output, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	if err := generate(options); err != nil {
		t.Fatal(err)
	}
	secondHeader, err := os.ReadFile(filepath.Join(output, "h2_fixture_api.h"))
	if err != nil {
		t.Fatal(err)
	}
	secondManifest, err := os.ReadFile(filepath.Join(output, manifestName))
	if err != nil {
		t.Fatal(err)
	}
	if string(firstHeader) != string(secondHeader) || string(firstManifest) != string(secondManifest) {
		t.Fatal("repeated generation was not byte-for-byte deterministic")
	}
	options.check = true
	if err := generate(options); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(output, "unexpected.txt"), []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := generate(options); err == nil || !strings.Contains(err.Error(), "unexpected output entry") {
		t.Fatalf("got %v, want unexpected entry", err)
	}
	options.check = false
	if err := generate(options); err == nil || !strings.Contains(err.Error(), "unexpected file set") {
		t.Fatalf("got %v, want unsafe replacement rejection", err)
	}
}

func TestWriteDeclaredOutputs(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	header := filepath.Join(root, "include", "h2_fixture_api.h")
	source := filepath.Join(root, "src", "h2_fixture_api.c")
	if err := writeDeclaredOutputs(
		"testdata/embedded_subset.openapi.json",
		"fixture",
		header,
		source,
	); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{header, source} {
		contents, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if len(contents) == 0 {
			t.Fatalf("declared output %s is empty", path)
		}
	}
}

func TestGenerateRefusesUnmanagedDirectory(t *testing.T) {
	t.Parallel()
	output := filepath.Join(t.TempDir(), "generated")
	if err := os.Mkdir(output, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(output, "user.txt"), []byte("keep"), 0o600); err != nil {
		t.Fatal(err)
	}
	err := generate(generateOptions{inputPath: "testdata/embedded_subset.openapi.json", outputPath: output, packageName: "fixture"})
	if err == nil || !strings.Contains(err.Error(), "unmanaged output") {
		t.Fatalf("got %v, want unmanaged output rejection", err)
	}
	data, readErr := os.ReadFile(filepath.Join(output, "user.txt"))
	if readErr != nil || string(data) != "keep" {
		t.Fatalf("unmanaged file changed: data=%q err=%v", data, readErr)
	}
}

func TestCheckDoesNotCreateOutput(t *testing.T) {
	t.Parallel()
	output := filepath.Join(t.TempDir(), "missing")
	err := generate(generateOptions{inputPath: "testdata/embedded_subset.openapi.json", outputPath: output, packageName: "fixture", check: true})
	if err == nil {
		t.Fatal("check unexpectedly succeeded")
	}
	if _, statErr := os.Stat(output); !os.IsNotExist(statErr) {
		t.Fatalf("check created output: %v", statErr)
	}
}

func TestGenerateRejectsOutputSymlink(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	target := filepath.Join(root, "target")
	if err := os.Mkdir(target, 0o700); err != nil {
		t.Fatal(err)
	}
	output := filepath.Join(root, "generated")
	if err := os.Symlink(target, output); err != nil {
		t.Fatal(err)
	}
	err := generate(generateOptions{inputPath: "testdata/embedded_subset.openapi.json", outputPath: output, packageName: "fixture"})
	if err == nil || !strings.Contains(err.Error(), "real directory") {
		t.Fatalf("got %v, want symlink rejection", err)
	}
}

func TestGenerateRejectsManifestPackageMismatch(t *testing.T) {
	t.Parallel()
	output := filepath.Join(t.TempDir(), "generated")
	if err := generate(generateOptions{inputPath: "testdata/embedded_subset.openapi.json", outputPath: output, packageName: "fixture"}); err != nil {
		t.Fatal(err)
	}
	err := generate(generateOptions{inputPath: "testdata/embedded_subset.openapi.json", outputPath: output, packageName: "animals"})
	if err == nil || !strings.Contains(err.Error(), "does not match") {
		t.Fatalf("got %v, want package mismatch", err)
	}
}

func TestCheckRejectsTrailingManifestJSON(t *testing.T) {
	t.Parallel()
	output := filepath.Join(t.TempDir(), "generated")
	options := generateOptions{inputPath: "testdata/embedded_subset.openapi.json", outputPath: output, packageName: "fixture"}
	if err := generate(options); err != nil {
		t.Fatal(err)
	}
	manifestPath := filepath.Join(output, manifestName)
	file, err := os.OpenFile(manifestPath, os.O_APPEND|os.O_WRONLY, 0)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := file.WriteString("{}\n"); err != nil {
		_ = file.Close()
		t.Fatal(err)
	}
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	options.check = true
	if err := generate(options); err == nil || !strings.Contains(err.Error(), "trailing JSON") {
		t.Fatalf("got %v, want trailing JSON rejection", err)
	}
}

func TestCheckRejectsStaleEditedAndMissingOutput(t *testing.T) {
	t.Parallel()
	for _, mode := range []string{"schema", "edited", "missing"} {
		mode := mode
		t.Run(mode, func(t *testing.T) {
			t.Parallel()
			root := t.TempDir()
			output := filepath.Join(root, "generated")
			options := generateOptions{inputPath: "testdata/embedded_subset.openapi.json", outputPath: output, packageName: "fixture"}
			if err := generate(options); err != nil {
				t.Fatal(err)
			}
			switch mode {
			case "schema":
				data, err := os.ReadFile(options.inputPath)
				if err != nil {
					t.Fatal(err)
				}
				changed := strings.Replace(string(data), `"maxLength": 32`, `"maxLength": 31`, 1)
				options.inputPath = filepath.Join(root, "changed.json")
				if err := os.WriteFile(options.inputPath, []byte(changed), 0o600); err != nil {
					t.Fatal(err)
				}
			case "edited":
				if err := os.WriteFile(filepath.Join(output, "h2_fixture_api.h"), []byte("edited\n"), 0o600); err != nil {
					t.Fatal(err)
				}
			case "missing":
				if err := os.Remove(filepath.Join(output, "h2_fixture_api.c")); err != nil {
					t.Fatal(err)
				}
			}
			options.check = true
			if err := generate(options); err == nil {
				t.Fatalf("check unexpectedly accepted %s output", mode)
			}
		})
	}
}

func TestRunDiagnosticIncludesSchemaAndJSONPointer(t *testing.T) {
	t.Parallel()
	schema := filepath.Join("testdata", "unsupported", "remote_ref.json")
	err := run([]string{
		"--schema", schema,
		"--output", filepath.Join(t.TempDir(), "generated"),
		"--package", "test",
	})
	if err == nil || !strings.Contains(err.Error(), schema) || !strings.Contains(err.Error(), "/components/schemas/Remote/$ref") {
		t.Fatalf("got %v, want schema path and JSON pointer", err)
	}
}
