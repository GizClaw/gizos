package main

import (
	"flag"
	"fmt"
	"io"
	"os"
)

func main() {
	input := flag.String("in", "-", "H2Loader monitor log, or - for stdin; no serial/reset operations")
	output := flag.String("out", "", "new local private artifact directory (required)")
	private := flag.Bool("private", false, "explicitly permit private SDK log/wire/code bytes from this input")
	gitSHA := flag.String("firmware-git-sha", "", "actual firmware build checkout commit, 40 hex digits (host provenance)")
	dirty := flag.Bool("firmware-dirty", false, "the recorded build checkout contained uncommitted edits")
	image := flag.String("firmware-package", "", "optional exact built package to hash into run metadata")
	renderer := flag.String("qr-renderer", "", "optional absolute path of the qr_svg Bazel host binary")
	flag.Parse()
	if *output == "" || !shaPattern.MatchString(*gitSHA) {
		fmt.Fprintln(os.Stderr, "-out and a 40-digit -firmware-git-sha are required")
		os.Exit(2)
	}
	var in io.Reader = os.Stdin
	if *input != "-" {
		file, err := os.Open(*input)
		if err != nil {
			fail(err)
		}
		defer file.Close()
		in = file
	}
	options := captureOptions{Private: *private, GitSHA: *gitSHA, Dirty: *dirty, Package: *image, Renderer: *renderer}
	if err := capture(in, *output, options); err != nil {
		fail(err)
	}
	fmt.Fprintln(os.Stderr, "Saved trace artifacts; inspect integrity.json and the independent E2E result report.")
}
func fail(err error) { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
