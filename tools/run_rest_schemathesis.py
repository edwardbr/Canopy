#!/usr/bin/env python3
"""Run Schemathesis against generated third-party REST loopback servers."""

from __future__ import annotations

import argparse
import copy
import json
import os
import shlex
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


HTTP_METHODS = ("get", "put", "post", "delete", "patch", "head", "options", "trace")
SUPPORTED_REQUEST_MEDIA_TYPES = {"application/json", "application/*+json", "*/*"}


DEFAULT_CHECKS = ",".join(
    [
        "not_a_server_error",
        "status_code_conformance",
        "content_type_conformance",
        "response_schema_conformance",
        "positive_data_acceptance",
        "unsupported_method",
    ]
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default="third_party_interfaces/rest/roundtrip_manifest.json")
    parser.add_argument("--build-dir", default="build_debug")
    parser.add_argument("--slug", action="append", help="Endpoint slug to test. Repeatable. Defaults to all.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--phases",
        default="examples,fuzzing",
        help="Schemathesis phases to run. Coverage is opt-in because it exercises loopback-server edge cases.",
    )
    parser.add_argument("--mode", default="positive", choices=["positive", "negative", "all"])
    parser.add_argument("--checks", default=DEFAULT_CHECKS)
    parser.add_argument("--max-examples", type=int, default=1)
    parser.add_argument("--workers", default="1")
    parser.add_argument("--request-timeout", type=float, default=5.0)
    parser.add_argument("--server-timeout", type=float, default=10.0)
    parser.add_argument("--report-dir", default="/tmp/canopy_schemathesis_reports")
    parser.add_argument(
        "--validate-security",
        action="store_true",
        help="Keep OpenAPI security requirements. By default the loopback test strips them because auth is user-supplied.",
    )
    parser.add_argument(
        "--include-unsupported-media",
        action="store_true",
        help="Keep operations that require request media not implemented by the generated loopback server.",
    )
    parser.add_argument(
        "--schemathesis",
        default=os.environ.get("SCHEMATHESIS", ""),
        help="Schemathesis command. Defaults to schemathesis, or uvx --from schemathesis schemathesis.",
    )
    return parser.parse_args()


def load_entries(manifest: Path, slugs: list[str] | None) -> list[dict]:
    data = json.loads(manifest.read_text(encoding="utf-8"))
    entries = [entry for entry in data["entries"] if entry.get("status") == "written"]
    if not slugs:
        return entries
    wanted = set(slugs)
    selected = [entry for entry in entries if entry["slug"] in wanted or entry["target"] in wanted]
    missing = wanted - {entry["slug"] for entry in selected} - {entry["target"] for entry in selected}
    if missing:
        raise SystemExit(f"unknown endpoint slug/target: {', '.join(sorted(missing))}")
    return selected


def find_spec_path(entry: dict) -> Path:
    binding_path = Path(entry["rest_binding"])
    binding = json.loads(binding_path.read_text(encoding="utf-8"))
    source = binding.get("source", {}).get("openapi")
    if not source:
        raise SystemExit(f"{entry['slug']}: rest binding does not identify source OpenAPI file")
    spec_path = Path(source)
    if not spec_path.is_absolute():
        spec_path = binding_path.parent / spec_path
    if not spec_path.exists():
        raise SystemExit(f"{entry['slug']}: source OpenAPI file not found: {spec_path}")
    return spec_path


def deep_merge_json(base, overlay):
    if isinstance(base, dict) and isinstance(overlay, dict):
        merged = dict(base)
        for key, value in overlay.items():
            merged[key] = deep_merge_json(merged[key], value) if key in merged else value
        return merged
    return overlay


def resolve_local_ref(spec: dict, ref: str) -> object:
    if not ref.startswith("#/"):
        raise KeyError(ref)
    value: object = spec
    for part in ref[2:].split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        if not isinstance(value, dict):
            raise KeyError(ref)
        value = value[part]
    return value


def security_header_names(spec: dict) -> set[str]:
    names = {"authorization"}
    schemes = {}
    components = spec.get("components")
    if isinstance(components, dict) and isinstance(components.get("securitySchemes"), dict):
        schemes.update(components["securitySchemes"])
    if isinstance(spec.get("securityDefinitions"), dict):
        schemes.update(spec["securityDefinitions"])

    for scheme in schemes.values():
        if not isinstance(scheme, dict):
            continue
        if str(scheme.get("type", "")).lower() in {"http", "oauth2", "openidconnect"}:
            names.add("authorization")
        if str(scheme.get("type", "")).lower() == "apikey" and str(scheme.get("in", "")).lower() == "header":
            header_name = scheme.get("name")
            if isinstance(header_name, str) and header_name:
                names.add(header_name.lower())
    return names


