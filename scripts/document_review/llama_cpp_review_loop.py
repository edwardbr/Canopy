#!/usr/bin/env python3
"""
Iterative repo review loop for a local llama.cpp OpenAI-compatible server.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import textwrap
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_EXTENSIONS = [
    ".md",
    ".txt",
    ".idl",
    ".h",
    ".hpp",
    ".hh",
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
]

DEFAULT_PATHS = [
    "README.md",
    "documents",
    "rpc/include",
]

DEFAULT_EXCLUDES = [
    "build/**",
    "build_*/**",
    ".git/**",
    "submodules/**",
]

REVIEW_SYSTEM_PROMPT = """\
You are reviewing a C++ codebase for factual accuracy, stale documentation, and user-facing correctness.

Rules:
- Only report issues supported by the provided code, comments, docs, or check output.
- Prefer high-confidence findings over broad speculation.
- Distinguish between documentation mismatch, misleading naming, likely bug, and uncertainty.
- If an example or doc does not match the code, report the exact file and line.
- Keep evidence concrete and short.
- Do not wrap JSON in markdown fences.
- Do not add prose before or after the JSON object.
- If there are no findings, return an empty findings array.
- Output JSON only.

Return exactly:
{
  "findings": [
    {
      "kind": "doc_mismatch|naming|bug|uncertain",
      "severity": "high|medium|low",
      "file": "relative/path",
      "line": 1,
      "summary": "short summary",
      "evidence": "specific evidence from the provided material",
      "suggested_fix": "short actionable fix",
      "confidence": 0.0
    }
  ],
  "overall_status": "clean|issues_found",
  "notes": "optional short note"
}
"""

STRICT_FINDINGS_SYSTEM_PROMPT = """\
You are reviewing a C++ codebase for factual accuracy, stale documentation, and user-facing correctness.

Rules:
- Only report issues supported by the provided code, comments, docs, or check output.
- Prefer high-confidence findings over broad speculation.
- Report only actionable findings.
- Do not wrap JSON in markdown fences.
- Do not add prose before or after the JSON object.
- If there are no findings, return {"findings":[]}.

