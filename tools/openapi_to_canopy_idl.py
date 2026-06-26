#!/usr/bin/env python3
#
# Copyright (c) 2026 Edward Boggis-Rolfe
# All rights reserved.

"""Convert a small OpenAPI/Swagger JSON surface into Canopy IDL.

The converter intentionally supports the conservative subset used by generated
REST caller tests and simple compatibility examples:

* Swagger 2.0 and OpenAPI 3.x JSON.
* local JSON pointer $ref values.
* object schemas, arrays, maps, primitive scalars, and optional fields.
* path/query/header parameters and one application/json body.

By default the converter is permissive for broad REST-client generation: safe
``allOf`` object composition is flattened, while ambiguous ``oneOf``/``anyOf``
and complex ``allOf`` shapes fall back to ``json::v1::object``. Use
``--strict-composition`` to make those fallbacks fail with a specific
diagnostic instead.
"""

from __future__ import annotations

import argparse
import copy
import json
import keyword
import os
import posixpath
import re
import sys
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import parse_qsl, urlparse


HTTP_METHODS = {"get", "put", "post", "delete", "patch", "head", "options", "trace"}
HTTP_NO_RESPONSE_BODY_STATUSES = {"204", "205"}
JSON_SCHEMA_METADATA_KEYS = {
    "description",
    "title",
    "deprecated",
    "example",
    "examples",
    "externalDocs",
    "xml",
}
CPP_KEYWORDS = {
    "alignas",
    "alignof",
    "and",
    "and_eq",
    "asm",
    "atomic_cancel",
    "atomic_commit",
    "atomic_noexcept",
    "auto",
    "bitand",
    "bitor",
    "bool",
    "break",
    "case",
    "catch",
    "char",
    "char8_t",
    "char16_t",
    "char32_t",
    "class",
    "compl",
    "concept",
    "const",
    "consteval",
    "constexpr",
    "constinit",
    "const_cast",
    "continue",
    "co_await",
    "co_return",
    "co_yield",
    "decltype",
    "default",
    "delete",
    "do",
    "double",
    "dynamic_cast",
    "else",
    "enum",
    "explicit",
    "export",
    "extern",
    "false",
    "float",
    "for",
    "friend",
    "goto",
    "if",
    "inline",
    "int",
    "long",
    "mutable",
    "namespace",
    "new",
    "noexcept",
    "not",
    "not_eq",
    "nullptr",
    "operator",
    "or",
    "or_eq",
    "private",
    "protected",
    "public",
    "reflexpr",
    "register",
    "reinterpret_cast",
    "requires",
    "return",
    "short",
    "signed",
    "sizeof",
    "static",
    "static_assert",
    "static_cast",
    "struct",
    "switch",
    "synchronized",
    "template",
    "this",
    "thread_local",
    "throw",
    "true",
    "try",
    "typedef",
    "typeid",
    "typename",
    "union",
    "unsigned",
    "using",
    "virtual",
    "void",
    "volatile",
    "wchar_t",
    "while",
    "xor",
    "xor_eq",
}
CANOPY_IDL_KEYWORDS = {
    "cpp_quote",
    "enum",
    "import",
    "in",
    "interface",
    "namespace",
    "out",
    "struct",
    "typedef",
}
RESERVED_IDENTIFIERS = CPP_KEYWORDS | CANOPY_IDL_KEYWORDS


@dataclass
class ConvertOptions:
    strict_composition: bool = False


def snake_case(value: str) -> str:
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", value)
    value = re.sub(r"[^0-9A-Za-z_]+", "_", value)
    value = re.sub(r"_+", "_", value).strip("_").lower()
    if not value:
        value = "value"
    if value[0].isdigit():
        value = f"value_{value}"
    if keyword.iskeyword(value) or value in RESERVED_IDENTIFIERS:
        value = f"{value}_value"
    return value


def type_name(value: str) -> str:
    return snake_case(value)


def unique_identifier(base: str, used: set[str]) -> str:
    candidate = base
    suffix = 2
    while candidate in used:
        candidate = f"{base}_{suffix}"
        suffix += 1
    used.add(candidate)
    return candidate


def local_ref_name(ref: str) -> str:
    if not ref.startswith("#/"):
        raise ValueError(f"only local $ref values are supported, got: {ref}")
    return ref.rsplit("/", 1)[-1]


def json_pointer_get(document: dict[str, Any], ref: str) -> Any:
    if not ref.startswith("#/"):
        raise ValueError(f"only local $ref values are supported, got: {ref}")
    current: Any = document
    for raw_part in ref[2:].split("/"):
        part = raw_part.replace("~1", "/").replace("~0", "~")
        if not isinstance(current, dict) or part not in current:
            raise ValueError(f"unresolved local $ref: {ref}")
        current = current[part]
    return current


