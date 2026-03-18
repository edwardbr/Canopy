#!/usr/bin/env python3
"""
Generate per-file edit prompts from a llama.cpp review report or fix queue.
"""

from __future__ import annotations

import argparse
import json
import sys
import textwrap
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate per-file edit prompts from a llama.cpp review report or fix queue."
    )
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Repository root. Defaults to the parent of scripts/.",
    )
    parser.add_argument(
        "--input",
        default="",
        help="Path to a review report.json or fix-queue JSON file. If omitted, the latest review report is used.",
    )
    parser.add_argument(
        "--round",
        type=int,
        default=0,
        help="If using the latest run, choose a specific round number. Defaults to the latest round.",
    )
    parser.add_argument(
        "--format",
        choices=["text", "json"],
        default="text",
        help="Output format.",
    )
    parser.add_argument(
        "--min-confidence",
        type=float,
        default=0.0,
        help="Only include findings at or above this confidence.",
    )
    parser.add_argument(
        "--max-items-per-file",
        type=int,
        default=0,
        help="Optional cap per file. Zero means unlimited.",
    )
    parser.add_argument(
        "--output-dir",
        default="",
        help="Optional directory to write one prompt file per source file.",
    )
    parser.add_argument(
        "--include-file-content",
        action="store_true",
        help="Include current file content in each generated prompt.",
    )
    parser.add_argument(
        "--content-char-limit",
        type=int,
        default=16000,
        help="Maximum file content characters to include when --include-file-content is used.",
    )
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def find_latest_run_dir(repo_root: Path) -> Path:
    runs_root = repo_root / "scripts" / "llama_review_runs"
    if not runs_root.exists():
        raise FileNotFoundError(f"No review runs found under {runs_root}")
    run_dirs = [path for path in runs_root.iterdir() if path.is_dir()]
    if not run_dirs:
        raise FileNotFoundError(f"No review runs found under {runs_root}")
    return sorted(run_dirs)[-1]


def find_latest_report(repo_root: Path, round_number: int) -> Path:
    run_dir = find_latest_run_dir(repo_root)
    round_dirs = sorted(path for path in run_dir.iterdir() if path.is_dir() and path.name.startswith("round_"))
    if not round_dirs:
        raise FileNotFoundError(f"No round directories found under {run_dir}")

    if round_number > 0:
        target = run_dir / f"round_{round_number:02d}" / "report.json"
        if not target.exists():
            raise FileNotFoundError(f"Round report does not exist: {target}")
        return target

    for round_dir in reversed(round_dirs):
        candidate = round_dir / "report.json"
        if candidate.exists():
            return candidate

    raise FileNotFoundError(f"No report.json found under {run_dir}")


def normalize_report_findings(payload: dict[str, Any], min_confidence: float) -> list[dict[str, Any]]:
    findings = payload.get("findings", [])
    output: list[dict[str, Any]] = []
    for item in findings:
        confidence = float(item.get("confidence", 0.0))
        if confidence < min_confidence:
            continue
        output.append(
            {
                "kind": str(item.get("kind", "uncertain")),
                "severity": str(item.get("severity", "medium")),
                "file": str(item.get("file", "")),
                "line": int(item.get("line", 1) or 1),
                "summary": str(item.get("summary", "")).strip(),
                "evidence": str(item.get("evidence", "")).strip(),
                "suggested_fix": str(item.get("suggested_fix", "")).strip(),
                "confidence": confidence,
            }
        )
    return output


def normalize_queue_files(payload: dict[str, Any], min_confidence: float) -> list[dict[str, Any]]:
    files = payload.get("files", [])
    output: list[dict[str, Any]] = []
    for file_entry in files:
        file_path = str(file_entry.get("file", ""))
        items = []
        for item in file_entry.get("items", []):
            confidence = float(item.get("confidence", 0.0))
            if confidence < min_confidence:
                continue
            items.append(
                {
                    "kind": str(item.get("kind", "uncertain")),
                    "severity": str(item.get("severity", "medium")),
                    "file": file_path,
                    "line": int(item.get("line", 1) or 1),
                    "summary": str(item.get("summary", "")).strip(),
                    "evidence": str(item.get("evidence", "")).strip(),
                    "suggested_fix": str(item.get("suggested_fix", "")).strip(),
                    "confidence": confidence,
                }
            )
        output.extend(items)
    return output


def load_findings(input_path: Path, min_confidence: float) -> list[dict[str, Any]]:
    payload = load_json(input_path)
    if "findings" in payload and isinstance(payload.get("findings"), list):
        return normalize_report_findings(payload, min_confidence)
    if "files" in payload and isinstance(payload.get("files"), list):
        return normalize_queue_files(payload, min_confidence)
    raise ValueError(f"Unsupported input format: {input_path}")


