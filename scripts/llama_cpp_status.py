#!/usr/bin/env python3
"""
Summarise the latest llama.cpp workflow manifests.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarise the latest llama.cpp workflow manifests."
    )
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Repository root. Defaults to the parent of scripts/.",
    )
    parser.add_argument(
        "--format",
        choices=["text", "json"],
        default="text",
        help="Output format.",
    )
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def latest_dir(path: Path) -> Path | None:
    if not path.exists():
        return None
    dirs = sorted(item for item in path.iterdir() if item.is_dir())
    if not dirs:
        return None
    return dirs[-1]


def latest_round_report(run_dir: Path | None) -> tuple[Path | None, dict[str, Any] | None]:
    if run_dir is None:
        return None, None
    round_dirs = sorted(item for item in run_dir.iterdir() if item.is_dir() and item.name.startswith("round_"))
    for round_dir in reversed(round_dirs):
        report_path = round_dir / "report.json"
        if report_path.exists():
            return report_path, load_json(report_path)
    return None, None


def load_manifest_if_exists(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    manifest = path / "manifest.json"
    if not manifest.exists():
        return None
    return load_json(manifest)


def build_summary(repo_root: Path) -> dict[str, Any]:
    review_root = repo_root / "scripts" / "llama_review_runs"
    patch_root = repo_root / "scripts" / "llama_patch_runs"
    stage_root = repo_root / "scripts" / "llama_stage_runs"

    latest_review_dir = latest_dir(review_root)
    latest_patch_dir = latest_dir(patch_root)
    latest_stage_dir = latest_dir(stage_root)

    review_manifest = load_manifest_if_exists(latest_review_dir)
    patch_manifest = load_manifest_if_exists(latest_patch_dir)
    stage_manifest = load_manifest_if_exists(latest_stage_dir)
    review_report_path, review_report = latest_round_report(latest_review_dir)

    stage_items = stage_manifest.get("items", []) if stage_manifest else []
    stage_failed = sum(1 for item in stage_items if item.get("status") == "failed")
    stage_staged = sum(
        1
        for item in stage_items
        if item.get("status") == "staged" or item.get("mode") == "full_replacement"
    )

    return {
        "review": {
            "run_dir": str(latest_review_dir) if latest_review_dir else None,
            "manifest": review_manifest,
            "latest_report_path": str(review_report_path) if review_report_path else None,
            "latest_report": review_report,
            "finding_count": review_report.get("finding_count", 0) if review_report else 0,
        },
        "patch": {
            "run_dir": str(latest_patch_dir) if latest_patch_dir else None,
            "manifest": patch_manifest,
            "prompt_count": len(patch_manifest.get("prompt_files", [])) if patch_manifest else 0,
        },
        "stage": {
            "run_dir": str(latest_stage_dir) if latest_stage_dir else None,
            "manifest": stage_manifest,
            "item_count": len(stage_items),
            "staged_count": stage_staged,
            "failed_count": stage_failed,
        },
    }


def render_text(summary: dict[str, Any]) -> str:
    review = summary["review"]
    patch = summary["patch"]
    stage = summary["stage"]

    lines = []
    lines.append("Latest llama.cpp workflow status")
    lines.append("")

    lines.append("Review")
    lines.append(f"  run: {review['run_dir'] or 'none'}")
    lines.append(f"  latest report: {review['latest_report_path'] or 'none'}")
    lines.append(f"  findings: {review['finding_count']}")
    if review["manifest"]:
        lines.append(f"  endpoint: {review['manifest'].get('endpoint', '')}")
        lines.append(f"  model: {review['manifest'].get('model', '')}")
        lines.append(f"  chunks: {review['manifest'].get('chunk_count', '')}")
    lines.append("")

    lines.append("Patch")
    lines.append(f"  run: {patch['run_dir'] or 'none'}")
    lines.append(f"  prompts: {patch['prompt_count']}")
    if patch["manifest"]:
        lines.append(f"  endpoint: {patch['manifest'].get('endpoint', '')}")
        lines.append(f"  model: {patch['manifest'].get('model', '')}")
    lines.append("")

    lines.append("Stage")
    lines.append(f"  run: {stage['run_dir'] or 'none'}")
    lines.append(f"  staged: {stage['staged_count']}")
    lines.append(f"  failed: {stage['failed_count']}")
    lines.append(f"  total items: {stage['item_count']}")
    lines.append("")

    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    summary = build_summary(repo_root)

    if args.format == "json":
        print(json.dumps(summary, indent=2))
    else:
        print(render_text(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