Return exactly:
{
  "findings": [
    {
      "kind": "doc_mismatch|naming|bug|uncertain",
      "severity": "high|medium|low",
      "file": "relative/path",
      "line": 1,
      "summary": "short summary",
      "evidence": "specific evidence from the provided material",
      "suggested_fix": "short actionable fix",
      "confidence": 0.0
    }
  ]
}
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Review a repo iteratively using a local llama.cpp OpenAI-compatible server."
    )
    parser.add_argument(
        "paths",
        nargs="*",
        default=DEFAULT_PATHS,
        help="Repo-relative files or directories to review.",
    )
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[2]),
        help="Repository root. Defaults to the parent of scripts/.",
    )
    parser.add_argument(
        "--endpoint",
        default="http://127.0.0.1:8081/v1/chat/completions",
        help="llama-server OpenAI-compatible chat completions endpoint.",
    )
    parser.add_argument(
        "--model",
        default="local-model",
        help="Model name sent to the OpenAI-compatible endpoint.",
    )
    parser.add_argument(
        "--max-rounds",
        type=int,
        default=5,
        help="Maximum review rounds.",
    )
    parser.add_argument(
        "--chunk-chars",
        type=int,
        default=28000,
        help="Approximate characters per review chunk.",
    )
    parser.add_argument(
        "--temperature",
        type=float,
        default=0.0,
        help="Sampling temperature for the reviewer.",
    )
    parser.add_argument(
        "--extensions",
        nargs="*",
        default=DEFAULT_EXTENSIONS,
        help="File extensions to include.",
    )
    parser.add_argument(
        "--exclude",
        nargs="*",
        default=DEFAULT_EXCLUDES,
        help="rg-style glob exclusions.",
    )
    parser.add_argument(
        "--focus",
        default=(
            "Find factual inaccuracies, stale or misleading comments/docs, examples that do not match the current API, "
            "and user-facing correctness issues."
        ),
        help="Primary review focus.",
    )
    parser.add_argument(
        "--top-p",
        type=float,
        default=0.9,
        help="Sampling top-p value sent to the reviewer.",
    )
    parser.add_argument(
        "--top-k",
        type=int,
        default=20,
        help="Sampling top-k value sent to the reviewer.",
    )
    parser.add_argument(
        "--repeat-penalty",
        type=float,
        default=1.05,
        help="Repeat penalty sent to the reviewer.",
    )
    parser.add_argument(
        "--check-command",
        default="",
        help=(
            "Optional shell command run after each round. Placeholders: "
            "{repo_root} {run_dir} {round} {report_json}"
        ),
    )
    parser.add_argument(
        "--fix-command",
        default="",
        help=(
            "Optional shell command run after each non-clean round before the next iteration. Placeholders: "
            "{repo_root} {run_dir} {round} {report_json}"
        ),
    )
    parser.add_argument(
        "--sleep-seconds",
        type=float,
        default=0.0,
        help="Optional delay between rounds.",
    )
    parser.add_argument(
        "--request-timeout",
        type=int,
        default=1800,
        help="HTTP timeout in seconds for each request to llama-server. Use 0 for no timeout.",
    )
    parser.add_argument(
        "--progress-interval",
        type=int,
        default=30,
        help="Print an in-flight progress message every N seconds while waiting on llama-server. Use 0 to disable.",
    )
    parser.add_argument(
        "--strict-findings-only",
        action="store_true",
        help="Use a stricter JSON schema with only a findings array for easier downstream automation.",
    )
    parser.add_argument(
        "--cache-prompt",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Request llama-server KV cache reuse for repeated prompt prefixes.",
    )
    parser.add_argument(
        "--slot-id",
        type=int,
        default=None,
        help="Optional llama-server slot to pin requests to for better KV reuse. Omit to let the server choose.",
    )
    parser.add_argument(
        "--prewarm-slot",
        action="store_true",
        help="Prewarm the shared round prompt prefix into the selected llama-server slot before processing chunks.",
    )
    parser.add_argument(
        "--auto-retry-splits",
        type=int,
        default=2,
        help="When a chunk returns empty or malformed output, recursively split and retry up to this many levels.",
    )
    return parser.parse_args()


def run_command(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, text=True, capture_output=True, check=False)


def glob_to_regex(pattern: str) -> re.Pattern[str]:
    regex = re.escape(pattern)
    regex = regex.replace(r"\*\*", ".*")
    regex = regex.replace(r"\*", "[^/]*")
    regex = regex.replace(r"\?", ".")
    return re.compile(f"^{regex}$")


def should_exclude(rel_path: str, exclude_patterns: list[str]) -> bool:
    normalized = rel_path.replace(os.sep, "/")
    for pattern in exclude_patterns:
        if glob_to_regex(pattern).match(normalized):
            return True
    return False


def list_files_with_python(repo_root: Path, targets: list[str], extensions: list[str], excludes: list[str]) -> list[Path]:
    extension_set = {ext if ext.startswith(".") else f".{ext}" for ext in extensions}
    selected: set[Path] = set()

    for target in targets:
        target_path = (repo_root / target).resolve()
        if not target_path.exists():
            continue
        if target_path.is_file():
            rel = str(target_path.relative_to(repo_root)).replace(os.sep, "/")
            if should_exclude(rel, excludes):
                continue
            if extension_set and target_path.suffix not in extension_set:
                continue
            selected.add(target_path)
            continue

        for path in target_path.rglob("*"):
            if not path.is_file():
                continue
            rel = str(path.relative_to(repo_root)).replace(os.sep, "/")
            if should_exclude(rel, excludes):
                continue
            if extension_set and path.suffix not in extension_set:
                continue
            selected.add(path)

    return sorted(selected)