def resolve_ref(document: dict[str, Any], schema: dict[str, Any]) -> dict[str, Any]:
    if "$ref" not in schema:
        return schema
    resolved = copy.deepcopy(json_pointer_get(document, schema["$ref"]))
    overlay = {key: value for key, value in schema.items() if key != "$ref"}
    resolved.update(overlay)
    return resolved


def schema_map(spec: dict[str, Any]) -> dict[str, Any]:
    if "swagger" in spec:
        return spec.get("definitions", {})
    return spec.get("components", {}).get("schemas", {})


def first_json_schema(content: dict[str, Any]) -> dict[str, Any] | None:
    for media_type in ("application/json", "application/*+json", "*/*"):
        media = content.get(media_type)
        if isinstance(media, dict) and isinstance(media.get("schema"), dict):
            return media["schema"]
    for media in content.values():
        if isinstance(media, dict) and isinstance(media.get("schema"), dict):
            return media["schema"]
    return None


def response_schema(spec: dict[str, Any], operation: dict[str, Any]) -> dict[str, Any] | None:
    responses = operation.get("responses", {})
    ordered = sorted(
        responses.items(),
        key=lambda item: (0 if str(item[0]).startswith("2") else 1, str(item[0])),
    )
    for status, response in ordered:
        status_text = str(status)
        if not (status_text.startswith("2") or status_text == "default"):
            continue
        if status_text in HTTP_NO_RESPONSE_BODY_STATUSES:
            return None
        if "$ref" in response:
            response = resolve_ref(spec, response)
        if "schema" in response:
            return response["schema"]
        content = response.get("content")
        if isinstance(content, dict):
            schema = first_json_schema(content)
            if schema is not None:
                return schema
    return None


def request_body_schema(spec: dict[str, Any], operation: dict[str, Any]) -> dict[str, Any] | None:
    body = operation.get("requestBody")
    if isinstance(body, dict):
        if "$ref" in body:
            body = resolve_ref(spec, body)
        content = body.get("content")
        if isinstance(content, dict):
            return first_json_schema(content)
    for parameter in operation.get("parameters", []):
        if "$ref" in parameter:
            parameter = resolve_ref(spec, parameter)
        if parameter.get("in") == "body" and isinstance(parameter.get("schema"), dict):
            return parameter["schema"]
    return None


