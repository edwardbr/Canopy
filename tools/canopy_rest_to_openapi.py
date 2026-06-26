#!/usr/bin/env python3
#
# Copyright (c) 2026 Edward Boggis-Rolfe
# All rights reserved.

"""Export a Canopy REST IDL plus REST binding metadata as OpenAPI 3 JSON.

This is the publishing-side counterpart to openapi_to_canopy_idl.py. It is
intentionally conservative: it reads the IDL source and the structured REST
binding used by the generated rest_caller/rest_handler and emits enough OpenAPI
for simple HTTP/JSON clients to call the same surface.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class Field:
    name: str
    type_name: str
    required: bool = True


@dataclass
class Struct:
    name: str
    qualified_name: str
    namespace: tuple[str, ...]
    fields: list[Field] = field(default_factory=list)


@dataclass
class Enum:
    name: str
    qualified_name: str
    values: list[str] = field(default_factory=list)


@dataclass
class Parameter:
    name: str
    type_name: str
    direction: str


@dataclass
class Method:
    name: str
    return_type: str
    parameters: list[Parameter] = field(default_factory=list)


@dataclass
class Interface:
    name: str
    qualified_name: str
    namespace: tuple[str, ...]
    methods: dict[str, Method] = field(default_factory=dict)


@dataclass
class Model:
    typedefs: dict[str, str] = field(default_factory=dict)
    structs: dict[str, Struct] = field(default_factory=dict)
    enums: dict[str, Enum] = field(default_factory=dict)
    interfaces: dict[str, Interface] = field(default_factory=dict)
    unqualified_types: dict[str, str] = field(default_factory=dict)


@dataclass
class RestParameter:
    name: str
    location: str
    wire_name: str
    required: bool


@dataclass
class RestMethod:
    name: str
    http_method: str
    path: str
    body_param: str = ""
    out_param: str = ""
    response_status_code: int = 0
    parameters: list[RestParameter] = field(default_factory=list)


@dataclass
class RestInterface:
    qualified_name: str
    host: str = "localhost"
    base_path: str = ""
    methods: dict[str, RestMethod] = field(default_factory=dict)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    text = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    return text


def find_matching_brace(text: str, open_index: int) -> int:
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise ValueError("unmatched IDL brace")


def split_top_level(text: str, delimiter: str) -> list[str]:
    parts: list[str] = []
    start = 0
    angle_depth = 0
    paren_depth = 0
    bracket_depth = 0
    for index, char in enumerate(text):
        if char == "<":
            angle_depth += 1
        elif char == ">" and angle_depth:
            angle_depth -= 1
        elif char == "(":
            paren_depth += 1
        elif char == ")" and paren_depth:
            paren_depth -= 1
        elif char == "[":
            bracket_depth += 1
        elif char == "]" and bracket_depth:
            bracket_depth -= 1
        elif char == delimiter and angle_depth == 0 and paren_depth == 0 and bracket_depth == 0:
            parts.append(text[start:index])
            start = index + 1
    parts.append(text[start:])
    return parts


def collapse_ws(value: str) -> str:
    return re.sub(r"\s+", " ", value).strip()


def qualified(namespace: tuple[str, ...], name: str) -> str:
    return "::".join((*namespace, name)) if namespace else name


def register_type(model: Model, qualified_name: str) -> None:
    model.unqualified_types.setdefault(qualified_name.rsplit("::", 1)[-1], qualified_name)


def parse_field(declaration: str) -> Field | None:
    declaration = collapse_ws(re.sub(r"\[[^\]]+\]", "", declaration))
    if not declaration or "(" in declaration:
        return None
    declaration = re.sub(r"\s*=.*$", "", declaration).strip()
    match = re.match(r"(?P<type>.+?)(?:\s+|[&*])(?P<name>[A-Za-z_]\w*)$", declaration)
    if not match:
        return None
    type_name = collapse_ws(match.group("type"))
    name = match.group("name")
    required = not is_optional_type(type_name)
    return Field(name=name, type_name=type_name, required=required)


def parse_parameter(declaration: str) -> Parameter | None:
    attrs = re.findall(r"\[([^\]]+)\]", declaration)
    direction = "in"
    for attr in attrs:
        if "out" in attr:
            direction = "out"
            break
    declaration = collapse_ws(re.sub(r"\[[^\]]+\]", "", declaration))
    declaration = re.sub(r"\s*=.*$", "", declaration).strip()
    if not declaration:
        return None
    match = re.match(r"(?P<type>.+?)(?:\s+|[&*])(?P<name>[A-Za-z_]\w*)$", declaration)
    if not match:
        return None
    return Parameter(
        name=match.group("name"),
        type_name=collapse_ws(match.group("type")),
        direction=direction,
    )


def parse_methods(body: str) -> dict[str, Method]:
    methods: dict[str, Method] = {}
    for declaration in split_top_level(body, ";"):
        declaration = collapse_ws(declaration)
        if not declaration or "(" not in declaration:
            continue
        match = re.match(r"(?P<ret>.+?)\s+(?P<name>[A-Za-z_]\w*)\s*\((?P<params>.*)\)\s*(?:const)?$", declaration)
        if not match:
            continue
        params = [
            parsed
            for parsed in (parse_parameter(item) for item in split_top_level(match.group("params"), ","))
            if parsed is not None
        ]
        methods[match.group("name")] = Method(
            name=match.group("name"),
            return_type=collapse_ws(match.group("ret")),
            parameters=params,
        )
    return methods


def parse_enum_values(body: str) -> list[str]:
    values: list[str] = []
    for item in split_top_level(body.replace(";", ","), ","):
        item = re.sub(r"=.*$", "", item).strip()
        if item:
            values.append(item)
    return values


def parse_scope(text: str, namespace: tuple[str, ...], model: Model) -> None:
    index = 0
    keyword_pattern = re.compile(r"\b(namespace|struct|interface|enum|typedef)\b")
    while True:
        match = keyword_pattern.search(text, index)
        if not match:
            break
        keyword = match.group(1)
        if keyword == "namespace":
            name_match = re.match(r"\s+([A-Za-z_]\w*)\s*\{", text[match.end() :])
            if not name_match:
                index = match.end()
                continue
            name = name_match.group(1)
            open_index = match.end() + name_match.end() - 1
            close_index = find_matching_brace(text, open_index)
            parse_scope(text[open_index + 1 : close_index], (*namespace, name), model)
            index = close_index + 1
        elif keyword in {"struct", "interface", "enum"}:
            name_match = re.match(r"\s+([A-Za-z_]\w*)\s*\{", text[match.end() :])
            if not name_match:
                index = match.end()
                continue
            name = name_match.group(1)
            open_index = match.end() + name_match.end() - 1
            close_index = find_matching_brace(text, open_index)
            body = text[open_index + 1 : close_index]
            qualified_name = qualified(namespace, name)
            if keyword == "struct":
                struct = Struct(name=name, qualified_name=qualified_name, namespace=namespace)
                struct.fields = [field for field in (parse_field(item) for item in split_top_level(body, ";")) if field]
                model.structs[qualified_name] = struct
                register_type(model, qualified_name)
            elif keyword == "enum":
                enum = Enum(name=name, qualified_name=qualified_name, values=parse_enum_values(body))
                model.enums[qualified_name] = enum
                register_type(model, qualified_name)
            else:
                interface = Interface(
                    name=name,
                    qualified_name=qualified_name,
                    namespace=namespace,
                    methods=parse_methods(body),
                )
                model.interfaces[qualified_name] = interface
            index = close_index + 1
        else:
            semicolon = text.find(";", match.end())
            if semicolon == -1:
                break
            declaration = text[match.end() : semicolon].strip()
            typedef_match = re.match(r"(.+?)\s+([A-Za-z_]\w*)$", collapse_ws(declaration))
            if typedef_match:
                alias = typedef_match.group(2)
                target = typedef_match.group(1)
                model.typedefs[alias] = target
                model.typedefs[qualified(namespace, alias)] = target
            index = semicolon + 1


def parse_idl(path: Path) -> Model:
    model = Model()
    parse_scope(strip_comments(path.read_text(encoding="utf-8")), (), model)
    return model


def read_tab_rest_metadata(path: Path) -> dict[str, RestInterface]:
    interfaces: dict[str, RestInterface] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        kind = fields[0]
        if kind == "interface":
            if len(fields) != 4:
                raise ValueError(f"invalid interface metadata line: {raw_line}")
            interfaces[fields[1]] = RestInterface(fields[1], fields[2], fields[3])
        elif kind == "method":
            if len(fields) < 5 or len(fields) > 7:
                raise ValueError(f"invalid method metadata line: {raw_line}")
            interface = interfaces.setdefault(fields[1], RestInterface(fields[1]))
            interface.methods[fields[2]] = RestMethod(
                name=fields[2],
                http_method=fields[3].lower(),
                path=fields[4] or "/",
                body_param=fields[5] if len(fields) > 5 else "",
                out_param=fields[6] if len(fields) > 6 else "",
            )
        elif kind == "param":
            if len(fields) != 7:
                raise ValueError(f"invalid parameter metadata line: {raw_line}")
            interface = interfaces.setdefault(fields[1], RestInterface(fields[1]))
            method = interface.methods.setdefault(fields[2], RestMethod(fields[2], "get", "/"))
            method.parameters.append(
                RestParameter(
                    name=fields[3],
                    location=fields[4],
                    wire_name=fields[5],
                    required=fields[6] == "true",
                )
            )
        else:
            raise ValueError(f"unknown REST metadata line: {raw_line}")
    return interfaces


def read_json_rest_binding(path: Path) -> dict[str, RestInterface]:
    value = json.loads(path.read_text(encoding="utf-8"))
    interfaces_value = value.get("interfaces")
    if not isinstance(interfaces_value, list):
        raise ValueError(f"REST binding has no interfaces array: {path}")

    interfaces: dict[str, RestInterface] = {}
    for interface_value in interfaces_value:
        qualified_name = interface_value.get("qualified_name", "")
        if not qualified_name:
            raise ValueError(f"REST binding interface missing qualified_name: {path}")

        interface = RestInterface(
            qualified_name,
            interface_value.get("host", "localhost"),
            interface_value.get("base_path", ""),
        )
        for method_value in interface_value.get("methods", []):
            request_value = method_value.get("request") or {}
            response_value = method_value.get("response") or {}
            method = RestMethod(
                name=method_value.get("name", ""),
                http_method=str(method_value.get("http_method", "get")).lower(),
                path=method_value.get("path", "") or "/",
                body_param=request_value.get("body_param", ""),
                out_param=response_value.get("out_param", ""),
                response_status_code=int(response_value.get("status_code", 0) or 0),
            )
            if not method.name:
                raise ValueError(f"REST binding method missing name: {path}")
            for parameter_value in method_value.get("parameters", []):
                method.parameters.append(
                    RestParameter(
                        name=parameter_value.get("name", ""),
                        location=parameter_value.get("location", ""),
                        wire_name=parameter_value.get("wire_name", ""),
                        required=bool(parameter_value.get("required", False)),
                    )
                )
            interface.methods[method.name] = method
        interfaces[qualified_name] = interface
    return interfaces


def read_rest_metadata(path: Path) -> dict[str, RestInterface]:
    if path.name.endswith(".json"):
        return read_json_rest_binding(path)
    return read_tab_rest_metadata(path)


def is_optional_type(type_name: str) -> bool:
    type_name = strip_type(type_name)
    return type_name.startswith("rpc::optional<") or type_name.startswith("std::optional<")


def inner_template(type_name: str) -> str:
    start = type_name.find("<")
    end = type_name.rfind(">")
    return type_name[start + 1 : end].strip() if start != -1 and end != -1 and end > start else ""


def split_template_args(value: str) -> list[str]:
    return [item.strip() for item in split_top_level(value, ",") if item.strip()]


def strip_type(type_name: str) -> str:
    type_name = collapse_ws(type_name)
    type_name = re.sub(r"^const\s+", "", type_name)
    type_name = type_name.removeprefix("::")
    while type_name.endswith("&") or type_name.endswith("*"):
        type_name = type_name[:-1].strip()
    return type_name


def resolve_alias(type_name: str, model: Model) -> str:
    type_name = strip_type(type_name)
    seen: set[str] = set()
    while type_name in model.typedefs and type_name not in seen:
        seen.add(type_name)
        type_name = strip_type(model.typedefs[type_name])
    return type_name


def component_name(qualified_name: str) -> str:
    return re.sub(r"[^0-9A-Za-z_]+", "_", qualified_name).strip("_")


def resolve_idl_type(type_name: str, model: Model, namespace: tuple[str, ...] = ()) -> str | None:
    type_name = resolve_alias(type_name, model)
    if type_name in model.structs or type_name in model.enums:
        return type_name
    for length in range(len(namespace), -1, -1):
        candidate = qualified(namespace[:length], type_name)
        if candidate in model.structs or candidate in model.enums:
            return candidate
    return model.unqualified_types.get(type_name)


def schema_for_type(type_name: str, model: Model, namespace: tuple[str, ...] = ()) -> dict[str, Any]:
    type_name = resolve_alias(type_name, model)
    if is_optional_type(type_name):
        return schema_for_type(inner_template(type_name), model, namespace)

    if type_name in {"std::string", "string", "char*", "const char*"}:
        return {"type": "string"}
    if type_name == "bool":
        return {"type": "boolean"}
    if type_name in {"float", "double"}:
        return {"type": "number", "format": "double" if type_name == "double" else "float"}
    if type_name in {"int8_t", "int16_t", "int32_t", "uint8_t", "uint16_t", "uint32_t", "int", "unsigned int"}:
        return {"type": "integer", "format": "int32"}
    if type_name in {"int64_t", "uint64_t", "size_t", "long", "unsigned long"}:
        return {"type": "integer", "format": "int64"}
    if type_name == "json::v1::object":
        return {"type": "object", "additionalProperties": True}

    for prefix in ("std::vector<", "std::list<", "std::set<", "std::unordered_set<"):
        if type_name.startswith(prefix):
            return {"type": "array", "items": schema_for_type(inner_template(type_name), model, namespace)}

    for prefix in ("std::map<", "std::unordered_map<"):
        if type_name.startswith(prefix):
            args = split_template_args(inner_template(type_name))
            value_type = args[1] if len(args) == 2 else "json::v1::object"
            return {"type": "object", "additionalProperties": schema_for_type(value_type, model, namespace)}

    resolved = resolve_idl_type(type_name, model, namespace)
    if resolved in model.structs or resolved in model.enums:
        return {"$ref": f"#/components/schemas/{component_name(resolved)}"}

    return {"type": "object", "additionalProperties": True, "x-canopy-type": type_name}


def build_components(model: Model) -> dict[str, Any]:
    schemas: dict[str, Any] = {}
    for qualified_name, enum in sorted(model.enums.items()):
        schemas[component_name(qualified_name)] = {"type": "string", "enum": enum.values}
    for qualified_name, struct in sorted(model.structs.items()):
        properties: dict[str, Any] = {}
        required: list[str] = []
        for item in struct.fields:
            properties[item.name] = schema_for_type(item.type_name, model, struct.namespace)
            if item.required:
                required.append(item.name)
        schema: dict[str, Any] = {"type": "object", "properties": properties}
        if required:
            schema["required"] = required
        schemas[component_name(qualified_name)] = schema
    return {"schemas": schemas}


def method_parameter(method: Method | None, name: str) -> Parameter | None:
    if not method:
        return None
    for parameter in method.parameters:
        if parameter.name == name:
            return parameter
    return None


def server_url(scheme: str, host: str, base_path: str) -> str:
    if "://" in host:
        url = host.rstrip("/")
    else:
        url = f"{scheme}://{host or 'localhost'}"
    if base_path:
        url += "/" + base_path.strip("/")
    return url


def generate_openapi(
    model: Model,
    metadata: dict[str, RestInterface],
    title: str,
    version: str,
    scheme: str,
) -> dict[str, Any]:
    paths: dict[str, Any] = {}
    servers: list[dict[str, str]] = []
    seen_servers: set[str] = set()

    for rest_interface in metadata.values():
        url = server_url(scheme, rest_interface.host, rest_interface.base_path)
        if url not in seen_servers:
            servers.append({"url": url})
            seen_servers.add(url)

        interface = model.interfaces.get(rest_interface.qualified_name)
        tag = rest_interface.qualified_name.rsplit("::", 1)[-1]
        namespace = interface.namespace if interface else ()

        for rest_method in rest_interface.methods.values():
            idl_method = interface.methods.get(rest_method.name) if interface else None
            operation: dict[str, Any] = {
                "operationId": rest_method.name,
                "tags": [tag],
                "responses": {},
            }

            parameters: list[dict[str, Any]] = []
            for rest_parameter in rest_method.parameters:
                if rest_parameter.location not in {"path", "query"}:
                    continue
                idl_parameter = method_parameter(idl_method, rest_parameter.name)
                parameters.append(
                    {
                        "name": rest_parameter.wire_name,
                        "in": rest_parameter.location,
                        "required": True if rest_parameter.location == "path" else rest_parameter.required,
                        "schema": schema_for_type(
                            idl_parameter.type_name if idl_parameter else "json::v1::object",
                            model,
                            namespace,
                        ),
                    }
                )
            if parameters:
                operation["parameters"] = parameters

            if rest_method.body_param:
                idl_parameter = method_parameter(idl_method, rest_method.body_param)
                operation["requestBody"] = {
                    "required": True,
                    "content": {
                        "application/json": {
                            "schema": schema_for_type(
                                idl_parameter.type_name if idl_parameter else "json::v1::object",
                                model,
                                namespace,
                            )
                        }
                    },
                }

            response_status = str(rest_method.response_status_code or (200 if rest_method.out_param else 204))
            if rest_method.out_param:
                idl_parameter = method_parameter(idl_method, rest_method.out_param)
                operation["responses"][response_status] = {
                    "description": "Successful response",
                    "content": {
                        "application/json": {
                            "schema": schema_for_type(
                                idl_parameter.type_name if idl_parameter else "json::v1::object",
                                model,
                                namespace,
                            )
                        }
                    },
                }
            else:
                operation["responses"][response_status] = {"description": "No content"}

            path = rest_method.path if rest_method.path.startswith("/") else f"/{rest_method.path}"
            paths.setdefault(path, {})[rest_method.http_method.lower()] = operation

    return {
        "openapi": "3.0.3",
        "info": {"title": title, "version": version},
        "servers": servers or [{"url": f"{scheme}://localhost"}],
        "paths": paths,
        "components": build_components(model),
        "x-canopy-rest-metadata": "generated from Canopy IDL and REST binding metadata",
    }


def write_if_different(path: Path, content: str) -> None:
    old = None
    try:
        old = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        pass
    if old == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idl", required=True, type=Path, help="Canopy IDL file")
    parser.add_argument("--binding", required=True, type=Path, help="Canopy REST binding file")
    parser.add_argument("--output", required=True, type=Path, help="OpenAPI JSON output path")
    parser.add_argument("--title", default=None, help="OpenAPI info.title")
    parser.add_argument("--version", default="1.0.0", help="OpenAPI info.version")
    parser.add_argument("--scheme", default="https", help="Default server scheme when metadata host has no scheme")
    args = parser.parse_args(argv)

    try:
        model = parse_idl(args.idl)
        metadata = read_rest_metadata(args.binding)
        title = args.title or args.idl.stem
        document = generate_openapi(model, metadata, title, args.version, args.scheme)
        write_if_different(args.output, json.dumps(document, indent=2, sort_keys=True) + "\n")
    except Exception as exc:  # noqa: BLE001 - command line diagnostic
        print(f"canopy_rest_to_openapi.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