def is_security_parameter(spec: dict, parameter: object, header_names: set[str]) -> bool:
    if isinstance(parameter, dict) and "$ref" in parameter and isinstance(parameter["$ref"], str):
        try:
            parameter = resolve_local_ref(spec, parameter["$ref"])
        except (KeyError, TypeError):
            return False
    if not isinstance(parameter, dict):
        return False
    return str(parameter.get("in", "")).lower() == "header" and str(parameter.get("name", "")).lower() in header_names


def strip_security(spec: dict) -> dict:
    spec = copy.deepcopy(spec)
    header_names = security_header_names(spec)
    spec.pop("security", None)
    spec.pop("securityDefinitions", None)
    components = spec.get("components")
    if isinstance(components, dict):
        components.pop("securitySchemes", None)

    paths = spec.get("paths")
    if isinstance(paths, dict):
        for path_item in paths.values():
            if not isinstance(path_item, dict):
                continue
            parameters = path_item.get("parameters")
            if isinstance(parameters, list):
                path_item["parameters"] = [
                    parameter for parameter in parameters if not is_security_parameter(spec, parameter, header_names)
                ]
            for method in HTTP_METHODS:
                operation = path_item.get(method)
                if not isinstance(operation, dict):
                    continue
                operation.pop("security", None)
                parameters = operation.get("parameters")
                if isinstance(parameters, list):
                    operation["parameters"] = [
                        parameter for parameter in parameters if not is_security_parameter(spec, parameter, header_names)
                    ]
    return spec


def is_supported_request_media_type(media_type: str) -> bool:
    media = media_type.split(";", 1)[0].strip().lower()
    return media in SUPPORTED_REQUEST_MEDIA_TYPES or media.endswith("+json")


def resolve_if_ref(spec: dict, value: object) -> object:
    if isinstance(value, dict) and isinstance(value.get("$ref"), str):
        try:
            return resolve_local_ref(spec, value["$ref"])
        except (KeyError, TypeError):
            return value
    return value


def operation_has_form_data(spec: dict, operation: dict) -> bool:
    for raw_parameter in operation.get("parameters", []):
        parameter = resolve_if_ref(spec, raw_parameter)
        if isinstance(parameter, dict) and parameter.get("in") == "formData":
            return True
    return False


def filter_unsupported_request_media(spec: dict) -> tuple[dict, int]:
    spec = copy.deepcopy(spec)
    removed = 0
    paths = spec.get("paths")
    if not isinstance(paths, dict):
        return spec, removed

    for path, path_item in list(paths.items()):
        if not isinstance(path_item, dict):
            continue
        for method in HTTP_METHODS:
            operation = path_item.get(method)
            if not isinstance(operation, dict):
                continue

            request_body = resolve_if_ref(spec, operation.get("requestBody"))
            content = request_body.get("content") if isinstance(request_body, dict) else None
            if isinstance(content, dict) and content:
                supported_content = {
                    media_type: value
                    for media_type, value in content.items()
                    if isinstance(media_type, str) and is_supported_request_media_type(media_type)
                }
                if not supported_content:
                    path_item.pop(method)
                    removed += 1
                    continue
                if len(supported_content) != len(content):
                    content.clear()
                    content.update(supported_content)

            if operation_has_form_data(spec, operation):
                path_item.pop(method)
                removed += 1
                continue

            consumes = operation.get("consumes") or spec.get("consumes")
            if isinstance(consumes, list) and consumes and not any(
                isinstance(media_type, str) and is_supported_request_media_type(media_type) for media_type in consumes
            ):
                path_item.pop(method)
                removed += 1

        if not any(isinstance(path_item.get(method), dict) for method in HTTP_METHODS):
            path_item.pop("parameters", None)
            if not path_item:
                paths.pop(path)
    return spec, removed


def overlay_paths(entry: dict) -> list[Path]:
    binding_path = Path(entry["rest_binding"])
    binding = json.loads(binding_path.read_text(encoding="utf-8"))
    source = binding.get("source", {})
    overlays = source.get("overlays") or []
    if not isinstance(overlays, list):
        return []

    paths: list[Path] = []
    for overlay in overlays:
        if not isinstance(overlay, str) or not overlay:
            continue
        overlay_path = Path(overlay)
        if not overlay_path.is_absolute():
            overlay_path = binding_path.parent / overlay_path
        if not overlay_path.exists():
            raise SystemExit(f"{entry['slug']}: OpenAPI overlay not found: {overlay_path}")
        paths.append(overlay_path)
    return paths


