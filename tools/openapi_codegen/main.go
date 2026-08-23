package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintf(os.Stderr, "openapi_codegen: %v\n", err)
		os.Exit(1)
	}
}

func run(arguments []string) error {
	flags := flag.NewFlagSet("openapi_codegen", flag.ContinueOnError)
	flags.SetOutput(os.Stderr)
	schema := flags.String("schema", "", "local OpenAPI 3.0 JSON document")
	output := flags.String("output", "", "dedicated generated output directory")
	headerOutput := flags.String("header-output", "", "declared generated header path")
	sourceOutput := flags.String("source-output", "", "declared generated source path")
	packageName := flags.String("package", "", "C package name used after the h2_ prefix")
	check := flags.Bool("check", false, "verify generated output without writing")
	if err := flags.Parse(arguments); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("unexpected positional arguments: %v", flags.Args())
	}
	if *schema == "" || *packageName == "" {
		return fmt.Errorf("--schema and --package are required")
	}
	resolvedInput := resolveWorkspacePath(*schema)
	if *headerOutput != "" || *sourceOutput != "" {
		if *output != "" || *check || *headerOutput == "" || *sourceOutput == "" {
			return fmt.Errorf("--header-output and --source-output must be used together without --output or --check")
		}
		return writeDeclaredOutputs(
			resolvedInput,
			*packageName,
			*headerOutput,
			*sourceOutput,
		)
	}
	if *output == "" {
		return fmt.Errorf("--output is required")
	}
	resolvedOutput := resolveWorkspacePath(*output)

	return generate(generateOptions{
		inputPath:   resolvedInput,
		outputPath:  resolvedOutput,
		packageName: *packageName,
		check:       *check,
	})
}

func resolveWorkspacePath(path string) string {
	workspace := os.Getenv("BUILD_WORKSPACE_DIRECTORY")
	if filepath.IsAbs(path) || workspace == "" {
		return path
	}
	if _, err := os.Stat(path); err == nil {
		return path
	}
	return filepath.Join(workspace, path)
}
