package main

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strconv"
	"strings"

	"github.com/getkin/kin-openapi/openapi3"
)

const maxExactJSONInteger = 9007199254740991
const maxPortableStringLength = uint64(4294967294)
const maxPortableArrayItems = uint64(4294967295)

type apiSpec struct {
	Package    string      `json:"package"`
	Models     []modelSpec `json:"models"`
	Operations []operation `json:"operations"`
}

type modelSpec struct {
	Name     string        `json:"name"`
	Fields   []fieldSpec   `json:"fields,omitempty"`
	Variants []variantSpec `json:"variants,omitempty"`
	Alias    *typeSpec     `json:"alias,omitempty"`
}

type variantSpec struct {
	Name  string   `json:"name"`
	CName string   `json:"c_name"`
	Type  typeSpec `json:"type"`
}

type fieldSpec struct {
	JSONName     string   `json:"json_name"`
	CName        string   `json:"c_name"`
	Required     bool     `json:"required"`
	ReadOnly     bool     `json:"read_only,omitempty"`
	WriteOnly    bool     `json:"write_only,omitempty"`
	PresenceName string   `json:"presence_name,omitempty"`
	NullName     string   `json:"null_name,omitempty"`
	Type         typeSpec `json:"type"`
}

type typeSpec struct {
	Kind          string    `json:"kind"`
	Model         string    `json:"model,omitempty"`
	Nullable      bool      `json:"nullable,omitempty"`
	MinLength     uint64    `json:"min_length,omitempty"`
	MaxLength     uint64    `json:"max_length,omitempty"`
	MaxLengthSet  bool      `json:"max_length_set,omitempty"`
	MinItems      uint64    `json:"min_items,omitempty"`
	MaxItems      uint64    `json:"max_items,omitempty"`
	MaxItemsSet   bool      `json:"max_items_set,omitempty"`
	Item          *typeSpec `json:"item,omitempty"`
	Enum          []string  `json:"enum,omitempty"`
	Minimum       *int64    `json:"minimum,omitempty"`
	Maximum       *int64    `json:"maximum,omitempty"`
	NumberMinimum *float64  `json:"number_minimum,omitempty"`
	NumberMaximum *float64  `json:"number_maximum,omitempty"`
}

type operation struct {
	Name             string          `json:"name"`
	Method           string          `json:"method"`
	Path             string          `json:"path"`
	Parameters       []parameterSpec `json:"parameters"`
	RequestModel     string          `json:"request_model,omitempty"`
	RequestRequired  bool            `json:"request_required,omitempty"`
	RequestMediaType string          `json:"request_media_type,omitempty"`
	BearerSecurity   bool            `json:"bearer_security,omitempty"`
	Responses        []responseSpec  `json:"responses"`
}

type responseSpec struct {
	Status  int                  `json:"status"`
	Model   string               `json:"model,omitempty"`
	Success bool                 `json:"success"`
	Headers []responseHeaderSpec `json:"headers,omitempty"`
}

type responseHeaderSpec struct {
	Name  string   `json:"name"`
	CName string   `json:"c_name"`
	Type  typeSpec `json:"type"`
}

type parameterSpec struct {
	Name         string   `json:"name"`
	CName        string   `json:"c_name"`
	Location     string   `json:"location"`
	Required     bool     `json:"required"`
	AllowEmpty   bool     `json:"allow_empty,omitempty"`
	Explode      bool     `json:"explode,omitempty"`
	PresenceName string   `json:"presence_name,omitempty"`
	NullName     string   `json:"null_name,omitempty"`
	Type         typeSpec `json:"type"`
}

type schemaBuilder struct {
	document    *openapi3.T
	models      map[string]*modelSpec
	visiting    map[*openapi3.Schema]bool
	modelSource map[string]*openapi3.Schema
}

func loadSpec(inputPath string, packageName string) (*apiSpec, error) {
	if err := validatePackageName(packageName); err != nil {
		return nil, err
	}
	absolutePath, err := filepath.Abs(inputPath)
	if err != nil {
		return nil, fmt.Errorf("resolve input path: %w", err)
	}
	if filepath.Ext(absolutePath) != ".json" {
		return nil, fmt.Errorf("input must be an OpenAPI JSON document")
	}
	if err := validateReferenceTree(absolutePath); err != nil {
		return nil, err
	}

	loader := openapi3.NewLoader()
	loader.IsExternalRefsAllowed = true
	document, err := loader.LoadFromFile(absolutePath)
	if err != nil {
		return nil, fmt.Errorf("load OpenAPI document: %w", err)
	}
	if !strings.HasPrefix(document.OpenAPI, "3.0.") {
		return nil, fmt.Errorf("/openapi: version %q is unsupported; expected 3.0.x", document.OpenAPI)
	}
	disambiguateOperationIDs(document)
	validationContext := openapi3.WithValidationOptions(
		context.Background(),
		openapi3.DisableExamplesValidation(),
	)
	if err := document.Validate(validationContext); err != nil {
		return nil, fmt.Errorf("validate OpenAPI document: %w", err)
	}

	builder := &schemaBuilder{
		document:    document,
		models:      make(map[string]*modelSpec),
		visiting:    make(map[*openapi3.Schema]bool),
		modelSource: make(map[string]*openapi3.Schema),
	}
	operations, err := builder.collectOperations()
	if err != nil {
		return nil, err
	}
	if err := validatePublicIdentifiers(builder.models, operations); err != nil {
		return nil, err
	}

	modelNames := make([]string, 0, len(builder.models))
	for name := range builder.models {
		modelNames = append(modelNames, name)
	}
	sort.Strings(modelNames)
	models := make([]modelSpec, 0, len(modelNames))
	for _, name := range modelNames {
		models = append(models, *builder.models[name])
	}
	sort.Slice(operations, func(left, right int) bool {
		return operations[left].Name < operations[right].Name
	})
	return &apiSpec{Package: packageName, Models: models, Operations: operations}, nil
}