def effective_spec_path(
    entry: dict,
    report_dir: Path,
    validate_security: bool,
    include_unsupported_media: bool,
) -> Path:
    spec_path = find_spec_path(entry)
    overlays = overlay_paths(entry)
    if not overlays and validate_security and include_unsupported_media:
        return spec_path

    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    for overlay_path in overlays:
        overlay = json.loads(overlay_path.read_text(encoding="utf-8"))
        spec = deep_merge_json(spec, overlay)
    if not validate_security:
        spec = strip_security(spec)
    if not include_unsupported_media:
        spec, removed_operations = filter_unsupported_request_media(spec)
        if removed_operations:
            print(f"[{entry['slug']}] skipped {removed_operations} unsupported request-media operation(s)", flush=True)

    output = report_dir / f"{entry['target']}.effective.openapi.json"
    output.write_text(json.dumps(spec, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return output


def find_base_path(entry: dict) -> str:
    binding_path = Path(entry["rest_binding"])
    binding = json.loads(binding_path.read_text(encoding="utf-8"))
    interfaces = binding.get("interfaces") or []
    if not interfaces:
        return ""
    base_path = interfaces[0].get("base_path") or ""
    if not base_path or base_path == "/":
        return ""
    return "/" + base_path.strip("/")


def schemathesis_command(args: argparse.Namespace) -> list[str]:
    if args.schemathesis:
        return shlex.split(args.schemathesis)
    if shutil.which("schemathesis"):
        return ["schemathesis"]
    if shutil.which("uvx"):
        return ["uvx", "--from", "schemathesis", "schemathesis"]
    raise SystemExit("schemathesis is not installed and uvx is not available")


def reserve_port(host: str) -> int:
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    with socket.socket(family, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def wait_for_port(host: str, port: int, server: subprocess.Popen, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    while time.monotonic() < deadline:
        if server.poll() is not None:
            output = ""
            if server.stdout is not None:
                output = server.stdout.read() or ""
            raise RuntimeError(f"server exited before accepting connections\n{output}")
        try:
            with socket.create_connection((host, port), timeout=0.25):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"server did not accept connections on {host}:{port}")


def stop_server(server: subprocess.Popen) -> None:
    if server.poll() is not None:
        return
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait(timeout=5)


def run_endpoint(args: argparse.Namespace, entry: dict, schemathesis: list[str], report_dir: Path) -> int:
    spec_path = effective_spec_path(entry, report_dir, args.validate_security, args.include_unsupported_media)
    base_path = find_base_path(entry)
    executable = Path(args.build_dir) / "output" / entry["test_target"]
    if not executable.exists():
        raise SystemExit(f"{entry['slug']}: test executable not found: {executable}")

    port = reserve_port(args.host)
    server = subprocess.Popen(
        [str(executable), "--serve", args.host, str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        wait_for_port(args.host, port, server, args.server_timeout)
        endpoint_report = report_dir / f"{entry['target']}.junit.xml"
        base_url = f"http://{args.host}:{port}{base_path}"
        command = [
            *schemathesis,
            "run",
            str(spec_path),
            "--url",
            base_url,
            "--phases",
            args.phases,
            "--mode",
            args.mode,
            "--checks",
            args.checks,
            "--max-examples",
            str(args.max_examples),
            "--workers",
            str(args.workers),
            "--request-timeout",
            str(args.request_timeout),
            "--generation-database",
            ":memory:",
            "--report",
            "junit",
            "--report-junit-path",
            str(endpoint_report),
            "--no-color",
        ]
        env = os.environ.copy()
        env.setdefault("UV_CACHE_DIR", "/tmp/canopy_uv_cache")
        env.setdefault("UV_TOOL_DIR", "/tmp/canopy_uv_tools")
        print(f"[{entry['slug']}] Schemathesis {spec_path} -> {base_url}", flush=True)
        return subprocess.run(command, env=env).returncode
    finally:
        stop_server(server)


def main() -> int:
    args = parse_args()
    manifest = Path(args.manifest).resolve()
    report_dir = Path(args.report_dir).resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    entries = load_entries(manifest, args.slug)
    schemathesis = schemathesis_command(args)

    failures = 0
    for entry in entries:
        result = run_endpoint(args, entry, schemathesis, report_dir)
        if result != 0:
            failures += 1
            print(f"[{entry['slug']}] failed with exit code {result}", file=sys.stderr)
    print(f"schemathesis endpoints={len(entries)} failures={failures} report_dir={report_dir}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