def run_shell(command_template: str, repo_root: Path, run_dir: Path, round_index: int, report_json: Path) -> dict[str, Any]:
    command = command_template.format(
        repo_root=shlex.quote(str(repo_root)),
        run_dir=shlex.quote(str(run_dir)),
        round=round_index,
        report_json=shlex.quote(str(report_json)),
    )
    result = subprocess.run(
        ["/bin/bash", "-lc", command],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=False,
    )
    return {
        "command": command,
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def list_files(repo_root: Path, targets: list[str], extensions: list[str], excludes: list[str]) -> list[Path]:
    if shutil.which("rg") is None:
        return list_files_with_python(repo_root, targets, extensions, excludes)

    rg_cmd = ["rg", "--files"]
    for pattern in excludes:
        rg_cmd.extend(["-g", f"!{pattern}"])
    for target in targets:
        rg_cmd.append(target)

    result = run_command(rg_cmd, repo_root)
    if result.returncode != 0:
        raise RuntimeError(f"rg --files failed:\n{result.stderr}")

    selected: list[Path] = []
    extension_set = {ext if ext.startswith(".") else f".{ext}" for ext in extensions}
    for line in result.stdout.splitlines():
        rel = line.strip()
        if not rel:
            continue
        path = repo_root / rel
        if not path.is_file():
            continue
        if extension_set and path.suffix not in extension_set:
            continue
        selected.append(path)
    return sorted(selected)


def read_file(repo_root: Path, path: Path) -> str:
    rel = path.relative_to(repo_root)
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        content = path.read_text(encoding="utf-8", errors="replace")
    return f"FILE: {rel}\n```\n{content}\n```\n"


def chunk_files(repo_root: Path, files: list[Path], chunk_chars: int) -> list[str]:
    chunks: list[str] = []
    current: list[str] = []
    current_size = 0

    for path in files:
        payload = read_file(repo_root, path)
        payload_size = len(payload)
        if current and current_size + payload_size > chunk_chars:
            chunks.append("\n".join(current))
            current = []
            current_size = 0
        if payload_size > chunk_chars and current:
            chunks.append("\n".join(current))
            current = []
            current_size = 0
        if payload_size > chunk_chars:
            chunks.append(payload)
            continue
        current.append(payload)
        current_size += payload_size

    if current:
        chunks.append("\n".join(current))
    return chunks


def extract_json_object(raw_text: str) -> dict[str, Any]:
    raw_text = raw_text.strip()
    if not raw_text:
        return {"findings": [], "overall_status": "issues_found", "notes": "Model returned empty content."}
    try:
        return json.loads(raw_text)
    except json.JSONDecodeError:
        match = re.search(r"\{.*\}", raw_text, re.DOTALL)
        if match:
            try:
                return json.loads(match.group(0))
            except json.JSONDecodeError:
                pass

    snippet = raw_text[:1200]
    return {
        "findings": [
            {
                "kind": "uncertain",
                "severity": "low",
                "file": "__llm_response__",
                "line": 1,
                "summary": "Model response was not valid JSON.",
                "evidence": snippet,
                "suggested_fix": "Retry with a smaller chunk size or adjust the prompt/settings for stricter JSON output.",
                "confidence": 1.0,
            }
        ],
        "overall_status": "issues_found",
        "notes": "Model returned non-JSON content.",
    }


def parsed_content_needs_retry(parsed_content: dict[str, Any]) -> bool:
    findings = parsed_content.get("findings", [])
    if not isinstance(findings, list) or len(findings) != 1:
        return False
    item = findings[0]
    return (
        item.get("file") == "__llm_response__"
        and item.get("summary") in {"Model response was not valid JSON."}
    ) or parsed_content.get("notes") == "Model returned empty content."


def split_chunk_text(chunk_text: str) -> list[str]:
    marker = "\nFILE: "
    parts = chunk_text.split(marker)
    if len(parts) <= 1:
        midpoint = len(chunk_text) // 2
        return [chunk_text[:midpoint], chunk_text[midpoint:]] if midpoint > 0 else [chunk_text]

    rebuilt: list[str] = []
    for index, part in enumerate(parts):
        if index == 0:
            rebuilt.append(part)
        else:
            rebuilt.append("FILE: " + part)

    if len(rebuilt) < 2:
        midpoint = len(chunk_text) // 2
        return [chunk_text[:midpoint], chunk_text[midpoint:]] if midpoint > 0 else [chunk_text]

    split_at = max(1, len(rebuilt) // 2)
    first = "\n".join(rebuilt[:split_at]).strip()
    second = "\n".join(rebuilt[split_at:]).strip()
    return [item for item in [first, second] if item]


def call_llama(
    endpoint: str,
    model: str,
    temperature: float,
    top_p: float,
    top_k: int,
    repeat_penalty: float,
    request_timeout: int,
    progress_interval: int,
    cache_prompt: bool,
    slot_id: int | None,
    system_prompt: str,
    user_prompt: str,
    max_tokens: int | None = None,
) -> dict[str, Any]:
    payload = {
        "model": model,
        "temperature": temperature,
        "top_p": top_p,
        "top_k": top_k,
        "repeat_penalty": repeat_penalty,
        "response_format": {"type": "json_object"},
        "cache_prompt": cache_prompt,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
    }
    if slot_id is not None:
        payload["id_slot"] = slot_id
    if max_tokens is not None:
        payload["max_tokens"] = max_tokens
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        endpoint,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    stop_event = threading.Event()
    progress_thread = None

    if progress_interval > 0:
        started = time.monotonic()

        def report_progress() -> None:
            while not stop_event.wait(progress_interval):
                elapsed = int(time.monotonic() - started)
                print(f"[llama_cpp_review_loop] waiting for response... {elapsed}s elapsed", file=sys.stderr)

        progress_thread = threading.Thread(target=report_progress, daemon=True)
        progress_thread.start()

    try:
        timeout = None if request_timeout == 0 else request_timeout
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        details = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP error from llama server: {exc.code}\n{details}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Failed to reach llama server at {endpoint}: {exc}") from exc
    finally:
        stop_event.set()
        if progress_thread is not None:
            progress_thread.join(timeout=1)

    parsed = json.loads(body)
    message = parsed["choices"][0]["message"]["content"]
    return {
        "raw_response": parsed,
        "content": message,
        "parsed_content": extract_json_object(message),
    }


def normalize_finding(item: dict[str, Any]) -> dict[str, Any]:
    line_value = item.get("line", 1)
    try:
        line_number = int(line_value)
    except (TypeError, ValueError):
        line_number = 1

    confidence_value = item.get("confidence", 0.5)
    try:
        confidence = float(confidence_value)
    except (TypeError, ValueError):
        confidence = 0.5

    return {
        "kind": str(item.get("kind", "uncertain")),
        "severity": str(item.get("severity", "medium")),
        "file": str(item.get("file", "")).strip(),
        "line": max(line_number, 1),
        "summary": str(item.get("summary", "")).strip(),
        "evidence": str(item.get("evidence", "")).strip(),
        "suggested_fix": str(item.get("suggested_fix", "")).strip(),
        "confidence": max(0.0, min(confidence, 1.0)),
    }


def finding_key(item: dict[str, Any]) -> tuple[Any, ...]:
    return (
        item["kind"],
        item["file"],
        item["line"],
        item["summary"],
        item["suggested_fix"],
    )


def dedupe_findings(items: list[dict[str, Any]]) -> list[dict[str, Any]]:
    seen: set[tuple[Any, ...]] = set()
    output: list[dict[str, Any]] = []
    for item in items:
        normalized = normalize_finding(item)
        if not normalized["file"] or not normalized["summary"]:
            continue
        key = finding_key(normalized)
        if key in seen:
            continue
        seen.add(key)
        output.append(normalized)

    severity_rank = {"high": 0, "medium": 1, "low": 2}
    output.sort(key=lambda entry: (severity_rank.get(entry["severity"], 3), entry["file"], entry["line"]))
    return output


def aggregate_round_reports(run_dir: Path) -> dict[str, Any]:
    reports = sorted(run_dir.glob("round_*/report.json"))
    round_finding_counts: dict[str, int] = {}
    findings: list[dict[str, Any]] = []

    for report_path in reports:
        report = json.loads(report_path.read_text(encoding="utf-8"))
        round_name = report_path.parent.name
        round_finding_counts[round_name] = int(report.get("finding_count", 0))
        report_findings = report.get("findings", [])
        if isinstance(report_findings, list):
            findings.extend(report_findings)

    deduped = dedupe_findings(findings)
    return {
        "run_dir": str(run_dir),
        "status": "aggregated_all_rounds",
        "notes": [
            "This file aggregates all round-level report.json outputs for the run.",
            "Findings are deduplicated across rounds by kind, file, line, summary, and suggested_fix.",
            "Round reports remain per-round outputs; this file is the cumulative view.",
        ],
        "source_reports": [str(path) for path in reports],
        "summary": {
            "round_count": len(reports),
            "round_finding_counts": round_finding_counts,
            "aggregated_unique_finding_count": len(deduped),
        },
        "findings": deduped,
    }


def write_master_report(run_dir: Path) -> None:
    master_report = aggregate_round_reports(run_dir)
    save_json(run_dir / "all_rounds_result.json", master_report)


def summarise_findings(items: list[dict[str, Any]], limit: int = 25) -> str:
    lines = []
    for item in items[:limit]:
        lines.append(
            f"- {item['severity']} {item['kind']} {item['file']}:{item['line']} :: {item['summary']}"
        )
    if len(items) > limit:
        lines.append(f"- ... {len(items) - limit} more")
    return "\n".join(lines) if lines else "No prior findings."


def truncate_text(value: str, limit: int = 12000) -> str:
    if len(value) <= limit:
        return value
    return value[:limit] + "\n...[truncated]..."


def build_user_prompt(
    repo_root: Path,
    focus: str,
    round_index: int,
    chunk_index: int,
    chunk_count: int,
    chunk_text: str,
    prior_findings: list[dict[str, Any]],
    latest_check: dict[str, Any] | None,
) -> str:
    prefix = build_prompt_prefix(
        repo_root=repo_root,
        focus=focus,
        prior_findings=prior_findings,
        latest_check=latest_check,
    )
    suffix = build_prompt_suffix(
        round_index=round_index,
        chunk_index=chunk_index,
        chunk_count=chunk_count,
        chunk_text=chunk_text,
    )
    return prefix + "\n\n" + suffix


def build_prompt_prefix(
    repo_root: Path,
    focus: str,
    prior_findings: list[dict[str, Any]],
    latest_check: dict[str, Any] | None,
) -> str:
    check_block = "No check output was provided for this round."
    if latest_check:
        check_block = textwrap.dedent(
            f"""\
            Command: {latest_check.get("command", "")}
            Return code: {latest_check.get("returncode", "")}
            STDOUT:
            {truncate_text(latest_check.get("stdout", ""))}

            STDERR:
            {truncate_text(latest_check.get("stderr", ""))}
            """
        )

    return textwrap.dedent(
        f"""\
        Repo root: {repo_root}
        Review focus: {focus}

        Prior findings summary:
        {summarise_findings(prior_findings)}

        Latest check output:
        {check_block}

        Review the following repo material and return JSON only.
        Prefer findings that a maintainer could act on immediately.
        """
    ).strip()

def build_prompt_suffix(
    round_index: int,
    chunk_index: int,
    chunk_count: int,
    chunk_text: str,
) -> str:
    return textwrap.dedent(
        f"""\
        Round: {round_index}
        Chunk: {chunk_index} of {chunk_count}

        {chunk_text}
        """
    ).strip()


def save_json(path: Path, payload: Any) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=False) + "\n", encoding="utf-8")


