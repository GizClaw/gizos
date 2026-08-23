package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const (
	manifestName    = ".h2_openapi_codegen.json"
	manifestVersion = 1
)

type generateOptions struct {
	inputPath   string
	outputPath  string
	packageName string
	check       bool
}

type outputManifest struct {
	FormatVersion int      `json:"format_version"`
	Package       string   `json:"package"`
	SchemaDigest  string   `json:"schema_digest"`
	Files         []string `json:"files"`
}

func generate(options generateOptions) error {
	specification, files, err := render(options.inputPath, options.packageName)
	if err != nil {
		return err
	}
	manifest, err := makeManifest(specification, files)
	if err != nil {
		return err
	}
	if options.check {
		return checkOutput(options.outputPath, manifest, files)
	}
	return writeOutput(options.outputPath, manifest, files)
}

func render(inputPath string, packageName string) (*apiSpec, map[string][]byte, error) {
	specification, err := loadSpec(inputPath, packageName)
	if err != nil {
		return nil, nil, fmt.Errorf("schema %s: %w", inputPath, err)
	}
	files, err := emit(specification)
	if err != nil {
		return nil, nil, fmt.Errorf("emit C client: %w", err)
	}
	return specification, files, nil
}

func writeDeclaredOutputs(inputPath string, packageName string, headerPath string, sourcePath string) error {
	_, files, err := render(inputPath, packageName)
	if err != nil {
		return err
	}
	outputs := map[string]string{
		"h2_" + packageName + "_api.h": headerPath,
		"h2_" + packageName + "_api.c": sourcePath,
	}
	for name, path := range outputs {
		data, ok := files[name]
		if !ok {
			return fmt.Errorf("generated output %q is missing", name)
		}
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return fmt.Errorf("create declared output parent: %w", err)
		}
		if err := os.WriteFile(path, data, 0o644); err != nil {
			return fmt.Errorf("write declared output %q: %w", path, err)
		}
	}
	return nil
}

func makeManifest(specification *apiSpec, files map[string][]byte) (outputManifest, error) {
	normalized, err := json.Marshal(specification)
	if err != nil {
		return outputManifest{}, fmt.Errorf("normalize schema: %w", err)
	}
	digest := sha256.Sum256(normalized)
	fileNames := make([]string, 0, len(files))
	for name := range files {
		fileNames = append(fileNames, name)
	}
	sort.Strings(fileNames)
	return outputManifest{
		FormatVersion: manifestVersion,
		Package:       specification.Package,
		SchemaDigest:  hex.EncodeToString(digest[:]),
		Files:         fileNames,
	}, nil
}

func checkOutput(outputPath string, expected outputManifest, files map[string][]byte) error {
	actual, err := readValidManifest(outputPath, expected.Package)
	if err != nil {
		return err
	}
	if actual.FormatVersion != expected.FormatVersion || actual.SchemaDigest != expected.SchemaDigest || !equalStrings(actual.Files, expected.Files) {
		return fmt.Errorf("generated output is stale")
	}
	entries, err := os.ReadDir(outputPath)
	if err != nil {
		return fmt.Errorf("read output directory: %w", err)
	}
	expectedEntries := make(map[string]bool, len(files)+1)
	expectedEntries[manifestName] = true
	for name := range files {
		expectedEntries[name] = true
	}
	for _, entry := range entries {
		if entry.Type()&os.ModeSymlink != 0 || entry.IsDir() || !expectedEntries[entry.Name()] {
			return fmt.Errorf("unexpected output entry %q", entry.Name())
		}
	}
	if len(entries) != len(expectedEntries) {
		return fmt.Errorf("generated output is missing files")
	}
	for name, expectedData := range files {
		actualData, err := os.ReadFile(filepath.Join(outputPath, name))
		if err != nil {
			return fmt.Errorf("read generated file %q: %w", name, err)
		}
		if string(actualData) != string(expectedData) {
			return fmt.Errorf("generated file %q is stale", name)
		}
	}
	return nil
}

func writeOutput(outputPath string, manifest outputManifest, files map[string][]byte) error {
	absoluteOutput, err := filepath.Abs(outputPath)
	if err != nil {
		return fmt.Errorf("resolve output path: %w", err)
	}
	parent := filepath.Dir(absoluteOutput)
	if err := os.MkdirAll(parent, 0o755); err != nil {
		return fmt.Errorf("create output parent: %w", err)
	}
	if filepath.Dir(absoluteOutput) == absoluteOutput {
		return fmt.Errorf("output must not be a filesystem root")
	}
	exists, err := validateWritableOutput(absoluteOutput, manifest)
	if err != nil {
		return err
	}
	temporary, err := os.MkdirTemp(parent, ".h2-openapi-codegen-")
	if err != nil {
		return fmt.Errorf("create temporary output: %w", err)
	}
	keepTemporary := false
	defer func() {
		if !keepTemporary {
			_ = os.RemoveAll(temporary)
		}
	}()
	for name, data := range files {
		if filepath.Base(name) != name || strings.HasPrefix(name, ".") {
			return fmt.Errorf("invalid generated file name %q", name)
		}
		if err := os.WriteFile(filepath.Join(temporary, name), data, 0o644); err != nil {
			return fmt.Errorf("write generated file %q: %w", name, err)
		}
	}
	manifestData, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return fmt.Errorf("encode manifest: %w", err)
	}
	manifestData = append(manifestData, '\n')
	if err := os.WriteFile(filepath.Join(temporary, manifestName), manifestData, 0o644); err != nil {
		return fmt.Errorf("write manifest: %w", err)
	}
	if err := checkOutput(temporary, manifest, files); err != nil {
		return fmt.Errorf("verify temporary generated output: %w", err)
	}
	if !exists {
		if err := os.Rename(temporary, absoluteOutput); err != nil {
			return fmt.Errorf("install generated output: %w", err)
		}
		keepTemporary = true
		return nil
	}

	backup, err := unusedSiblingPath(absoluteOutput + ".backup")
	if err != nil {
		return err
	}
	if err := os.Rename(absoluteOutput, backup); err != nil {
		return fmt.Errorf("preserve previous output: %w", err)
	}
	if err := os.Rename(temporary, absoluteOutput); err != nil {
		restoreErr := os.Rename(backup, absoluteOutput)
		if restoreErr != nil {
			return errors.Join(fmt.Errorf("install generated output: %w", err), fmt.Errorf("restore previous output: %w", restoreErr))
		}
		return fmt.Errorf("install generated output: %w", err)
	}
	keepTemporary = true
	if err := os.RemoveAll(backup); err != nil {
		return fmt.Errorf("remove previous generated output backup %q: %w", backup, err)
	}
	return nil
}