def merged_parameters(spec: dict[str, Any], path_item: dict[str, Any], operation: dict[str, Any]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for raw_parameter in path_item.get("parameters", []) + operation.get("parameters", []):
        parameter = resolve_ref(spec, raw_parameter) if "$ref" in raw_parameter else raw_parameter
        if parameter.get("in") in {"path", "query", "header"}:
            output.append(parameter)
    return output


def schema_fallback_or_raise(
    fallback_name: str,
    reason: str,
    options: ConvertOptions,
) -> str:
    if options.strict_composition:
        raise ValueError(f"{fallback_name}: {reason}")
    return "json::v1::object"


def schema_json_equal(lhs: Any, rhs: Any) -> bool:
    return json.dumps(lhs, sort_keys=True, separators=(",", ":")) == json.dumps(rhs, sort_keys=True, separators=(",", ":"))


def merge_properties(
    target: dict[str, Any],
    source: dict[str, Any],
    fallback_name: str,
) -> bool:
    for name, schema in source.items():
        existing = target.get(name)
        if existing is not None and not schema_json_equal(existing, schema):
            return False
        target[name] = schema
    return True


def flatten_allof_schema(
    spec: dict[str, Any],
    schema: dict[str, Any],
    fallback_name: str,
    options: ConvertOptions,
) -> dict[str, Any] | None:
    schema = resolve_ref(spec, schema)
    all_of = schema.get("allOf")
    if not isinstance(all_of, list):
        return schema

    merged_properties: dict[str, Any] = {}
    merged_required: set[str] = set()

    def merge_object_branch(branch: dict[str, Any], branch_name: str) -> bool:
        branch = resolve_ref(spec, branch)
        if "oneOf" in branch or "anyOf" in branch:
            return False
        if "allOf" in branch:
            flattened = flatten_allof_schema(spec, branch, branch_name, options)
            if flattened is None:
                return False
            branch = flattened

        branch_type = branch.get("type")
        branch_properties = branch.get("properties")
        if branch_type not in (None, "object") and not isinstance(branch_properties, dict):
            return False
        if branch_type is None and not isinstance(branch_properties, dict):
            # Metadata-only branches do not affect the generated struct.
            return True

        additional = branch.get("additionalProperties")
        if isinstance(additional, dict):
            return False
        if isinstance(branch_properties, dict) and not merge_properties(merged_properties, branch_properties, branch_name):
            return False
        merged_required.update(str(item) for item in branch.get("required", []) if isinstance(item, str))
        return True

    for index, branch in enumerate(all_of):
        if not isinstance(branch, dict) or not merge_object_branch(branch, f"{fallback_name}_all_of_{index}"):
            return None

    own_properties = schema.get("properties")
    if isinstance(own_properties, dict) and not merge_properties(merged_properties, own_properties, fallback_name):
        return None
    merged_required.update(str(item) for item in schema.get("required", []) if isinstance(item, str))

    if not merged_properties and not merged_required:
        return None

    flattened: dict[str, Any] = {
        "type": "object",
        "properties": merged_properties,
    }
    if merged_required:
        flattened["required"] = sorted(merged_required)
    return flattened


def single_schema_allof_branch(schema: dict[str, Any]) -> dict[str, Any] | None:
    """Return the single semantic branch for metadata-only allOf wrappers."""
    all_of = schema.get("allOf")
    if not isinstance(all_of, list):
        return None
    if any(key not in JSON_SCHEMA_METADATA_KEYS | {"allOf"} for key in schema):
        return None

    data_branches: list[dict[str, Any]] = []
    for branch in all_of:
        if not isinstance(branch, dict):
            return None
        if any(key not in JSON_SCHEMA_METADATA_KEYS for key in branch):
            data_branches.append(branch)
    return data_branches[0] if len(data_branches) == 1 else None


def schema_is_named_struct_candidate(
    spec: dict[str, Any],
    schema: dict[str, Any],
    fallback_name: str,
    options: ConvertOptions,
) -> bool:
    schema = resolve_ref(spec, schema)
    if "oneOf" in schema or "anyOf" in schema:
        return False
    if "allOf" in schema:
        schema = flatten_allof_schema(spec, schema, fallback_name, options)
        if schema is None:
            return False
    return bool(schema.get("properties")) and not object_schema_has_duplicate_cpp_fields(schema)


def response_object_fields_schema(
    spec: dict[str, Any],
    schema: dict[str, Any],
    fallback_name: str,
    options: ConvertOptions,
) -> dict[str, Any] | None:
    schema = resolve_ref(spec, schema)
    if "oneOf" in schema or "anyOf" in schema:
        return None
    if "allOf" in schema:
        flattened = flatten_allof_schema(spec, schema, fallback_name, options)
        if flattened is None:
            return None
        schema = flattened
    schema_type = schema.get("type")
    if isinstance(schema_type, list):
        non_null = [item for item in schema_type if item != "null"]
        schema_type = non_null[0] if len(non_null) == 1 else schema_type
    properties = schema.get("properties")
    if schema_type not in (None, "object") and not isinstance(properties, dict):
        return None
    if not isinstance(properties, dict) or not properties:
        return None
    if object_schema_has_duplicate_cpp_fields(schema):
        return None
    return schema


def object_schema_has_duplicate_cpp_fields(schema: dict[str, Any]) -> bool:
    properties = schema.get("properties")
    if not isinstance(properties, dict):
        return False
    seen: set[str] = set()
    for raw_field_name in properties:
        field_name = snake_case(raw_field_name)
        if field_name in seen:
            return True
        seen.add(field_name)
    return False


def schema_to_cpp_type(
    spec: dict[str, Any],
    schema: dict[str, Any],
    fallback_name: str,
    extra_structs: dict[str, dict[str, Any]],
    options: ConvertOptions,
    active_structs: set[str] | None = None,
) -> str:
    active_structs = active_structs or set()
    if "$ref" in schema:
        ref_name = local_ref_name(schema["$ref"])
        cpp_ref_name = type_name(ref_name)
        if cpp_ref_name in active_structs:
            return schema_fallback_or_raise(
                fallback_name,
                f"recursive schema reference {ref_name!r} cannot be represented by value",
                options,
            )
        resolved = resolve_ref(spec, schema)
        if schema_is_named_struct_candidate(spec, resolved, ref_name, options):
            return cpp_ref_name
        return schema_to_cpp_type(spec, resolved, ref_name, extra_structs, options, active_structs)
    schema = resolve_ref(spec, schema)
    if "oneOf" in schema:
        return schema_fallback_or_raise(fallback_name, "oneOf is not representable as a strong Canopy type", options)
    if "anyOf" in schema:
        return schema_fallback_or_raise(fallback_name, "anyOf is not representable as a strong Canopy type", options)
    if "allOf" in schema:
        wrapped = single_schema_allof_branch(schema)
        if wrapped is not None:
            return schema_to_cpp_type(spec, wrapped, fallback_name, extra_structs, options, active_structs)
        flattened = flatten_allof_schema(spec, schema, fallback_name, options)
        if flattened is None:
            return schema_fallback_or_raise(fallback_name, "allOf cannot be safely flattened", options)
        schema = flattened
    schema_type = schema.get("type")
    schema_format = schema.get("format", "")
    if isinstance(schema_type, list):
        non_null = [item for item in schema_type if item != "null"]
        if len(non_null) == 1:
            schema_type = non_null[0]
        else:
            raise ValueError(f"{fallback_name}: union JSON schema types are not supported")
    if schema_type == "string" or "enum" in schema:
        return "std::string"
    if schema_type is None and "additionalProperties" in schema:
        additional = schema.get("additionalProperties")
        if isinstance(additional, dict):
            value_type = schema_to_cpp_type(
                spec, additional, f"{fallback_name}_value", extra_structs, options, active_structs)
            return f"std::map<std::string, {value_type}>"
        return "std::map<std::string, json::v1::object>"
    if schema_type is None and "properties" not in schema:
        return "json::v1::object"
    if schema_type == "boolean":
        return "bool"
    if schema_type == "integer":
        if schema_format == "int64":
            return "int64_t"
        if schema_format == "uint64":
            return "uint64_t"
        if schema_format in {"uint32", "uint"}:
            return "uint32_t"
        return "int32_t"
    if schema_type == "number":
        return "float" if schema_format == "float" else "double"
    if schema_type == "array":
        item_schema = schema.get("items")
        if not isinstance(item_schema, dict):
            raise ValueError(f"{fallback_name}: array schema is missing items")
        item_type = schema_to_cpp_type(spec, item_schema, f"{fallback_name}_item", extra_structs, options, active_structs)
        return f"std::vector<{item_type}>"
    if schema_type == "object" or "properties" in schema:
        if not schema.get("properties"):
            additional = schema.get("additionalProperties")
            if isinstance(additional, dict):
                value_type = schema_to_cpp_type(
                    spec, additional, f"{fallback_name}_value", extra_structs, options, active_structs)
                return f"std::map<std::string, {value_type}>"
            return "json::v1::object"
        if object_schema_has_duplicate_cpp_fields(schema):
            return "json::v1::object"
        generated_name = type_name(fallback_name)
        extra_structs.setdefault(generated_name, schema)
        return generated_name
    raise ValueError(f"{fallback_name}: unsupported schema type {schema_type!r}")


@dataclass
class Field:
    name: str
    cpp_type: str


@dataclass
class Struct:
    name: str
    fields: list[Field]


@dataclass
class Parameter:
    name: str
    wire_name: str
    location: str
    cpp_type: str
    required: bool


@dataclass
class Constant:
    location: str
    wire_name: str
    value: str


@dataclass
class ResponseField:
    name: str
    wire_name: str
    cpp_type: str
    required: bool


@dataclass
class Method:
    name: str
    http_method: str
    path: str
    parameters: list[Parameter] = field(default_factory=list)
    constants: list[Constant] = field(default_factory=list)
    body_param: str = ""
    body_type: str = ""
    response_type: str = ""
    out_param: str = ""
    response_fields: list[ResponseField] = field(default_factory=list)


@dataclass
class Model:
    namespaces: list[str]
    interface_name: str
    host: str
    base_path: str
    structs: list[Struct]
    methods: list[Method]

    @property
    def qualified_interface(self) -> str:
        return "::".join([*self.namespaces, self.interface_name])


def make_method_name(http_method: str, operation: dict[str, Any], path: str, used: set[str]) -> str:
    verb = http_method.lower()
    base = operation.get("operationId")
    if not base:
        literal_segments = [segment for segment in path.split("/") if segment and not segment.startswith("{")]
        base = "_".join([verb, *literal_segments[-2:]])
    name = snake_case(base)
    if name != verb and not name.startswith(f"{verb}_"):
        name = f"{verb}_{name}"
    original = name
    suffix = 2
    while name in used:
        name = f"{original}_{suffix}"
        suffix += 1
    used.add(name)
    return name


def literal_segments(path: str) -> list[str]:
    return [snake_case(segment) for segment in path.split("/") if segment and not segment.startswith("{")]


def operation_path(path: str) -> str:
    path = path.split("#", 1)[0]
    return path or "/"


def operation_fragment_constants(path: str, parameters: list[dict[str, Any]]) -> list[Constant]:
    if "#" not in path:
        return []
    fragment = path.split("#", 1)[1]
    if not fragment:
        return []
    parameter_locations = {
        str(parameter.get("name")): str(parameter.get("in"))
        for parameter in parameters
        if parameter.get("in") in {"query", "header"}
    }
    constants: list[Constant] = []
    for name, value in parse_qsl(fragment, keep_blank_values=True):
        location = parameter_locations.get(name)
        if location is None:
            location = "header" if name.lower().startswith("x-") else "query"
        constants.append(Constant(location=location, wire_name=name, value=value))
    return constants


def constant_value_to_string(value: Any) -> str | None:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float, str)):
        return str(value)
    return None


