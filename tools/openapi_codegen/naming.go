package main

import (
	"fmt"
	"regexp"
	"strings"
)

var packagePattern = regexp.MustCompile(`^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$`)

var cKeywords = map[string]bool{
	"auto": true, "break": true, "case": true, "char": true, "const": true,
	"continue": true, "default": true, "do": true, "double": true, "else": true,
	"enum": true, "extern": true, "float": true, "for": true, "goto": true,
	"if": true, "inline": true, "int": true, "long": true, "register": true,
	"restrict": true, "return": true, "short": true, "signed": true,
	"sizeof": true, "static": true, "struct": true, "switch": true,
	"typedef": true, "union": true, "unsigned": true, "void": true,
	"volatile": true, "while": true, "_alignas": true, "_alignof": true,
	"_atomic": true, "_bool": true, "_complex": true, "_generic": true,
	"_imaginary": true, "_noreturn": true, "_static_assert": true,
	"_thread_local": true,
	"bool":          true, "true": true, "false": true, "complex": true,
	"imaginary": true,
}

func validatePackageName(name string) error {
	if !packagePattern.MatchString(name) {
		return fmt.Errorf("package %q must match %s", name, packagePattern)
	}
	return nil
}

func cIdentifier(value string) string {
	var output strings.Builder
	previousUnderscore := false
	characters := []rune(value)
	for index, character := range characters {
		previousIsLowerOrDigit := index > 0 && (isASCIILower(characters[index-1]) || isASCIIDigit(characters[index-1]))
		acronymBoundary := index > 0 && index+1 < len(characters) && isASCIIUpper(characters[index-1]) && isASCIILower(characters[index+1])
		if isASCIIUpper(character) && output.Len() > 0 && !previousUnderscore && (previousIsLowerOrDigit || acronymBoundary) {
			output.WriteByte('_')
		}
		if isASCIIUpper(character) || isASCIILower(character) || isASCIIDigit(character) {
			if isASCIIUpper(character) {
				character += 'a' - 'A'
			}
			output.WriteRune(character)
			previousUnderscore = false
			continue
		}
		if !previousUnderscore && output.Len() > 0 {
			output.WriteByte('_')
			previousUnderscore = true
		}
	}
	result := strings.Trim(output.String(), "_")
	if result == "" {
		return "value"
	}
	if result[0] >= '0' && result[0] <= '9' {
		return "value_" + result
	}
	if cKeywords[result] {
		return "value_" + result
	}
	return result
}

func isASCIIUpper(value rune) bool { return value >= 'A' && value <= 'Z' }

func isASCIILower(value rune) bool { return value >= 'a' && value <= 'z' }

func isASCIIDigit(value rune) bool { return value >= '0' && value <= '9' }

func cMacro(value string) string {
	return strings.ToUpper(cIdentifier(value))
}