func validateWritableOutput(path string, expected outputManifest) (bool, error) {
	information, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("inspect output path: %w", err)
	}
	if information.Mode()&os.ModeSymlink != 0 || !information.IsDir() {
		return false, fmt.Errorf("output must be a real directory")
	}
	entries, err := os.ReadDir(path)
	if err != nil {
		return false, fmt.Errorf("read output directory: %w", err)
	}
	if len(entries) == 0 {
		return true, nil
	}
	manifest, err := readValidManifest(path, expected.Package)
	if err != nil {
		return false, fmt.Errorf("refuse to replace unmanaged output: %w", err)
	}
	if !equalStrings(manifest.Files, expected.Files) {
		return false, fmt.Errorf("refuse to replace managed output with a mismatched generated file set")
	}
	expectedEntries := make(map[string]bool, len(manifest.Files)+1)
	expectedEntries[manifestName] = true
	for _, name := range manifest.Files {
		expectedEntries[name] = true
	}
	if len(entries) != len(expectedEntries) {
		return false, fmt.Errorf("refuse to replace managed output with an unexpected file set")
	}
	for _, entry := range entries {
		if !expectedEntries[entry.Name()] || entry.Type()&os.ModeSymlink != 0 || !entry.Type().IsRegular() {
			return false, fmt.Errorf("refuse to replace managed output with unsafe entry %q", entry.Name())
		}
	}
	return true, nil
}

func readValidManifest(outputPath string, packageName string) (outputManifest, error) {
	information, err := os.Lstat(outputPath)
	if err != nil {
		return outputManifest{}, fmt.Errorf("inspect output directory: %w", err)
	}
	if information.Mode()&os.ModeSymlink != 0 || !information.IsDir() {
		return outputManifest{}, fmt.Errorf("output must be a real directory")
	}
	manifestPath := filepath.Join(outputPath, manifestName)
	manifestInfo, err := os.Lstat(manifestPath)
	if err != nil {
		return outputManifest{}, fmt.Errorf("read output manifest: %w", err)
	}
	if manifestInfo.Mode()&os.ModeSymlink != 0 || !manifestInfo.Mode().IsRegular() {
		return outputManifest{}, fmt.Errorf("output manifest must be a regular file")
	}
	if manifestInfo.Size() > 64*1024 {
		return outputManifest{}, fmt.Errorf("output manifest is too large")
	}
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return outputManifest{}, fmt.Errorf("read output manifest: %w", err)
	}
	var manifest outputManifest
	decoder := json.NewDecoder(strings.NewReader(string(data)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&manifest); err != nil {
		return outputManifest{}, fmt.Errorf("parse output manifest: %w", err)
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		if err == nil {
			return outputManifest{}, fmt.Errorf("parse output manifest: trailing JSON value")
		}
		return outputManifest{}, fmt.Errorf("parse output manifest: %w", err)
	}
	if manifest.FormatVersion != manifestVersion {
		return outputManifest{}, fmt.Errorf("manifest format version %d is unsupported", manifest.FormatVersion)
	}
	if manifest.Package != packageName {
		return outputManifest{}, fmt.Errorf("manifest package %q does not match %q", manifest.Package, packageName)
	}
	if len(manifest.SchemaDigest) != sha256.Size*2 {
		return outputManifest{}, fmt.Errorf("manifest schema digest is invalid")
	}
	if _, err := hex.DecodeString(manifest.SchemaDigest); err != nil {
		return outputManifest{}, fmt.Errorf("manifest schema digest is invalid: %w", err)
	}
	seen := make(map[string]bool)
	for _, name := range manifest.Files {
		if filepath.Base(name) != name || name == manifestName || seen[name] {
			return outputManifest{}, fmt.Errorf("manifest file set is invalid")
		}
		seen[name] = true
	}
	return manifest, nil
}

func unusedSiblingPath(prefix string) (string, error) {
	for index := 0; index < 1000; index++ {
		candidate := fmt.Sprintf("%s-%d", prefix, index)
		if _, err := os.Lstat(candidate); errors.Is(err, os.ErrNotExist) {
			return candidate, nil
		} else if err != nil {
			return "", fmt.Errorf("inspect backup path: %w", err)
		}
	}
	return "", fmt.Errorf("could not allocate output backup path")
}

func equalStrings(left []string, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	for index := range left {
		if left[index] != right[index] {
			return false
		}
	}
	return true
}
