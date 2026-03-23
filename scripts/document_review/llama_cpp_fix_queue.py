#!/usr/bin/env python3
"""
Build a compact fix queue from a llama.cpp review-loop report.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a grouped fix queue from a llama.cpp review-loop report."
    )
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[2]),
        help="Repository root. Defaults to the parent of scripts/.",
    )
    parser.add_argument(
        "--report",
        default="",
        help="Path to a specific report.json. If omitted, the latest run report is used.",
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
        "--exclude-path-substring",
        action="append",
        default=[],
        help="Drop findings whose file path contains this substring. Repeatable.",
    )
    parser.add_argument(
        "--exclude-summary-regex",
        action="append",
        default=[],
        help="Drop findings whose summary matches this regex. Repeatable.",
    )
    parser.add_argument(
        "--exclude-evidence-regex",
        action="append",
        default=[],
        help="Drop findings whose evidence matches this regex. Repeatable.",
    )
    parser.add_argument(
        "--exclude-file",
        action="append",
        default=[],
        help="Drop findings for this exact file path. Repeatable.",
    )
    parser.add_argument(
        "--drop-llm-response",
        action="store_true",
        help="Drop synthetic __llm_response__ findings.",
    )
    parser.add_argument(
        "--dedupe-by-summary",
        action="store_true",
        help="Collapse findings more aggressively by file, line, and summary before grouping.",
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


def find_report(repo_root: Path, report_arg: str, round_number: int) -> Path:
    if report_arg:
        report_path = Path(report_arg).expanduser().resolve()
        if not report_path.exists():
            raise FileNotFoundError(f"Report does not exist: {report_path}")
        return report_path

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


def normalize_findings(items: list[dict[str, Any]], min_confidence: float) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for item in items:
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


def filter_findings(
    findings: list[dict[str, Any]],
    *,
    exclude_path_substrings: list[str],
    exclude_summary_regexes: list[str],
    exclude_evidence_regexes: list[str],
    exclude_files: list[str],
    drop_llm_response: bool,
) -> list[dict[str, Any]]:
    compiled_regexes = [re.compile(pattern, re.IGNORECASE) for pattern in exclude_summary_regexes]
    compiled_evidence_regexes = [re.compile(pattern, re.IGNORECASE) for pattern in exclude_evidence_regexes]
    exact_files = set(exclude_files)
    output: list[dict[str, Any]] = []

    for item in findings:
        file_path = item["file"]
        summary = item["summary"]
        evidence = item["evidence"]

        if drop_llm_response and file_path == "__llm_response__":
            continue
        if file_path in exact_files:
            continue
        if any(part and part in file_path for part in exclude_path_substrings):
            continue
        if any(regex.search(summary) for regex in compiled_regexes):
            continue
        if any(regex.search(evidence) for regex in compiled_evidence_regexes):
            continue

        output.append(item)

    return output


def dedupe_findings(findings: list[dict[str, Any]], dedupe_by_summary: bool) -> list[dict[str, Any]]:
    seen: set[tuple[Any, ...]] = set()
    output: list[dict[str, Any]] = []

    for item in findings:
        if dedupe_by_summary:
            key = (item["file"], item["line"], item["summary"])
        else:
            key = (item["file"], item["line"], item["summary"], item["suggested_fix"], item["evidence"])
        if key in seen:
            continue
        seen.add(key)
        output.append(item)

    return output


def group_findings(findings: list[dict[str, Any]], max_items_per_file: int) -> list[dict[str, Any]]:
    severity_rank = {"high": 0, "medium": 1, "low": 2}
    grouped: dict[str, list[dict[str, Any]]] = {}

    for item in findings:
        grouped.setdefault(item["file"], []).append(item)

    files: list[dict[str, Any]] = []
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
        files.append(
            {
                "file": file_path,
                "count": len(items),
                "highest_severity": min(
                    (severity_rank.get(entry["severity"], 3) for entry in items),
                    default=3,
                ),
                "items": items,
            }
        )

    files.sort(key=lambda entry: (entry["highest_severity"], entry["file"]))
    for entry in files:
        entry["highest_severity"] = {0: "high", 1: "medium", 2: "low"}.get(entry["highest_severity"], "unknown")
    return files


def render_text(report_path: Path, queue: list[dict[str, Any]]) -> str:
    lines = [f"Report: {report_path}", f"Files with findings: {len(queue)}", ""]
    for file_entry in queue:
        lines.append(f"{file_entry['file']} [{file_entry['highest_severity']}] ({file_entry['count']})")
        for item in file_entry["items"]:
            lines.append(
                f"  - {item['severity']} {item['kind']} line {item['line']}: {item['summary']}"
            )
            if item["suggested_fix"]:
                lines.append(f"    fix: {item['suggested_fix']}")
            if item["evidence"]:
                lines.append(f"    evidence: {item['evidence']}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    try:
        report_path = find_report(repo_root, args.report, args.round)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    report = load_json(report_path)
    findings = normalize_findings(report.get("findings", []), args.min_confidence)
    findings = filter_findings(
        findings,
        exclude_path_substrings=args.exclude_path_substring,
        exclude_summary_regexes=args.exclude_summary_regex,
        exclude_evidence_regexes=args.exclude_evidence_regex,
        exclude_files=args.exclude_file,
        drop_llm_response=args.drop_llm_response,
    )
    findings = dedupe_findings(findings, args.dedupe_by_summary)
    queue = group_findings(findings, args.max_items_per_file)

    payload = {
        "report": str(report_path),
        "finding_count": len(findings),
        "files": queue,
    }

    if args.format == "json":
        print(json.dumps(payload, indent=2))
    else:
        print(render_text(report_path, queue), end="")

    return 0


if __name__ == "__main__":
    sys.exit(main())