def single_enum_constant(parameter: dict[str, Any]) -> str | None:
    schema = parameter.get("schema") or {
        key: parameter[key]
        for key in ("type", "format", "items", "enum")
        if key in parameter
    }
    if not isinstance(schema, dict):
        return None
    enum = schema.get("enum")
    if not isinstance(enum, list) or len(enum) != 1:
        return None
    return constant_value_to_string(enum[0])


def constant_key(constant: Constant) -> tuple[str, str]:
    return constant.location, constant.wire_name.lower()


def parameter_key(parameter: dict[str, Any]) -> tuple[str, str]:
    return str(parameter.get("in", "")), str(parameter.get("name", "")).lower()


def add_constant(constants: list[Constant], constant: Constant) -> None:
    key = constant_key(constant)
    for existing in constants:
        if constant_key(existing) == key:
            existing.value = constant.value
            return
    constants.append(constant)


def common_namespace(spec: dict[str, Any]) -> list[str]:
    base_path = spec.get("basePath", "")
    paths = [path for path, item in spec.get("paths", {}).items() if isinstance(item, dict)]
    literal_paths = []
    for path in paths:
        combined = posixpath.join("/", base_path.strip("/"), path.strip("/"))
        segments = literal_segments(combined)
        if segments:
            literal_paths.append(segments)
    if not literal_paths:
        return ["rest"]
    prefix = literal_paths[0]
    for path_segments in literal_paths[1:]:
        count = 0
        for lhs, rhs in zip(prefix, path_segments):
            if lhs != rhs:
                break
            count += 1
        prefix = prefix[:count]
    return prefix or literal_paths[0]