func disambiguateOperationIDs(document *openapi3.T) {
	type occurrence struct {
		method    string
		path      string
		operation *openapi3.Operation
	}
	groups := make(map[string][]occurrence)
	if document.Paths == nil {
		return
	}
	for _, path := range document.Paths.InMatchingOrder() {
		item := document.Paths.Value(path)
		if item == nil {
			continue
		}
		for _, method := range []string{"CONNECT", "DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST", "PUT", "TRACE"} {
			operation := item.GetOperation(method)
			if operation != nil && operation.OperationID != "" {
				groups[operation.OperationID] = append(groups[operation.OperationID], occurrence{method: method, path: path, operation: operation})
			}
		}
	}
	for operationID, occurrences := range groups {
		if len(occurrences) < 2 {
			continue
		}
		for _, item := range occurrences {
			item.operation.OperationID = operationID + "_" + strings.ToLower(item.method) + "_" + cIdentifier(item.path)
		}
	}
}

func validatePublicIdentifiers(models map[string]*modelSpec, operations []operation) error {
	owners := map[string]string{
		"result":          "generated result type",
		"string":          "generated string type",
		"header_callback": "generated header callback type",
		"client":          "generated client type",
		"client_config":   "generated client config type",
		"client_init":     "generated client initializer",
	}
	claim := func(identifier string, owner string) error {
		if previous, exists := owners[identifier]; exists {
			return fmt.Errorf("C identifier %q collides between %s and %s", identifier, previous, owner)
		}
		owners[identifier] = owner
		return nil
	}
	modelNames := make([]string, 0, len(models))
	for name := range models {
		modelNames = append(modelNames, name)
	}
	sort.Strings(modelNames)
	for _, name := range modelNames {
		if err := claim(name, fmt.Sprintf("model %q", name)); err != nil {
			return err
		}
	}
	for _, item := range operations {
		claims := []struct {
			identifier string
			owner      string
		}{
			{item.Name, fmt.Sprintf("operation %q", item.Name)},
			{item.Name + "_request", fmt.Sprintf("request type for %q", item.Name)},
			{item.Name + "_response", fmt.Sprintf("response type for %q", item.Name)},
			{item.Name + "_response_deinit", fmt.Sprintf("response deinitializer for %q", item.Name)},
		}
		for _, candidate := range claims {
			if err := claim(candidate.identifier, candidate.owner); err != nil {
				return err
			}
		}
	}
	return nil
}

func (builder *schemaBuilder) collectOperations() ([]operation, error) {
	if builder.document.Paths == nil {
		return nil, fmt.Errorf("document has no paths")
	}
	seenNames := make(map[string]string)
	var operations []operation
	for _, path := range builder.document.Paths.InMatchingOrder() {
		if strings.ContainsRune(path, 0) {
			return nil, fmt.Errorf("path contains U+0000")
		}
		item := builder.document.Paths.Value(path)
		if item == nil {
			continue
		}
		methods := []string{"DELETE", "GET", "PATCH", "POST", "PUT"}
		for _, method := range methods {
			openapiOperation := item.GetOperation(method)
			if openapiOperation == nil {
				continue
			}
			if openapiOperation.OperationID == "" {
				return nil, fmt.Errorf("%s %s is missing operationId", method, path)
			}
			if strings.ContainsRune(openapiOperation.OperationID, 0) {
				return nil, fmt.Errorf("%s %s operationId contains U+0000", method, path)
			}
			name := cIdentifier(openapiOperation.OperationID)
			if previous, exists := seenNames[name]; exists {
				return nil, fmt.Errorf("operationId collision: %q and %q both map to %q", previous, openapiOperation.OperationID, name)
			}
			seenNames[name] = openapiOperation.OperationID
			collected, err := builder.collectOperation(item, path, method, name, openapiOperation)
			if err != nil {
				return nil, fmt.Errorf("operation %q: %w", openapiOperation.OperationID, err)
			}
			operations = append(operations, collected)
		}
		for method, unsupported := range map[string]*openapi3.Operation{
			"CONNECT": item.Connect, "HEAD": item.Head, "OPTIONS": item.Options, "TRACE": item.Trace,
		} {
			if unsupported != nil {
				return nil, fmt.Errorf("%s %s uses unsupported method", method, path)
			}
		}
	}
	if len(operations) == 0 {
		return nil, fmt.Errorf("document has no supported operations")
	}
	return operations, nil
}

func (builder *schemaBuilder) collectOperation(item *openapi3.PathItem, path string, method string, name string, source *openapi3.Operation) (operation, error) {
	if len(source.Callbacks) != 0 {
		return operation{}, fmt.Errorf("callbacks are unsupported")
	}
	parameters, err := builder.collectParameters(append(item.Parameters, source.Parameters...))
	if err != nil {
		return operation{}, err
	}
	requestModel, requestRequired, requestMediaType, err := builder.collectBodyModel(name+"_request_body", source.RequestBody)
	if err != nil {
		return operation{}, fmt.Errorf("request body: %w", err)
	}
	if err := validateRequestFieldIdentifiers(parameters, requestModel != "", requestRequired); err != nil {
		return operation{}, err
	}
	responses, err := builder.collectResponses(name, source.Responses)
	if err != nil {
		return operation{}, err
	}
	if err := validateOperationResponseHeaders(responses); err != nil {
		return operation{}, err
	}
	if requestMediaType == "multipart/form-data" {
		if err := builder.validateMultipartModel(requestModel); err != nil {
			return operation{}, fmt.Errorf("request body: %w", err)
		}
	}
	bearerSecurity, err := builder.operationUsesBearerSecurity(source)
	if err != nil {
		return operation{}, err
	}
	return operation{
		Name:             name,
		Method:           method,
		Path:             path,
		Parameters:       parameters,
		RequestModel:     requestModel,
		RequestRequired:  requestRequired,
		RequestMediaType: requestMediaType,
		BearerSecurity:   bearerSecurity,
		Responses:        responses,
	}, nil
}

