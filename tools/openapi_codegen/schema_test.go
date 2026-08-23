package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/getkin/kin-openapi/openapi3"
)

func TestLoadSpec(t *testing.T) {
	t.Parallel()
	specification, err := loadSpec("testdata/embedded_subset.openapi.json", "fixture")
	if err != nil {
		t.Fatal(err)
	}
	if len(specification.Operations) != 3 ||
		specification.Operations[0].Name != "list_items" ||
		specification.Operations[1].Name != "update_pet" ||
		specification.Operations[2].Name != "upload" {
		t.Fatalf("unexpected operations: %#v", specification.Operations)
	}
	if len(specification.Models) != 8 || specification.Models[2].Name != "error" || specification.Models[4].Name != "pet" {
		t.Fatalf("unexpected reachable models: %#v", specification.Models)
	}
}

func TestLoadSpecRejectsUnsupportedSchemas(t *testing.T) {
	t.Parallel()
	cases := map[string]string{
		"openapi31.json":                   "unsupported",
		"remote_ref.json":                  "local relative file or fragment",
		"additional_properties_false.json": "additionalProperties",
		"cyclic_schema.json":               "cyclic schema model",
		"unsafe_integer.json":              "inclusive integral minimum/maximum",
	}
	for name, expected := range cases {
		name := name
		expected := expected
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			_, err := loadSpec(filepath.Join("testdata", "unsupported", name), "test")
			if err == nil || !strings.Contains(err.Error(), expected) {
				t.Fatalf("got %v, want error containing %q", err, expected)
			}
		})
	}
}