def process_chunk(
    *,
    args: argparse.Namespace,
    repo_root: Path,
    round_dir: Path,
    system_prompt: str,
    focus: str,
    round_index: int,
    chunk_label: str,
    chunk_text: str,
    prior_findings: list[dict[str, Any]],
    latest_check: dict[str, Any] | None,
    retries_remaining: int,
) -> list[dict[str, Any]]:
    user_prompt = build_user_prompt(
        repo_root=repo_root,
        focus=focus,
        round_index=round_index,
        chunk_index=1,
        chunk_count=1,
        chunk_text=chunk_text,
        prior_findings=prior_findings,
        latest_check=latest_check,
    )
    print(
        f"Round {round_index} chunk {chunk_label}: sending request to {args.endpoint}",
        file=sys.stderr,
    )
    response = call_llama(
        endpoint=args.endpoint,
        model=args.model,
        temperature=args.temperature,
        top_p=args.top_p,
        top_k=args.top_k,
        repeat_penalty=args.repeat_penalty,
        request_timeout=args.request_timeout,
        progress_interval=args.progress_interval,
        cache_prompt=args.cache_prompt,
        slot_id=args.slot_id,
        system_prompt=system_prompt,
        user_prompt=user_prompt,
    )
    save_json(round_dir / f"chunk_{chunk_label}_raw.json", response["raw_response"])
    (round_dir / f"chunk_{chunk_label}_content.txt").write_text(
        response["content"],
        encoding="utf-8",
    )
    save_json(round_dir / f"chunk_{chunk_label}_parsed.json", response["parsed_content"])

    parsed_content = response["parsed_content"]
    if retries_remaining > 0 and parsed_content_needs_retry(parsed_content):
        subchunks = split_chunk_text(chunk_text)
        if len(subchunks) > 1:
            print(
                f"Round {round_index} chunk {chunk_label}: empty/malformed response, retrying as {len(subchunks)} smaller chunks",
                file=sys.stderr,
            )
            findings: list[dict[str, Any]] = []
            for index, subchunk in enumerate(subchunks, start=1):
                findings.extend(
                    process_chunk(
                        args=args,
                        repo_root=repo_root,
                        round_dir=round_dir,
                        system_prompt=system_prompt,
                        focus=focus,
                        round_index=round_index,
                        chunk_label=f"{chunk_label}_{index}",
                        chunk_text=subchunk,
                        prior_findings=prior_findings,
                        latest_check=latest_check,
                        retries_remaining=retries_remaining - 1,
                    )
                )
            return findings

    chunk_findings = parsed_content.get("findings", [])
    return chunk_findings if isinstance(chunk_findings, list) else []