func validateOperationResponseHeaders(responses []responseSpec) error {
	byName := make(map[string]responseHeaderSpec)
	byCName := make(map[string]responseHeaderSpec)
	for _, response := range responses {
		for _, header := range response.Headers {
			nameKey := strings.ToLower(header.Name)
			if previous, exists := byName[nameKey]; exists && !reflect.DeepEqual(previous.Type, header.Type) {
				return fmt.Errorf("response header %q has incompatible schemas across statuses", header.Name)
			}
			if previous, exists := byCName[header.CName]; exists && !strings.EqualFold(previous.Name, header.Name) {
				return fmt.Errorf("response headers %q and %q collide as %q across statuses", previous.Name, header.Name, header.CName)
			}
			byName[nameKey] = header
			byCName[header.CName] = header
		}
	}
	return nil
}

func (builder *schemaBuilder) validateMultipartModel(name string) error {
	model := builder.models[name]
	if model == nil || model.Alias != nil || len(model.Variants) != 0 {
		return fmt.Errorf("multipart/form-data requires an object model")
	}
	for _, field := range model.Fields {
		if strings.ContainsAny(field.JSONName, "\r\n\x00") {
			return fmt.Errorf("multipart field %q contains a forbidden control character", field.JSONName)
		}
		if field.ReadOnly {
			continue
		}
		if field.Type.Kind != "string" && field.Type.Kind != "bytes" {
			return fmt.Errorf("multipart field %q must use string or binary", field.JSONName)
		}
	}
	return nil
}

func (builder *schemaBuilder) operationUsesBearerSecurity(source *openapi3.Operation) (bool, error) {
	requirements := builder.document.Security
	if source.Security != nil {
		requirements = *source.Security
	}
	if len(requirements) == 0 {
		return false, nil
	}
	if len(requirements) != 1 || len(requirements[0]) != 1 {
		return false, fmt.Errorf("only one bearer security requirement is supported")
	}
	for name, scopes := range requirements[0] {
		if name != "bearer" || len(scopes) != 0 {
			return false, fmt.Errorf("security requirement %q is unsupported", name)
		}
		reference := builder.document.Components.SecuritySchemes[name]
		if reference == nil || reference.Value == nil || reference.Value.Type != "http" || !strings.EqualFold(reference.Value.Scheme, "bearer") {
			return false, fmt.Errorf("security scheme %q must be HTTP bearer", name)
		}
		return true, nil
	}
	return false, nil
}

func validateRequestFieldIdentifiers(parameters []parameterSpec, hasBody bool, bodyRequired bool) error {
	owners := make(map[string]string)
	claim := func(identifier string, owner string) error {
		if previous, exists := owners[identifier]; exists {
			return fmt.Errorf("request C field %q collides between %s and %s", identifier, previous, owner)
		}
		owners[identifier] = owner
		return nil
	}
	for _, parameter := range parameters {
		owner := fmt.Sprintf("%s parameter %q", parameter.Location, parameter.Name)
		if err := claim(parameter.CName, owner); err != nil {
			return err
		}
		if !parameter.Required {
			if err := claim(parameter.PresenceName, "presence field for "+owner); err != nil {
				return err
			}
		}
		if parameter.Type.Nullable {
			if err := claim(parameter.NullName, "null field for "+owner); err != nil {
				return err
			}
		}
	}
	if hasBody {
		if err := claim("body", "request body"); err != nil {
			return err
		}
		if !bodyRequired {
			if err := claim("has_body", "request body presence field"); err != nil {
				return err
			}
		}
	}
	return nil
}