def group_findings(findings: list[dict[str, Any]], max_items_per_file: int) -> list[dict[str, Any]]:
    severity_rank = {"high": 0, "medium": 1, "low": 2}
    grouped: dict[str, list[dict[str, Any]]] = {}

    for item in findings:
        grouped.setdefault(item["file"], []).append(item)

    grouped_files: list[dict[str, Any]] = []
    for file_path in sorted(grouped):
        items = sorted(
            grouped[file_path],
            key=lambda entry: (
                severity_rank.get(entry["severity"], 3),
                entry["line"],
                entry["summary"],
            ),
        )
        if max_items_per_file > 0:
            items = items[:max_items_per_file]
        grouped_files.append({"file": file_path, "items": items})
    return grouped_files


def sanitize_filename(file_path: str) -> str:
    return file_path.replace("/", "__")


def truncate_text(value: str, limit: int) -> str:
    if len(value) <= limit:
        return value
    return value[:limit] + "\n...[truncated]..."


def read_file_content(repo_root: Path, file_path: str, char_limit: int) -> str:
    path = repo_root / file_path
    if not path.exists() or not path.is_file():
        return ""
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        content = path.read_text(encoding="utf-8", errors="replace")
    return truncate_text(content, char_limit)


def build_prompt(repo_root: Path, file_path: str, items: list[dict[str, Any]], include_file_content: bool, content_char_limit: int) -> str:
    bullets = []
    for item in items:
        line = (
            f"- [{item['severity']}/{item['kind']}] line {item['line']}: {item['summary']}\n"
            f"  evidence: {item['evidence'] or 'not provided'}\n"
            f"  suggested_fix: {item['suggested_fix'] or 'not provided'}\n"
            f"  confidence: {item['confidence']:.2f}"
        )
        bullets.append(line)

    file_block = ""
    if include_file_content:
        current_content = read_file_content(repo_root, file_path, content_char_limit)
        if current_content:
            file_block = (
                "\nCurrent file content:\n"
                f"FILE: {file_path}\n```\n{current_content}\n```\n"
            )

    return textwrap.dedent(
        f"""\
        You are editing one file in a C++ codebase.

        Target file: {file_path}

        Apply the following findings carefully:
        {'\n'.join(bullets)}
        {file_block}
        Constraints:
        - Make only changes relevant to the listed findings.
        - Preserve existing behavior unless a listed finding requires a correction.
        - Keep comments and documentation aligned with the current code.
        - If an example or API description is wrong, update it to match the code rather than inventing a new API.
        - Keep the edit minimal and readable.

        Output:
        - First, a short summary of the planned changes.
        - Then, the exact patch or replacement content for this file only.
        """
    ).strip() + "\n"


def render_text(input_path: Path, prompts: list[dict[str, Any]]) -> str:
    lines = [f"Input: {input_path}", f"Files with prompts: {len(prompts)}", ""]
    for item in prompts:
        lines.append(f"{item['file']}")
        lines.append(item["prompt"])
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def write_prompt_files(output_dir: Path, prompts: list[dict[str, Any]]) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for item in prompts:
        filename = sanitize_filename(item["file"]) + ".prompt.txt"
        (output_dir / filename).write_text(item["prompt"], encoding="utf-8")


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()

    try:
        input_path = (
            Path(args.input).expanduser().resolve()
            if args.input
            else find_latest_report(repo_root, args.round)
        )
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    try:
        findings = load_findings(input_path, args.min_confidence)
    except (ValueError, FileNotFoundError, json.JSONDecodeError) as exc:
        print(str(exc), file=sys.stderr)
        return 2

    grouped = group_findings(findings, args.max_items_per_file)
    prompts = [
        {
            "file": group["file"],
            "prompt": build_prompt(
                repo_root=repo_root,
                file_path=group["file"],
                items=group["items"],
                include_file_content=args.include_file_content,
                content_char_limit=args.content_char_limit,
            ),
            "item_count": len(group["items"]),
        }
        for group in grouped
        if group["items"]
    ]

    payload = {
        "input": str(input_path),
        "file_count": len(prompts),
        "prompts": prompts,
    }

    if args.output_dir:
        write_prompt_files(Path(args.output_dir).expanduser().resolve(), prompts)

    if args.format == "json":
        print(json.dumps(payload, indent=2))
    else:
        print(render_text(input_path, prompts), end="")

    return 0


if __name__ == "__main__":
    sys.exit(main())