def configured_namespace(spec: dict[str, Any]) -> list[str]:
    raw_namespace = spec.get("x-canopy-namespace")
    if isinstance(raw_namespace, str):
        namespaces = [part for part in re.split(r"::|[./]", raw_namespace) if part]
    elif isinstance(raw_namespace, list):
        namespaces = [part for part in raw_namespace if isinstance(part, str) and part]
    else:
        namespaces = []
    return [snake_case(namespace) for namespace in namespaces] or common_namespace(spec)


def configured_interface_name(spec: dict[str, Any], namespaces: list[str]) -> str:
    raw_interface = spec.get("x-canopy-interface")
    if isinstance(raw_interface, str) and raw_interface:
        return snake_case(raw_interface)
    return f"i_{namespaces[-1]}"


def server_defaults(spec: dict[str, Any]) -> tuple[str, str]:
    if "swagger" in spec:
        return spec.get("host", ""), spec.get("basePath", "")
    servers = spec.get("servers", [])
    if servers and isinstance(servers[0], dict):
        parsed = urlparse(servers[0].get("url", ""))
        return parsed.netloc, parsed.path.rstrip("/")
    return "", ""


def build_struct(
    spec: dict[str, Any],
    name: str,
    schema: dict[str, Any],
    extra_structs: dict[str, dict[str, Any]],
    options: ConvertOptions,
    active_structs: set[str] | None = None,
) -> Struct:
    active_structs = set(active_structs or ())
    active_structs.add(type_name(name))
    schema = resolve_ref(spec, schema)
    if "allOf" in schema:
        flattened = flatten_allof_schema(spec, schema, name, options)
        if flattened is None:
            raise ValueError(f"schema {name} is not a safely flattenable object schema")
        schema = flattened
    if schema.get("type") != "object" and "properties" not in schema:
        raise ValueError(f"schema {name} is not an object schema")
    required = set(schema.get("required", []))
    fields: list[Field] = []
    for raw_field_name, field_schema in schema.get("properties", {}).items():
        field_name = snake_case(raw_field_name)
        cpp_type = schema_to_cpp_type(
            spec, field_schema, f"{name}_{field_name}", extra_structs, options, active_structs)
        if raw_field_name not in required:
            cpp_type = f"rpc::optional<{cpp_type}>"
        fields.append(Field(field_name, cpp_type))

    type_identifiers = {identifier for field in fields for identifier in cpp_type_identifiers(field.cpp_type)}
    fields.sort(key=lambda field: field.name in type_identifiers)
    return Struct(type_name(name), fields)