func (builder *schemaBuilder) collectParameters(refs openapi3.Parameters) ([]parameterSpec, error) {
	seen := make(map[string]bool)
	parameters := make([]parameterSpec, 0, len(refs))
	for _, reference := range refs {
		if reference == nil || reference.Value == nil {
			return nil, fmt.Errorf("unresolved parameter reference")
		}
		parameter := reference.Value
		if strings.ContainsRune(parameter.Name, 0) {
			return nil, fmt.Errorf("parameter name contains U+0000")
		}
		if parameter.In == "cookie" {
			return nil, fmt.Errorf("cookie parameter %q is unsupported", parameter.Name)
		}
		if parameter.In != "path" && parameter.In != "query" && parameter.In != "header" {
			return nil, fmt.Errorf("parameter %q has unsupported location %q", parameter.Name, parameter.In)
		}
		defaultStyle := map[string]string{"path": "simple", "query": "form", "header": "simple"}[parameter.In]
		if parameter.Style != "" && parameter.Style != defaultStyle {
			return nil, fmt.Errorf("parameter %q uses unsupported style %q", parameter.Name, parameter.Style)
		}
		if parameter.AllowReserved {
			return nil, fmt.Errorf("parameter %q uses unsupported allowReserved", parameter.Name)
		}
		if parameter.AllowEmptyValue && parameter.In != "query" {
			return nil, fmt.Errorf("parameter %q uses allowEmptyValue outside query", parameter.Name)
		}
		if parameter.In == "header" && !validHTTPHeaderName(parameter.Name) {
			return nil, fmt.Errorf("header parameter %q is not a valid HTTP token", parameter.Name)
		}
		key := parameter.In + ":" + parameter.Name
		if seen[key] {
			return nil, fmt.Errorf("duplicate parameter %s", key)
		}
		seen[key] = true
		if parameter.Schema == nil {
			return nil, fmt.Errorf("parameter %q must use schema, not content", parameter.Name)
		}
		typeValue, err := builder.typeFromSchema(parameter.Schema, "")
		if err != nil {
			return nil, fmt.Errorf("parameter %q: %w", parameter.Name, err)
		}
		if typeValue.Kind == "model" || typeValue.Kind == "json" || typeValue.Kind == "bytes" {
			return nil, fmt.Errorf("parameter %q must be a scalar", parameter.Name)
		}
		if parameter.In == "path" && typeValue.Nullable {
			return nil, fmt.Errorf("path parameter %q cannot be nullable", parameter.Name)
		}
		explode := parameter.In == "query"
		if parameter.Explode != nil {
			explode = *parameter.Explode
		}
		if typeValue.Kind == "array" {
			if parameter.In != "query" || !explode || typeValue.Item == nil || typeValue.Item.Kind == "array" || typeValue.Item.Kind == "model" || typeValue.Item.Kind == "json" || typeValue.Item.Kind == "bytes" {
				return nil, fmt.Errorf("parameter %q arrays require query form explode=true with scalar items", parameter.Name)
			}
		}
		if parameter.AllowEmptyValue && typeValue.Kind != "string" {
			return nil, fmt.Errorf("parameter %q uses allowEmptyValue with a non-string schema", parameter.Name)
		}
		if parameter.In == "path" && !parameter.Required {
			return nil, fmt.Errorf("path parameter %q must be required", parameter.Name)
		}
		presenceName := ""
		if !parameter.Required {
			presenceName = "has_" + cIdentifier(parameter.Name)
		}
		nullName := ""
		if typeValue.Nullable {
			nullName = "is_" + cIdentifier(parameter.Name) + "_null"
		}
		parameters = append(parameters, parameterSpec{
			Name: parameter.Name, CName: cIdentifier(parameter.Name), Location: parameter.In,
			Required: parameter.Required, AllowEmpty: parameter.AllowEmptyValue, Explode: explode,
			PresenceName: presenceName, NullName: nullName, Type: typeValue,
		})
	}
	sort.Slice(parameters, func(left, right int) bool {
		if parameters[left].Location == parameters[right].Location {
			return parameters[left].Name < parameters[right].Name
		}
		return parameters[left].Location < parameters[right].Location
	})
	return parameters, nil
}

func validHTTPHeaderName(value string) bool {
	if value == "" {
		return false
	}
	for _, current := range []byte(value) {
		if (current >= 'a' && current <= 'z') || (current >= 'A' && current <= 'Z') || (current >= '0' && current <= '9') {
			continue
		}
		switch current {
		case '!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~':
			continue
		default:
			return false
		}
	}
	return true
}

func (builder *schemaBuilder) collectBodyModel(name string, reference *openapi3.RequestBodyRef) (string, bool, string, error) {
	if reference == nil {
		return "", false, "", nil
	}
	if reference.Value == nil {
		return "", false, "", fmt.Errorf("unresolved reference")
	}
	contentType := "application/json"
	mediaType := reference.Value.Content.Get(contentType)
	if mediaType == nil {
		contentType = "multipart/form-data"
		mediaType = reference.Value.Content.Get(contentType)
	}
	if mediaType == nil || mediaType.Schema == nil {
		return "", false, "", fmt.Errorf("application/json or multipart/form-data schema is required")
	}
	model, err := builder.ensureModel(name, mediaType.Schema)
	if err == nil && contentType == "application/json" {
		err = builder.validateJSONModel(model, make(map[string]bool))
	}
	return model, reference.Value.Required, contentType, err
}

