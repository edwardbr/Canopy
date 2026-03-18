#!/usr/bin/env python3
"""
Stage candidate patch outputs into a separate directory without modifying the repo.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Stage llama.cpp candidate patches into a separate directory without touching the repo."
    )
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Repository root. Defaults to the parent of scripts/.",
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Path to a candidate file or a directory containing *.candidate.txt files.",
    )
    parser.add_argument(
        "--stage-dir",
        default="",
        help="Directory to write staged files into. Defaults to scripts/llama_stage_runs/<timestamp>/",
    )
    parser.add_argument(
        "--suffix",
        default=".candidate.txt",
        help="Candidate file suffix when reading a directory.",
    )
    return parser.parse_args()


def discover_candidates(input_path: Path, suffix: str) -> list[Path]:
    if input_path.is_file():
        return [input_path]
    if input_path.is_dir():
        return sorted(input_path.glob(f"*{suffix}"))
    raise FileNotFoundError(f"Input does not exist: {input_path}")


def infer_target_path(candidate_path: Path, suffix: str) -> str:
    name = candidate_path.name
    if not name.endswith(suffix):
        raise ValueError(f"Candidate file does not end with {suffix}: {candidate_path}")
    stem = name[: -len(suffix)]
    if "__" in stem:
        return stem.replace("__", "/")
    return stem


def looks_like_patch(content: str) -> bool:
    lines = content.splitlines()
    if not lines:
        return False
    markers = (
        "*** Begin Patch",
        "--- ",
        "diff --git ",
        "*** Update File:",
        "*** Add File:",
        "*** Delete File:",
    )
    return any(line.startswith(markers) for line in lines[:20])


def stage_full_replacement(repo_root: Path, stage_dir: Path, target_rel: str, content: str) -> dict[str, Any]:
    target = repo_root / target_rel
    staged_target = stage_dir / target_rel
    staged_target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        shutil.copy2(target, staged_target)
    staged_target.write_text(content, encoding="utf-8")
    return {"mode": "full_replacement", "target": target_rel, "staged_path": str(staged_target)}


def stage_patch(repo_root: Path, stage_dir: Path, target_rel: str, content: str) -> dict[str, Any]:
    target = repo_root / target_rel
    staged_target = stage_dir / target_rel
    staged_target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        shutil.copy2(target, staged_target)
    else:
        staged_target.write_text("", encoding="utf-8")

    patch_file = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as handle:
            handle.write(content)
            patch_file = Path(handle.name)

        result = subprocess.run(
            ["patch", "-u", str(staged_target), str(patch_file)],
            cwd=repo_root,
            text=True,
            capture_output=True,
            check=False,
        )
    finally:
        if patch_file and patch_file.exists():
            patch_file.unlink()

    if result.returncode != 0:
        return {
            "mode": "patch",
            "target": target_rel,
            "status": "failed",
            "stdout": result.stdout,
            "stderr": result.stderr,
        }

    return {
        "mode": "patch",
        "target": target_rel,
        "status": "staged",
        "staged_path": str(staged_target),
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    input_path = Path(args.input).expanduser().resolve()

    try:
        candidates = discover_candidates(input_path, args.suffix)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if not candidates:
        print(f"No candidate files found under {input_path}", file=sys.stderr)
        return 2

    if args.stage_dir:
        stage_dir = Path(args.stage_dir).expanduser().resolve()
    else:
        timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        stage_dir = repo_root / "scripts" / "llama_stage_runs" / timestamp
    stage_dir.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, Any] = {
        "repo_root": str(repo_root),
        "input": str(input_path),
        "stage_dir": str(stage_dir),
        "items": [],
    }

    for candidate in candidates:
        try:
            target_rel = infer_target_path(candidate, args.suffix)
            content = candidate.read_text(encoding="utf-8")
            if looks_like_patch(content):
                result = stage_patch(repo_root, stage_dir, target_rel, content)
            else:
                result = stage_full_replacement(repo_root, stage_dir, target_rel, content)
            result["candidate"] = str(candidate)
        except Exception as exc:  # noqa: BLE001
            result = {
                "candidate": str(candidate),
                "status": "failed",
                "error": str(exc),
            }
        manifest["items"].append(result)

    manifest_path = stage_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Staged candidates under {stage_dir}")
    print(f"Manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