def prewarm_round_prefix(
    *,
    args: argparse.Namespace,
    repo_root: Path,
    round_dir: Path,
    system_prompt: str,
    focus: str,
    prior_findings: list[dict[str, Any]],
    latest_check: dict[str, Any] | None,
    round_index: int,
    chunk_count: int,
) -> None:
    if not args.prewarm_slot or args.slot_id is None:
        return

    user_prompt = build_prompt_prefix(
        repo_root=repo_root,
        focus=focus,
        prior_findings=prior_findings,
        latest_check=latest_check,
    )
    user_prompt += "\n\n" + build_prompt_suffix(
        round_index=round_index,
        chunk_index=0,
        chunk_count=chunk_count,
        chunk_text=(
            "Warm this review prefix into the current slot. "
            "Return an empty findings array and no additional commentary."
        ),
    )

    print(f"Round {round_index}: prewarming slot {args.slot_id}", file=sys.stderr)
    response = call_llama(
        endpoint=args.endpoint,
        model=args.model,
        temperature=args.temperature,
        top_p=args.top_p,
        top_k=args.top_k,
        repeat_penalty=args.repeat_penalty,
        request_timeout=args.request_timeout,
        progress_interval=args.progress_interval,
        cache_prompt=args.cache_prompt,
        slot_id=args.slot_id,
        system_prompt=system_prompt,
        user_prompt=user_prompt,
        max_tokens=64,
    )
    save_json(round_dir / "prewarm_raw.json", response["raw_response"])
    (round_dir / "prewarm_content.txt").write_text(response["content"], encoding="utf-8")
    save_json(round_dir / "prewarm_parsed.json", response["parsed_content"])


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    if not repo_root.exists():
        print(f"Repo root does not exist: {repo_root}", file=sys.stderr)
        return 2

    timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = repo_root / "scripts" / "llama_review_runs" / timestamp
    run_dir.mkdir(parents=True, exist_ok=True)

    try:
        files = list_files(repo_root, args.paths, args.extensions, args.exclude)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if not files:
        print("No files matched the requested review scope.", file=sys.stderr)
        return 2

    chunks = chunk_files(repo_root, files, args.chunk_chars)
    manifest = {
        "repo_root": str(repo_root),
        "paths": args.paths,
        "files": [str(path.relative_to(repo_root)) for path in files],
        "chunk_count": len(chunks),
        "endpoint": args.endpoint,
        "model": args.model,
        "focus": args.focus,
        "strict_findings_only": args.strict_findings_only,
        "auto_retry_splits": args.auto_retry_splits,
    }
    save_json(run_dir / "manifest.json", manifest)

    prior_findings: list[dict[str, Any]] = []
    previous_finding_keys: set[tuple[Any, ...]] = set()
    latest_check: dict[str, Any] | None = None

    print(f"Run directory: {run_dir}")
    print(f"Reviewing {len(files)} files in {len(chunks)} chunks")

    system_prompt = STRICT_FINDINGS_SYSTEM_PROMPT if args.strict_findings_only else REVIEW_SYSTEM_PROMPT

    for round_index in range(1, args.max_rounds + 1):
        all_findings: list[dict[str, Any]] = []
        round_dir = run_dir / f"round_{round_index:02d}"
        round_dir.mkdir(parents=True, exist_ok=True)

        prewarm_round_prefix(
            args=args,
            repo_root=repo_root,
            round_dir=round_dir,
            system_prompt=system_prompt,
            focus=args.focus,
            prior_findings=prior_findings,
            latest_check=latest_check,
            round_index=round_index,
            chunk_count=len(chunks),
        )

        for chunk_index, chunk_text in enumerate(chunks, start=1):
            chunk_findings = process_chunk(
                args=args,
                repo_root=repo_root,
                round_dir=round_dir,
                system_prompt=system_prompt,
                focus=args.focus,
                round_index=round_index,
                chunk_label=f"{chunk_index:02d}",
                chunk_text=chunk_text,
                prior_findings=prior_findings,
                latest_check=latest_check,
                retries_remaining=args.auto_retry_splits,
            )
            if isinstance(chunk_findings, list):
                all_findings.extend(chunk_findings)

        deduped = dedupe_findings(all_findings)
        report = {
            "round": round_index,
            "overall_status": "clean" if not deduped else "issues_found",
            "finding_count": len(deduped),
            "findings": deduped,
            "latest_check": latest_check,
        }
        report_json = round_dir / "report.json"
        save_json(report_json, report)

        print(f"Round {round_index}: {len(deduped)} findings")
        for item in deduped[:10]:
            print(f"  - {item['severity']} {item['file']}:{item['line']} {item['summary']}")
        if len(deduped) > 10:
            print(f"  - ... {len(deduped) - 10} more")

        current_keys = {finding_key(item) for item in deduped}
        if not deduped:
            write_master_report(run_dir)
            print("Review loop is clean.")
            return 0

        if current_keys == previous_finding_keys:
            write_master_report(run_dir)
            print("Findings repeated unchanged. Stopping to avoid an infinite loop.")
            return 0

        if args.fix_command:
            fix_result = run_shell(args.fix_command, repo_root, run_dir, round_index, report_json)
            save_json(round_dir / "fix_command.json", fix_result)
            print(f"Fix command return code: {fix_result['returncode']}")

        latest_check = None
        if args.check_command:
            latest_check = run_shell(args.check_command, repo_root, run_dir, round_index, report_json)
            save_json(round_dir / "check_command.json", latest_check)
            print(f"Check command return code: {latest_check['returncode']}")

        prior_findings = deduped
        previous_finding_keys = current_keys

        if args.sleep_seconds > 0:
            time.sleep(args.sleep_seconds)

    write_master_report(run_dir)
    print("Reached max rounds.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