func TestValidateReferenceTreeRejectsEscape(t *testing.T) {
	t.Parallel()
	root := t.TempDir()
	outside := filepath.Join(filepath.Dir(root), filepath.Base(root)+"-outside.json")
	if err := os.WriteFile(outside, []byte(`{"type":"object"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Remove(outside) })
	input := filepath.Join(root, "source.json")
	data := `{"openapi":"3.0.3","info":{"title":"x","version":"1"},"paths":{},"components":{"schemas":{"x":{"$ref":"../` + filepath.Base(outside) + `"}}}}`
	if err := os.WriteFile(input, []byte(data), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := validateReferenceTree(input); err == nil || !strings.Contains(err.Error(), "escapes schema root") || !strings.Contains(err.Error(), "/components/schemas/x/$ref") {
		t.Fatalf("got %v, want schema-root escape", err)
	}
}

func TestValidatePublicIdentifiersRejectsCollisions(t *testing.T) {
	t.Parallel()
	cases := []struct {
		models     map[string]*modelSpec
		operations []operation
	}{
		{models: map[string]*modelSpec{"client": {Name: "client"}}},
		{models: map[string]*modelSpec{"pet": {Name: "pet"}}, operations: []operation{{Name: "pet"}}},
		{operations: []operation{{Name: "get_pet"}, {Name: "get_pet_response_deinit"}}},
	}
	for _, testCase := range cases {
		if err := validatePublicIdentifiers(testCase.models, testCase.operations); err == nil {
			t.Fatalf("collision unexpectedly accepted: %#v", testCase)
		}
	}
}

func TestTypeFromSchemaRejectsUnimplementedConstraints(t *testing.T) {
	t.Parallel()
	builder := &schemaBuilder{models: make(map[string]*modelSpec), visiting: make(map[*openapi3.Schema]bool), modelSource: make(map[string]*openapi3.Schema)}
	stringSchema := openapi3.NewStringSchema().WithMaxLength(16).WithPattern("[a-z]+")
	arraySchema := openapi3.NewArraySchema().WithItems(openapi3.NewBoolSchema()).WithMaxItems(4).WithUniqueItems(true)
	integerSchema := openapi3.NewInt64Schema().WithMin(-10).WithMax(10).WithExclusiveMin(true)
	for _, schema := range []*openapi3.Schema{stringSchema, arraySchema, integerSchema} {
		if _, err := builder.typeFromSchema(schema.NewRef(), "value"); err == nil {
			t.Fatalf("constraint unexpectedly accepted: %#v", schema)
		}
	}
}

func TestLoadSpecSupportsPerStatusBodyContracts(t *testing.T) {
	t.Parallel()
	path := writeTestSchema(t, `{
        "openapi":"3.0.3",
        "info":{"title":"x","version":"1"},
        "paths":{"/value":{"get":{"operationId":"getValue","responses":{
            "200":{"description":"value","content":{"application/json":{"schema":{"type":"object","properties":{}}}}},
            "204":{"description":"empty"}
        }}}}
    }`)
	specification, err := loadSpec(path, "test")
	if err != nil {
		t.Fatal(err)
	}
	if len(specification.Operations) != 1 || len(specification.Operations[0].Responses) != 2 {
		t.Fatalf("unexpected per-status responses: %#v", specification.Operations)
	}
}

func TestLoadSpecRejectsDefaultResponse(t *testing.T) {
	t.Parallel()
	path := writeTestSchema(t, `{
        "openapi":"3.0.3",
        "info":{"title":"x","version":"1"},
        "paths":{"/value":{"get":{"operationId":"getValue","responses":{
            "200":{"description":"value"},
            "default":{"description":"error"}
        }}}}
    }`)
	if _, err := loadSpec(path, "test"); err == nil || !strings.Contains(err.Error(), "explicit numeric status") {
		t.Fatalf("got %v, want default-response rejection", err)
	}
}

func TestValidateRequestFieldIdentifiersRejectsGeneratedCollisions(t *testing.T) {
	t.Parallel()
	cases := [][]parameterSpec{
		{{Name: "id", CName: "id", Location: "path", Required: true}, {Name: "id", CName: "id", Location: "query", Required: true}},
		{{Name: "id", CName: "id", Location: "query", PresenceName: "has_id"}, {Name: "has-id", CName: "has_id", Location: "header", Required: true}},
		{{Name: "body", CName: "body", Location: "query", Required: true}},
	}
	for index, parameters := range cases {
		if err := validateRequestFieldIdentifiers(parameters, index == 2, true); err == nil {
			t.Fatalf("collision case %d unexpectedly accepted", index)
		}
	}
}

func TestLoadSpecDisambiguatesModelPresenceFieldCollision(t *testing.T) {
	t.Parallel()
	path := writeTestSchema(t, `{
        "openapi":"3.0.3",
        "info":{"title":"x","version":"1"},
        "paths":{"/value":{"get":{"operationId":"getValue","responses":{
            "200":{"description":"value","content":{"application/json":{"schema":{
                "type":"object",
                "required":["has_id"],
                "properties":{"id":{"type":"boolean"},"has_id":{"type":"boolean"}}
            }}}}
        }}}}
    }`)
	specification, err := loadSpec(path, "test")
	if err != nil {
		t.Fatal(err)
	}
	if len(specification.Models) != 1 || len(specification.Models[0].Fields) != 2 || specification.Models[0].Fields[1].PresenceName != "has_id_2" {
		t.Fatalf("unexpected disambiguation: %#v", specification.Models)
	}
}

func TestLoadSpecSupportsProductionFeatureProfile(t *testing.T) {
	t.Parallel()
	path := writeTestSchema(t, `{
        "openapi":"3.0.3",
        "info":{"title":"x","version":"1"},
        "paths":{
            "/items":{"get":{"operationId":"items","security":[{"bearer":[]}],"parameters":[{"in":"query","name":"tag","schema":{"type":"array","items":{"type":"string"}}}],"responses":{"200":{"description":"ok","headers":{"X-Total-Count":{"schema":{"type":"string"}}},"content":{"application/json":{"schema":{"type":"array","items":{"$ref":"#/components/schemas/Value"}}}}}}}},
            "/upload":{"post":{"operationId":"upload","requestBody":{"required":true,"content":{"multipart/form-data":{"schema":{"type":"object","required":["file"],"properties":{"file":{"type":"string","format":"binary"}}}}}},"responses":{"204":{"description":"ok"}}}}
        },
        "components":{"securitySchemes":{"bearer":{"type":"http","scheme":"bearer"}},"schemas":{
            "Value":{"type":"object","properties":{"name":{"type":"string","nullable":true},"metadata":{"type":"object","additionalProperties":true},"choice":{"oneOf":[{"$ref":"#/components/schemas/A"},{"$ref":"#/components/schemas/B"}]}}},
            "A":{"type":"object","required":["a"],"properties":{"a":{"type":"string"}}},
            "B":{"type":"object","required":["b"],"properties":{"b":{"type":"boolean"}}}
        }}
    }`)
	specification, err := loadSpec(path, "test")
	if err != nil {
		t.Fatal(err)
	}
	if len(specification.Operations) != 2 || !specification.Operations[0].BearerSecurity || specification.Operations[0].Parameters[0].Type.Kind != "array" || len(specification.Operations[0].Responses[0].Headers) != 1 || specification.Operations[1].RequestMediaType != "multipart/form-data" {
		t.Fatalf("unexpected production feature lowering: %#v", specification.Operations)
	}
	files, err := emit(specification)
	if err != nil {
		t.Fatal(err)
	}
	header := string(files["h2_test_api.h"])
	source := string(files["h2_test_api.c"])
	for _, expected := range []string{
		"H2_TEST_OPERATION_COUNT 2u",
		"h2_test_value_choice_kind_t",
		"h2_test_json_t metadata",
		"bool is_name_null",
	} {
		if !strings.Contains(header, expected) {
			t.Errorf("generated header is missing %q", expected)
		}
	}
	for _, expected := range []string{
		"Bearer ",
		"response_header_cb = h2_test_items_response_header",
		"for (size_t query_index = 0u;",
		"h2-openapi-boundary",
		"h2_pal_json_object_entry",
	} {
		if !strings.Contains(source, expected) {
			t.Errorf("generated source is missing %q", expected)
		}
	}
}

func TestValidateOperationResponseHeadersRejectsCrossStatusConflicts(t *testing.T) {
	t.Parallel()
	responses := []responseSpec{
		{Status: 200, Headers: []responseHeaderSpec{{Name: "X-Value", CName: "x_value", Type: typeSpec{Kind: "string", MaxLength: 8, MaxLengthSet: true}}}},
		{Status: 400, Headers: []responseHeaderSpec{{Name: "x-value", CName: "x_value", Type: typeSpec{Kind: "string", MaxLength: 16, MaxLengthSet: true}}}},
	}
	if err := validateOperationResponseHeaders(responses); err == nil || !strings.Contains(err.Error(), "incompatible schemas") {
		t.Fatalf("got %v, want incompatible response-header schemas", err)
	}
	responses[1].Headers[0] = responseHeaderSpec{Name: "X_Value", CName: "x_value", Type: responses[0].Headers[0].Type}
	if err := validateOperationResponseHeaders(responses); err == nil || !strings.Contains(err.Error(), "collide") {
		t.Fatalf("got %v, want response-header identifier collision", err)
	}
}

func TestLoadSpecRejectsNullablePathParameter(t *testing.T) {
	t.Parallel()
	path := writeTestSchema(t, `{
        "openapi":"3.0.3",
        "info":{"title":"x","version":"1"},
        "paths":{"/values/{id}":{"get":{"operationId":"getValue","parameters":[
            {"in":"path","name":"id","required":true,"schema":{"type":"string","nullable":true}}
        ],"responses":{"204":{"description":"ok"}}}}}
    }`)
	if _, err := loadSpec(path, "test"); err == nil || !strings.Contains(err.Error(), "cannot be nullable") {
		t.Fatalf("got %v, want nullable path rejection", err)
	}
}

func TestTypeFromSchemaRejectsNullableArrayItems(t *testing.T) {
	t.Parallel()
	builder := &schemaBuilder{models: make(map[string]*modelSpec), visiting: make(map[*openapi3.Schema]bool), modelSource: make(map[string]*openapi3.Schema)}
	item := openapi3.NewStringSchema()
	item.Nullable = true
	array := openapi3.NewArraySchema().WithItems(item)
	if _, err := builder.typeFromSchema(array.NewRef(), "values"); err == nil || !strings.Contains(err.Error(), "nullable array items") {
		t.Fatalf("got %v, want nullable array-item rejection", err)
	}
}

func TestLoadSpecRejectsBinaryJSONBody(t *testing.T) {
	t.Parallel()
	path := writeTestSchema(t, `{
        "openapi":"3.0.3",
        "info":{"title":"x","version":"1"},
        "paths":{"/value":{"post":{"operationId":"setValue","requestBody":{"content":{"application/json":{"schema":{"type":"string","format":"binary"}}}},"responses":{"204":{"description":"ok"}}}}}
    }`)
	if _, err := loadSpec(path, "test"); err == nil || !strings.Contains(err.Error(), "only in multipart") {
		t.Fatalf("got %v, want binary JSON rejection", err)
	}
}

func TestEscapeMultipartQuoted(t *testing.T) {
	t.Parallel()
	if got := escapeMultipartQuoted(`a\"b`); got != `a\\\"b` {
		t.Fatalf("got %q", got)
	}
}

func writeTestSchema(t *testing.T, data string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "schema.json")
	if err := os.WriteFile(path, []byte(data), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}