func (builder *schemaBuilder) collectResponses(operationName string, responses *openapi3.Responses) ([]responseSpec, error) {
	if responses == nil {
		return nil, fmt.Errorf("responses are required")
	}
	keys := make([]string, 0, len(responses.Map()))
	for key := range responses.Map() {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	var collected []responseSpec
	for _, key := range keys {
		response := responses.Value(key)
		if response == nil || response.Value == nil {
			return nil, fmt.Errorf("response %s is unresolved", key)
		}
		if len(response.Value.Links) != 0 {
			return nil, fmt.Errorf("response %s uses unsupported links", key)
		}
		status, err := strconv.Atoi(key)
		if err != nil {
			return nil, fmt.Errorf("response status %q is unsupported; use an explicit numeric status", key)
		}
		item := responseSpec{Status: status, Success: status >= 200 && status < 300}
		mediaType := response.Value.Content.Get("application/json")
		if mediaType == nil && len(response.Value.Content) != 0 {
			return nil, fmt.Errorf("response %s must use application/json content", key)
		}
		if mediaType != nil {
			if mediaType.Schema == nil {
				return nil, fmt.Errorf("response %s has no JSON schema", key)
			}
			model, err := builder.ensureModel(fmt.Sprintf("%s_response_%d_body", operationName, status), mediaType.Schema)
			if err != nil {
				return nil, fmt.Errorf("response %s: %w", key, err)
			}
			if err := builder.validateJSONModel(model, make(map[string]bool)); err != nil {
				return nil, fmt.Errorf("response %s: %w", key, err)
			}
			item.Model = model
		}
		headerNames := make([]string, 0, len(response.Value.Headers))
		for name := range response.Value.Headers {
			headerNames = append(headerNames, name)
		}
		sort.Strings(headerNames)
		seenHeaderFields := make(map[string]string)
		for _, name := range headerNames {
			header := response.Value.Headers[name]
			if header == nil || header.Value == nil || header.Value.Schema == nil {
				return nil, fmt.Errorf("response %s header %q is unresolved or has no schema", key, name)
			}
			if !validHTTPHeaderName(name) {
				return nil, fmt.Errorf("response %s header %q is not a valid HTTP token", key, name)
			}
			headerType, err := builder.typeFromSchema(header.Value.Schema, "")
			if err != nil {
				return nil, fmt.Errorf("response %s header %q: %w", key, name, err)
			}
			if headerType.Kind != "string" {
				return nil, fmt.Errorf("response %s header %q must use a string schema", key, name)
			}
			cName := cIdentifier(name)
			if previous, exists := seenHeaderFields[cName]; exists {
				return nil, fmt.Errorf("response %s headers %q and %q collide as %q", key, previous, name, cName)
			}
			seenHeaderFields[cName] = name
			item.Headers = append(item.Headers, responseHeaderSpec{Name: name, CName: cName, Type: headerType})
		}
		collected = append(collected, item)
	}
	return collected, nil
}

func (builder *schemaBuilder) validateJSONModel(name string, visiting map[string]bool) error {
	if visiting[name] {
		return nil
	}
	model := builder.models[name]
	if model == nil {
		return fmt.Errorf("JSON model %q is unresolved", name)
	}
	visiting[name] = true
	defer delete(visiting, name)
	var validateType func(typeSpec) error
	validateType = func(value typeSpec) error {
		switch value.Kind {
		case "bytes":
			return fmt.Errorf("binary string is supported only in multipart/form-data")
		case "model":
			return builder.validateJSONModel(value.Model, visiting)
		case "array":
			if value.Item == nil {
				return fmt.Errorf("array item is missing")
			}
			return validateType(*value.Item)
		default:
			return nil
		}
	}
	if model.Alias != nil {
		return validateType(*model.Alias)
	}
	for _, field := range model.Fields {
		if err := validateType(field.Type); err != nil {
			return fmt.Errorf("model %q field %q: %w", name, field.JSONName, err)
		}
	}
	for _, variant := range model.Variants {
		if err := validateType(variant.Type); err != nil {
			return fmt.Errorf("model %q oneOf branch %q: %w", name, variant.Name, err)
		}
	}
	return nil
}

func (builder *schemaBuilder) ensureModel(name string, reference *openapi3.SchemaRef) (string, error) {
	if reference == nil || reference.Value == nil {
		return "", fmt.Errorf("unresolved schema reference")
	}
	schemaLocation := reference.Ref
	if schemaLocation == "" {
		schemaLocation = "<inline:" + name + ">"
	}
	if referencedName := componentName(reference.Ref); referencedName != "" {
		name = cIdentifier(referencedName)
	}
	if builder.visiting[reference.Value] {
		return "", fmt.Errorf("schema %s: cyclic schema model %q is unsupported", schemaLocation, name)
	}
	if existing := builder.models[name]; existing != nil {
		if builder.modelSource[name] == reference.Value {
			return name, nil
		}
		existingJSON, existingErr := json.Marshal(builder.modelSource[name])
		candidateJSON, candidateErr := json.Marshal(reference.Value)
		if existingErr != nil || candidateErr != nil || string(existingJSON) != string(candidateJSON) {
			return "", fmt.Errorf("schema %s: name collision for C model %q", schemaLocation, name)
		}
		return name, nil
	}
	builder.visiting[reference.Value] = true
	defer delete(builder.visiting, reference.Value)
	schema := reference.Value
	model := &modelSpec{Name: name}
	builder.models[name] = model
	builder.modelSource[name] = schema

	if len(schema.OneOf) != 0 {
		if len(schema.AnyOf) != 0 || schema.Not != nil || len(schema.AllOf) != 0 {
			return "", fmt.Errorf("schema %s: mixed composition is unsupported", schemaLocation)
		}
		seenVariants := make(map[string]bool)
		for index, variantReference := range schema.OneOf {
			variantType, err := builder.typeFromSchema(variantReference, fmt.Sprintf("%s_variant_%d", name, index+1))
			if err != nil {
				return "", fmt.Errorf("schema %s oneOf branch %d: %w", schemaLocation, index+1, err)
			}
			if variantType.Kind != "model" || variantType.Nullable {
				return "", fmt.Errorf("schema %s oneOf branch %d must be a non-null object model", schemaLocation, index+1)
			}
			variantName := variantType.Model
			if seenVariants[variantName] {
				return "", fmt.Errorf("schema %s repeats oneOf model %q", schemaLocation, variantName)
			}
			seenVariants[variantName] = true
			model.Variants = append(model.Variants, variantSpec{Name: variantName, CName: cIdentifier(variantName), Type: variantType})
		}
		if len(model.Variants) < 2 {
			return "", fmt.Errorf("schema %s oneOf requires at least two branches", schemaLocation)
		}
		return name, nil
	}

	if (schema.Type != nil && schema.Type.Is("object")) && len(schema.AllOf) == 0 && len(schema.Properties) == 0 && schema.AdditionalProperties.Has != nil && *schema.AdditionalProperties.Has {
		alias := typeSpec{Kind: "json", Nullable: schema.Nullable}
		model.Alias = &alias
		return name, nil
	}
	metadataOnly := len(schema.AllOf) == 0 && schema.Type == nil && len(schema.Properties) == 0 && len(schema.Required) == 0
	if len(schema.AllOf) == 0 && !metadataOnly && (schema.Type == nil || !schema.Type.Is("object")) {
		alias, err := builder.typeFromSchema(reference, name+"_value")
		if err != nil {
			return "", fmt.Errorf("schema %s: %w", schemaLocation, err)
		}
		model.Alias = &alias
		return name, nil
	}

	properties, required, err := builder.flattenObject(schema, make(map[*openapi3.Schema]bool))
	if err != nil {
		return "", fmt.Errorf("schema %s: %w", schemaLocation, err)
	}
	propertyNames := make([]string, 0, len(properties))
	for propertyName := range properties {
		propertyNames = append(propertyNames, propertyName)
	}
	sort.Strings(propertyNames)
	seenCNames := make(map[string]string)
	claimField := func(identifier string, owner string) error {
		if previous, exists := seenCNames[identifier]; exists {
			return fmt.Errorf("schema %s C field %q collides between %s and %s", schemaLocation, identifier, previous, owner)
		}
		seenCNames[identifier] = owner
		return nil
	}
	for _, propertyName := range propertyNames {
		if strings.ContainsRune(propertyName, 0) {
			return "", fmt.Errorf("schema %s property %q contains U+0000", schemaLocation, propertyName)
		}
		if err := claimField(cIdentifier(propertyName), fmt.Sprintf("property %q", propertyName)); err != nil {
			return "", err
		}
	}
	generatedFieldName := func(base string, owner string) string {
		candidate := base
		for suffix := 2; ; suffix++ {
			if _, exists := seenCNames[candidate]; !exists {
				seenCNames[candidate] = owner
				return candidate
			}
			candidate = base + "_" + strconv.Itoa(suffix)
		}
	}
	for _, propertyName := range propertyNames {
		cName := cIdentifier(propertyName)
		presenceName := ""
		if !required[propertyName] {
			presenceName = generatedFieldName("has_"+cName, fmt.Sprintf("presence field for property %q", propertyName))
		}
		property := properties[propertyName]
		typeValue, err := builder.typeFromSchema(property, name+"_"+cName)
		if err != nil {
			return "", fmt.Errorf("schema %s property %q: %w", schemaLocation, propertyName, err)
		}
		nullName := ""
		if typeValue.Nullable {
			nullName = generatedFieldName("is_"+cName+"_null", fmt.Sprintf("null field for property %q", propertyName))
		}
		model.Fields = append(model.Fields, fieldSpec{
			JSONName:     propertyName,
			CName:        cName,
			Required:     required[propertyName],
			ReadOnly:     property.Value.ReadOnly,
			WriteOnly:    property.Value.WriteOnly,
			PresenceName: presenceName,
			NullName:     nullName,
			Type:         typeValue,
		})
	}
	return name, nil
}

func (builder *schemaBuilder) flattenObject(schema *openapi3.Schema, visiting map[*openapi3.Schema]bool) (openapi3.Schemas, map[string]bool, error) {
	if schema == nil {
		return nil, nil, fmt.Errorf("schema is missing")
	}
	if visiting[schema] {
		return nil, nil, fmt.Errorf("cyclic allOf is unsupported")
	}
	visiting[schema] = true
	defer delete(visiting, schema)
	if len(schema.OneOf) != 0 || len(schema.AnyOf) != 0 || schema.Not != nil {
		return nil, nil, fmt.Errorf("oneOf, anyOf, and not are unsupported")
	}
	if len(schema.Enum) != 0 || schema.MinProps != 0 || schema.MaxProps != nil {
		return nil, nil, fmt.Errorf("enum and property-count constraints are unsupported for objects")
	}
	if schema.AdditionalProperties.Schema != nil || (schema.AdditionalProperties.Has != nil && !*schema.AdditionalProperties.Has) {
		return nil, nil, fmt.Errorf("schema-valued or false additionalProperties is unsupported")
	}
	metadataOnly := len(schema.AllOf) == 0 && schema.Type == nil && len(schema.Properties) == 0 && len(schema.Required) == 0
	if len(schema.AllOf) == 0 && !metadataOnly && (schema.Type == nil || !schema.Type.Is("object")) {
		return nil, nil, fmt.Errorf("expected object schema")
	}
	properties := make(openapi3.Schemas)
	required := make(map[string]bool)
	merge := func(nextProperties openapi3.Schemas, nextRequired map[string]bool) error {
		for name, property := range nextProperties {
			if _, exists := properties[name]; exists {
				return fmt.Errorf("allOf property %q is declared more than once", name)
			}
			properties[name] = property
		}
		for name := range nextRequired {
			required[name] = true
		}
		return nil
	}
	baseRequired := make(map[string]bool)
	for _, name := range schema.Required {
		baseRequired[name] = true
	}
	if err := merge(schema.Properties, baseRequired); err != nil {
		return nil, nil, err
	}
	for _, part := range schema.AllOf {
		if part == nil || part.Value == nil {
			return nil, nil, fmt.Errorf("unresolved allOf reference")
		}
		partProperties, partRequired, err := builder.flattenObject(part.Value, visiting)
		if err != nil {
			return nil, nil, err
		}
		if err := merge(partProperties, partRequired); err != nil {
			return nil, nil, err
		}
	}
	return properties, required, nil
}

func (builder *schemaBuilder) typeFromSchema(reference *openapi3.SchemaRef, inlineName string) (typeSpec, error) {
	if reference == nil || reference.Value == nil {
		return typeSpec{}, fmt.Errorf("unresolved schema reference")
	}
	schema := reference.Value
	nullable := schema.Nullable
	if len(schema.AnyOf) != 0 || schema.Not != nil {
		return typeSpec{}, fmt.Errorf("anyOf and not are unsupported")
	}
	if len(schema.OneOf) != 0 {
		if inlineName == "" {
			return typeSpec{}, fmt.Errorf("oneOf schema requires a named model")
		}
		model, err := builder.ensureModel(inlineName, reference)
		return typeSpec{Kind: "model", Model: model, Nullable: nullable}, err
	}
	if len(schema.AllOf) != 0 {
		var semanticParts []*openapi3.SchemaRef
		for _, part := range schema.AllOf {
			if part == nil || part.Value == nil {
				return typeSpec{}, fmt.Errorf("unresolved allOf reference")
			}
			metadataOnly := part.Value.Type == nil && len(part.Value.AllOf) == 0 && len(part.Value.OneOf) == 0 && len(part.Value.AnyOf) == 0 && len(part.Value.Properties) == 0 && len(part.Value.Required) == 0
			if !metadataOnly {
				semanticParts = append(semanticParts, part)
			}
		}
		if len(semanticParts) == 1 {
			partSchema := semanticParts[0].Value
			if len(partSchema.AllOf) == 0 && (partSchema.Type == nil || !partSchema.Type.Is("object")) {
				value, err := builder.typeFromSchema(semanticParts[0], inlineName)
				value.Nullable = value.Nullable || nullable
				return value, err
			}
		}
	}
	if len(schema.AllOf) != 0 || (schema.Type != nil && schema.Type.Is("object")) {
		if len(schema.AllOf) == 0 && len(schema.Properties) == 0 && schema.AdditionalProperties.Has != nil && *schema.AdditionalProperties.Has {
			return typeSpec{Kind: "json", Nullable: nullable}, nil
		}
		if inlineName == "" {
			return typeSpec{}, fmt.Errorf("object schema requires a named model")
		}
		model, err := builder.ensureModel(inlineName, reference)
		return typeSpec{Kind: "model", Model: model, Nullable: nullable}, err
	}
	if schema.Type == nil || len(schema.Type.Slice()) != 1 {
		return typeSpec{}, fmt.Errorf("schema must have exactly one type")
	}
	switch schema.Type.Slice()[0] {
	case "string":
		if schema.Format == "binary" {
			return typeSpec{Kind: "bytes", Nullable: nullable}, nil
		}
		if schema.Format != "" && schema.Format != "uuid" && schema.Format != "date-time" && schema.Format != "uri" {
			return typeSpec{}, fmt.Errorf("string format %q is unsupported", schema.Format)
		}
		if schema.MaxLength != nil && *schema.MaxLength > maxPortableStringLength {
			return typeSpec{}, fmt.Errorf("string maxLength exceeds portable C limit %d", maxPortableStringLength)
		}
		if schema.MaxLength != nil && schema.MinLength > *schema.MaxLength {
			return typeSpec{}, fmt.Errorf("string minLength exceeds maxLength")
		}
		if schema.Pattern != "" {
			return typeSpec{}, fmt.Errorf("string pattern is unsupported")
		}
		enumeration := make([]string, 0, len(schema.Enum))
		for _, value := range schema.Enum {
			text, ok := value.(string)
			if !ok {
				return typeSpec{}, fmt.Errorf("string enum contains a non-string value")
			}
			if strings.ContainsRune(text, 0) {
				return typeSpec{}, fmt.Errorf("string enum contains U+0000")
			}
			enumeration = append(enumeration, text)
		}
		value := typeSpec{Kind: "string", Nullable: nullable, MinLength: schema.MinLength, Enum: enumeration}
		if schema.MaxLength != nil {
			value.MaxLength = *schema.MaxLength
			value.MaxLengthSet = true
		}
		return value, nil
	case "boolean":
		if len(schema.Enum) != 0 {
			return typeSpec{}, fmt.Errorf("boolean enum is unsupported")
		}
		return typeSpec{Kind: "bool", Nullable: nullable}, nil
	case "integer":
		if schema.Format == "int32" {
			if schema.Min != nil || schema.Max != nil || schema.MultipleOf != nil || schema.ExclusiveMin || schema.ExclusiveMax || len(schema.Enum) != 0 {
				return typeSpec{}, fmt.Errorf("int32 numeric constraints and enum are unsupported")
			}
			return typeSpec{Kind: "int32", Nullable: nullable}, nil
		}
		if schema.Format != "" && schema.Format != "int64" {
			return typeSpec{}, fmt.Errorf("integer format %q is unsupported", schema.Format)
		}
		if schema.ExclusiveMin || schema.ExclusiveMax || schema.MultipleOf != nil || len(schema.Enum) != 0 {
			return typeSpec{}, fmt.Errorf("integer requires inclusive bounds and does not support multipleOf or enum")
		}
		if schema.Min == nil || schema.Max == nil || *schema.Min < -maxExactJSONInteger || *schema.Max > maxExactJSONInteger || math.Trunc(*schema.Min) != *schema.Min || math.Trunc(*schema.Max) != *schema.Max || *schema.Min > *schema.Max {
			return typeSpec{}, fmt.Errorf("integer requires inclusive integral minimum/maximum within ±%d", maxExactJSONInteger)
		}
		minimum := int64(*schema.Min)
		maximum := int64(*schema.Max)
		return typeSpec{Kind: "int64", Nullable: nullable, Minimum: &minimum, Maximum: &maximum}, nil
	case "number":
		format := schema.Format
		if format == "" {
			format = "double"
		}
		if format != "float" && format != "double" {
			return typeSpec{}, fmt.Errorf("number format %q is unsupported", schema.Format)
		}
		if schema.MultipleOf != nil || schema.ExclusiveMin || schema.ExclusiveMax || len(schema.Enum) != 0 {
			return typeSpec{}, fmt.Errorf("floating-point multipleOf, exclusive bounds, and enum are unsupported")
		}
		return typeSpec{Kind: format, Nullable: nullable, NumberMinimum: schema.Min, NumberMaximum: schema.Max}, nil
	case "array":
		if schema.UniqueItems || len(schema.Enum) != 0 {
			return typeSpec{}, fmt.Errorf("uniqueItems and array enum are unsupported")
		}
		if schema.MaxItems != nil && *schema.MaxItems > maxPortableArrayItems {
			return typeSpec{}, fmt.Errorf("array maxItems exceeds portable C limit %d", maxPortableArrayItems)
		}
		if schema.MaxItems != nil && schema.MinItems > *schema.MaxItems {
			return typeSpec{}, fmt.Errorf("array minItems exceeds maxItems")
		}
		item, err := builder.typeFromSchema(schema.Items, inlineName+"_item")
		if err != nil {
			return typeSpec{}, fmt.Errorf("array item: %w", err)
		}
		if item.Kind == "array" {
			return typeSpec{}, fmt.Errorf("nested arrays are unsupported")
		}
		if item.Nullable {
			return typeSpec{}, fmt.Errorf("nullable array items are unsupported")
		}
		value := typeSpec{Kind: "array", Nullable: nullable, MinItems: schema.MinItems, Item: &item}
		if schema.MaxItems != nil {
			value.MaxItems = *schema.MaxItems
			value.MaxItemsSet = true
		}
		return value, nil
	default:
		return typeSpec{}, fmt.Errorf("type %q is unsupported", schema.Type.Slice()[0])
	}
}

func componentName(reference string) string {
	const prefix = "#/components/schemas/"
	if strings.HasPrefix(reference, prefix) && !strings.Contains(strings.TrimPrefix(reference, prefix), "/") {
		name, err := url.PathUnescape(strings.TrimPrefix(reference, prefix))
		if err == nil {
			return name
		}
	}
	return ""
}

func validateReferenceTree(rootPath string) error {
	rootDirectory, err := filepath.EvalSymlinks(filepath.Dir(rootPath))
	if err != nil {
		return fmt.Errorf("resolve schema root: %w", err)
	}
	rootFile, err := filepath.EvalSymlinks(rootPath)
	if err != nil {
		return fmt.Errorf("resolve input: %w", err)
	}
	return visitReferenceFile(rootFile, rootDirectory, make(map[string]bool), make(map[string]bool))
}

type referenceOccurrence struct {
	value   string
	pointer string
}

func visitReferenceFile(path string, rootDirectory string, visiting map[string]bool, visited map[string]bool) error {
	if visiting[path] {
		return fmt.Errorf("cyclic external reference involving %s", path)
	}
	if visited[path] {
		return nil
	}
	visiting[path] = true
	defer delete(visiting, path)
	information, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("inspect schema %s: %w", path, err)
	}
	if !information.Mode().IsRegular() {
		return fmt.Errorf("schema %s must be a regular file", path)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("read schema %s: %w", path, err)
	}
	var document any
	if err := json.Unmarshal(data, &document); err != nil {
		return fmt.Errorf("parse JSON schema %s: %w", path, err)
	}
	var references []referenceOccurrence
	collectReferences(document, "", &references)
	sort.Slice(references, func(left, right int) bool {
		if references[left].pointer == references[right].pointer {
			return references[left].value < references[right].value
		}
		return references[left].pointer < references[right].pointer
	})
	for _, occurrence := range references {
		reference := occurrence.value
		if strings.HasPrefix(reference, "#") {
			continue
		}
		parsed, err := url.Parse(reference)
		if err != nil || parsed.Scheme != "" || parsed.Host != "" || parsed.User != nil || parsed.RawQuery != "" || parsed.ForceQuery || filepath.IsAbs(parsed.Path) || parsed.Path == "" {
			return fmt.Errorf("reference %q at %s must be a local relative file or fragment", reference, occurrence.pointer)
		}
		candidate, err := filepath.EvalSymlinks(filepath.Join(filepath.Dir(path), filepath.FromSlash(parsed.Path)))
		if err != nil {
			return fmt.Errorf("resolve reference %q at %s: %w", reference, occurrence.pointer, err)
		}
		relative, err := filepath.Rel(rootDirectory, candidate)
		if err != nil || relative == ".." || strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
			return fmt.Errorf("reference %q at %s escapes schema root", reference, occurrence.pointer)
		}
		if filepath.Ext(candidate) != ".json" {
			return fmt.Errorf("reference %q at %s must target a JSON document", reference, occurrence.pointer)
		}
		if err := visitReferenceFile(candidate, rootDirectory, visiting, visited); err != nil {
			return fmt.Errorf("reference %q at %s: %w", reference, occurrence.pointer, err)
		}
	}
	visited[path] = true
	return nil
}

func collectReferences(value any, pointer string, output *[]referenceOccurrence) {
	switch typed := value.(type) {
	case map[string]any:
		keys := make([]string, 0, len(typed))
		for key := range typed {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		for _, key := range keys {
			child := typed[key]
			childPointer := pointer + "/" + strings.ReplaceAll(strings.ReplaceAll(key, "~", "~0"), "/", "~1")
			if key == "$ref" {
				if reference, ok := child.(string); ok {
					*output = append(*output, referenceOccurrence{value: reference, pointer: childPointer})
				}
				continue
			}
			collectReferences(child, childPointer, output)
		}
	case []any:
		for index, child := range typed {
			collectReferences(child, fmt.Sprintf("%s/%d", pointer, index), output)
		}
	}
}