def cpp_type_identifiers(cpp_type: str) -> set[str]:
    return set(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", cpp_type))


def sort_structs_by_dependency(structs: list[Struct]) -> list[Struct]:
    by_name = {struct.name: struct for struct in structs}
    dependencies = {
        struct.name: {
            identifier
            for field in struct.fields
            for identifier in cpp_type_identifiers(field.cpp_type)
            if identifier in by_name and identifier != struct.name
        }
        for struct in structs
    }

    ordered: list[Struct] = []
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(name: str) -> None:
        if name in visited:
            return
        if name in visiting:
            return
        visiting.add(name)
        for dependency in sorted(dependencies[name]):
            visit(dependency)
        visiting.remove(name)
        visited.add(name)
        ordered.append(by_name[name])

    for struct in structs:
        visit(struct.name)
    return ordered


def cyclic_struct_names(structs: list[Struct]) -> set[str]:
    by_name = {struct.name: struct for struct in structs}
    dependencies = {
        struct.name: {
            identifier
            for field in struct.fields
            for identifier in cpp_type_identifiers(field.cpp_type)
            if identifier in by_name and identifier != struct.name
        }
        for struct in structs
    }
    cyclic: set[str] = set()
    visiting: list[str] = []
    visited: set[str] = set()

    def visit(name: str) -> None:
        if name in visited:
            return
        if name in visiting:
            cycle_start = visiting.index(name)
            cyclic.update(visiting[cycle_start:])
            return
        visiting.append(name)
        for dependency in sorted(dependencies[name]):
            visit(dependency)
        visiting.pop()
        visited.add(name)

    for struct in structs:
        visit(struct.name)
    return cyclic


def replace_type_identifiers(cpp_type: str, replacements: set[str]) -> str:
    if not replacements:
        return cpp_type
    pattern = re.compile(r"\b(" + "|".join(re.escape(name) for name in sorted(replacements, key=len, reverse=True)) + r")\b")
    return pattern.sub("json::v1::object", cpp_type)


def rename_cpp_type_identifiers(cpp_type: str, replacements: dict[str, str]) -> str:
    if not replacements:
        return cpp_type
    pattern = re.compile(r"\b(" + "|".join(re.escape(name) for name in sorted(replacements, key=len, reverse=True)) + r")\b")
    return pattern.sub(lambda match: replacements[match.group(1)], cpp_type)


def avoid_struct_method_name_collisions(structs: list[Struct], methods: list[Method]) -> list[Struct]:
    method_names = {method.name for method in methods}
    struct_names = {struct.name for struct in structs}
    used_names = set(method_names) | set(struct_names)
    replacements: dict[str, str] = {}

    for struct in structs:
        if struct.name not in method_names:
            continue
        used_names.remove(struct.name)
        replacements[struct.name] = unique_identifier(f"{struct.name}_type", used_names)

    if not replacements:
        return structs

    for struct in structs:
        struct.name = replacements.get(struct.name, struct.name)
        for field in struct.fields:
            field.cpp_type = rename_cpp_type_identifiers(field.cpp_type, replacements)
    for method in methods:
        method.body_type = rename_cpp_type_identifiers(method.body_type, replacements)
        method.response_type = rename_cpp_type_identifiers(method.response_type, replacements)
        for field in method.response_fields:
            field.cpp_type = rename_cpp_type_identifiers(field.cpp_type, replacements)
        for parameter in method.parameters:
            parameter.cpp_type = rename_cpp_type_identifiers(parameter.cpp_type, replacements)
    return structs


def collapse_cyclic_structs(structs: list[Struct], methods: list[Method]) -> list[Struct]:
    while True:
        cyclic = cyclic_struct_names(structs)
        if not cyclic:
            return structs

        structs = [struct for struct in structs if struct.name not in cyclic]
        for struct in structs:
            for field in struct.fields:
                field.cpp_type = replace_type_identifiers(field.cpp_type, cyclic)
        for method in methods:
            method.body_type = replace_type_identifiers(method.body_type, cyclic)
            method.response_type = replace_type_identifiers(method.response_type, cyclic)
            for field in method.response_fields:
                field.cpp_type = replace_type_identifiers(field.cpp_type, cyclic)
            for parameter in method.parameters:
                parameter.cpp_type = replace_type_identifiers(parameter.cpp_type, cyclic)


def build_model(spec: dict[str, Any], options: ConvertOptions = ConvertOptions()) -> Model:
    if "swagger" not in spec and "openapi" not in spec:
        raise ValueError("input is neither Swagger 2.0 nor OpenAPI 3.x JSON")
    if "openapi" in spec and not str(spec["openapi"]).startswith("3."):
        raise ValueError(f"unsupported OpenAPI version {spec['openapi']!r}")
    if "swagger" in spec and str(spec["swagger"]) != "2.0":
        raise ValueError(f"unsupported Swagger version {spec['swagger']!r}")

    extra_structs: dict[str, dict[str, Any]] = {}
    structs_by_name: dict[str, Struct] = {}
    for raw_name, raw_schema in schema_map(spec).items():
        if schema_is_named_struct_candidate(spec, raw_schema, raw_name, options):
            struct = build_struct(spec, raw_name, raw_schema, extra_structs, options)
            structs_by_name[struct.name] = struct

    methods: list[Method] = []
    used_methods: set[str] = set()
    for path, path_item in spec.get("paths", {}).items():
        if not isinstance(path_item, dict):
            continue
        for http_method, operation in path_item.items():
            if http_method.lower() not in HTTP_METHODS or not isinstance(operation, dict):
                continue
            used_parameters: set[str] = set(structs_by_name)
            method = Method(
                name=make_method_name(http_method, operation, path, used_methods),
                http_method=http_method.upper(),
                path=operation_path(path),
            )
            parameters = merged_parameters(spec, path_item, operation)
            for constant in operation_fragment_constants(path, parameters):
                add_constant(method.constants, constant)
            fixed_parameter_keys = {constant_key(constant) for constant in method.constants if constant.value != ""}
            for parameter in parameters:
                enum_constant = single_enum_constant(parameter)
                if enum_constant is not None and parameter.get("in") in {"query", "header"}:
                    add_constant(
                        method.constants,
                        Constant(
                            location=parameter["in"],
                            wire_name=parameter["name"],
                            value=enum_constant,
                        ),
                    )
                    fixed_parameter_keys.add(parameter_key(parameter))
            for parameter in parameters:
                if parameter.get("in") in {"query", "header"} and parameter_key(parameter) in fixed_parameter_keys:
                    continue
                if parameter.get("in") == "header":
                    continue
                parameter_schema = parameter.get("schema") or {
                    key: parameter[key]
                    for key in ("type", "format", "items", "enum")
                    if key in parameter
                }
                if not isinstance(parameter_schema, dict):
                    raise ValueError(f"parameter {parameter.get('name')} has no schema")
                required = bool(parameter.get("required", False)) or parameter.get("in") == "path"
                cpp_type = schema_to_cpp_type(
                    spec, parameter_schema, f"{method.name}_{parameter['name']}", extra_structs, options)
                if not required:
                    cpp_type = f"rpc::optional<{cpp_type}>"
                method.parameters.append(
                    Parameter(
                        name=unique_identifier(snake_case(parameter["name"]), used_parameters),
                        wire_name=parameter["name"],
                        location=parameter["in"],
                        cpp_type=cpp_type,
                        required=required,
                    )
                )
            body_schema = request_body_schema(spec, operation)
            if body_schema is not None:
                method.body_type = schema_to_cpp_type(spec, body_schema, f"{method.name}_request", extra_structs, options)
                method.body_param = unique_identifier("request", used_parameters)
            raw_response_schema = response_schema(spec, operation)
            if raw_response_schema is not None:
                response_fields_schema = response_object_fields_schema(
                    spec, raw_response_schema, f"{method.name}_response", options)
                if response_fields_schema is not None:
                    required_response_fields = set(response_fields_schema.get("required", []))
                    for raw_field_name, field_schema in response_fields_schema.get("properties", {}).items():
                        field_name = unique_identifier(snake_case(raw_field_name), used_parameters)
                        cpp_type = schema_to_cpp_type(
                            spec, field_schema, f"{method.name}_{field_name}", extra_structs, options)
                        required = raw_field_name in required_response_fields
                        if not required:
                            cpp_type = f"rpc::optional<{cpp_type}>"
                        method.response_fields.append(
                            ResponseField(
                                name=field_name,
                                wire_name=raw_field_name,
                                cpp_type=cpp_type,
                                required=required,
                            )
                        )
                else:
                    method.response_type = schema_to_cpp_type(
                        spec, raw_response_schema, f"{method.name}_response", extra_structs, options)
                    method.out_param = unique_identifier("response", used_parameters)
            methods.append(method)

    while extra_structs:
        pending = extra_structs
        extra_structs = {}
        for name, schema in pending.items():
            if name not in structs_by_name:
                structs_by_name[name] = build_struct(spec, name, schema, extra_structs, options)

    namespaces = configured_namespace(spec)
    host, base_path = server_defaults(spec)
    structs = avoid_struct_method_name_collisions(list(structs_by_name.values()), methods)
    structs = collapse_cyclic_structs(structs, methods)
    return Model(
        namespaces=namespaces,
        interface_name=configured_interface_name(spec, namespaces),
        host=host,
        base_path=base_path,
        structs=sort_structs_by_dependency(structs),
        methods=methods,
    )


def parameter_idl(parameter: Parameter) -> str:
    if parameter.cpp_type.startswith("rpc::optional<") or parameter.cpp_type in {
        "bool",
        "int32_t",
        "int64_t",
        "uint32_t",
        "uint64_t",
        "float",
        "double",
    }:
        return f"[in] {parameter.cpp_type} {parameter.name}"
    return f"[in] const {parameter.cpp_type}& {parameter.name}"


def model_uses_json_dom(model: Model) -> bool:
    for struct in model.structs:
        for field in struct.fields:
            if "json::v1::" in field.cpp_type:
                return True
    for method in model.methods:
        if "json::v1::" in method.body_type or "json::v1::" in method.response_type:
            return True
        for field in method.response_fields:
            if "json::v1::" in field.cpp_type:
                return True
        for parameter in method.parameters:
            if "json::v1::" in parameter.cpp_type:
                return True
    return False


def write_idl(model: Model, source_path: str, overlay_paths: list[str] | None = None) -> str:
    overlay_names = [os.path.basename(path) for path in overlay_paths or []]
    lines: list[str] = [
        "/*",
        f" * Generated from {os.path.basename(source_path)} by openapi_to_canopy_idl.py.",
    ]
    if overlay_names:
        lines.append(f" * Applied overlays: {', '.join(overlay_names)}.")
        lines.append(" * Treat the OpenAPI/Swagger JSON plus overlays as the source of truth.")
    else:
        lines.append(" * Treat the OpenAPI/Swagger JSON as the source of truth.")
    lines.extend(
        [
            " */",
            "",
            '#import "rpc/rpc_types.idl"',
        ]
    )
    if model_uses_json_dom(model):
        lines.append('#import "json/json.idl"')
    lines.extend(
        [
            "",
            "typedef int error_code;",
            "",
        ]
    )
    indent = ""
    for namespace in model.namespaces:
        lines.append(f"{indent}namespace {namespace}")
        lines.append(f"{indent}{{")
        indent += "    "
    for struct in model.structs:
        lines.append(f"{indent}struct {struct.name}")
        lines.append(f"{indent}{{")
        for field in struct.fields:
            lines.append(f"{indent}    {field.cpp_type} {field.name};")
        lines.append(f"{indent}}};")
        lines.append("")
    lines.append(f"{indent}interface {model.interface_name}")
    lines.append(f"{indent}{{")
    for method in model.methods:
        params = [parameter_idl(parameter) for parameter in method.parameters]
        if method.body_param:
            params.append(f"[in] const {method.body_type}& {method.body_param}")
        if method.response_type:
            params.append(f"[out] {method.response_type}& {method.out_param}")
        for field in method.response_fields:
            params.append(f"[out] {field.cpp_type}& {field.name}")
        if len(params) <= 1:
            lines.append(f"{indent}    error_code {method.name}({', '.join(params)});")
        else:
            lines.append(f"{indent}    error_code {method.name}(")
            for index, parameter in enumerate(params):
                suffix = "," if index + 1 < len(params) else ""
                lines.append(f"{indent}        {parameter}{suffix}")
            lines.append(f"{indent}    );")
    lines.append(f"{indent}}};")
    for namespace in reversed(model.namespaces):
        indent = indent[:-4]
        lines.append(f"{indent}}} // namespace {namespace}")
    lines.append("")
    return "\n".join(lines)


def output_binding_path(output_path: str) -> str:
    if output_path.endswith(".idl"):
        return output_path[: -len(".idl")] + ".rest.json"
    return output_path + ".rest.json"


def write_binding(model: Model, source_path: str, overlay_paths: list[str] | None = None) -> str:
    binding = {
        "schema": "canopy.rest.binding.v1",
        "source": {
            "openapi": os.path.basename(source_path),
            "overlays": [os.path.basename(path) for path in overlay_paths or []],
        },
        "interfaces": [
            {
                "qualified_name": model.qualified_interface,
                "host": model.host,
                "base_path": model.base_path,
                "methods": [
                    {
                        "name": method.name,
                        "http_method": method.http_method,
                        "path": method.path,
                        "request": {
                            "body_param": method.body_param,
                        },
                        "response": {
                            "out_param": method.out_param,
                            "has_body": bool(method.out_param or method.response_fields),
                            "fields": [
                                {
                                    "name": field.name,
                                    "wire_name": field.wire_name,
                                    "required": field.required,
                                }
                                for field in method.response_fields
                            ],
                        },
                        "parameters": [
                            {
                                "name": parameter.name,
                                "location": parameter.location,
                                "wire_name": parameter.wire_name,
                                "required": parameter.required,
                            }
                            for parameter in method.parameters
                        ],
                        "constants": [
                            {
                                "location": constant.location,
                                "wire_name": constant.wire_name,
                                "value": constant.value,
                            }
                            for constant in method.constants
                        ],
                    }
                    for method in model.methods
                ],
            }
        ],
    }
    return json.dumps(binding, indent=2, sort_keys=True) + "\n"


def write_if_different(path: str, content: str) -> None:
    old = None
    try:
        with open(path, "r", encoding="utf-8") as stream:
            old = stream.read()
    except FileNotFoundError:
        pass
    if old == content:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(content)


def deep_merge_json(base: Any, overlay: Any) -> Any:
    if isinstance(base, dict) and isinstance(overlay, dict):
        merged = dict(base)
        for key, value in overlay.items():
            merged[key] = deep_merge_json(merged[key], value) if key in merged else value
        return merged
    return overlay


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--overlay", action="append", default=[])
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--binding",
        default=None,
        help="optional structured REST binding JSON output; defaults to output path with .rest.json suffix",
    )
    parser.add_argument(
        "--strict-composition",
        action="store_true",
        help="fail instead of falling back to json::v1::object for ambiguous oneOf/anyOf/allOf schemas",
    )
    args = parser.parse_args()

    with open(args.input, "r", encoding="utf-8") as stream:
        spec = json.load(stream)
    for overlay_path in args.overlay:
        with open(overlay_path, "r", encoding="utf-8") as stream:
            spec = deep_merge_json(spec, json.load(stream))
    model = build_model(spec, ConvertOptions(strict_composition=args.strict_composition))
    write_if_different(args.output, write_idl(model, args.input, args.overlay))
    binding_path = args.binding or output_binding_path(args.output)
    write_if_different(binding_path, write_binding(model, args.input, args.overlay))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"openapi_to_canopy_idl.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
