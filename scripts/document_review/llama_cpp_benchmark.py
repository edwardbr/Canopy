#!/usr/bin/env python3
"""
Benchmark llama-server review-loop configurations for prompt-heavy Canopy review runs.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import shlex
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_VARIANTS = [
    {"ctx": 65536, "ub": 8192},
    {"ctx": 65536, "ub": 16384},
    {"ctx": 32768, "ub": 8192},
    {"ctx": 32768, "ub": 16384},
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark llama-server variants against the local review loop."
    )
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[2]),
        help="Repository root. Defaults to the parent of scripts/.",
    )
    parser.add_argument(
        "--server-cmd",
        required=True,
        help=(
            "Base llama-server command. Use {ctx}, {ub}, and {port} placeholders. "
            'Example: \'llama-server -m ~/model.gguf -c {ctx} -ub {ub} --port {port} -np 1\''
        ),
    )
    parser.add_argument(
        "--review-paths",
        nargs="*",
        default=["README.md", "documents", "rpc/include"],
        help="Repo-relative paths passed to the review loop.",
    )
    parser.add_argument(
        "--chunk-chars",
        type=int,
        default=12000,
        help="Chunk size passed to the review loop.",
    )
    parser.add_argument(
        "--max-rounds",
        type=int,
        default=1,
        help="Max rounds passed to the review loop for each benchmark run.",
    )
    parser.add_argument(
        "--model",
        default="local-model",
        help="Model field sent to llama-server by the review loop.",
    )
    parser.add_argument(
        "--port-base",
        type=int,
        default=8091,
        help="Starting port. Each variant uses a consecutive port.",
    )
    parser.add_argument(
        "--startup-timeout",
        type=int,
        default=180,
        help="Seconds to wait for llama-server to start accepting connections.",
    )
    parser.add_argument(
        "--request-timeout",
        type=int,
        default=0,
        help="Request timeout passed to the review loop. Use 0 for no timeout.",
    )
    parser.add_argument(
        "--progress-interval",
        type=int,
        default=60,
        help="Progress heartbeat interval passed to the review loop.",
    )
    parser.add_argument(
        "--slot-id",
        type=int,
        default=0,
        help="Slot id passed to the review loop.",
    )
    parser.add_argument(
        "--disable-prewarm",
        action="store_true",
        help="Disable review-loop prefix prewarming.",
    )
    parser.add_argument(
        "--disable-cache-prompt",
        action="store_true",
        help="Disable review-loop cache_prompt support.",
    )
    parser.add_argument(
        "--variants-json",
        default="",
        help="Optional JSON array of variant objects instead of the built-in ctx/ub matrix.",
    )
    return parser.parse_args()


def load_variants(args: argparse.Namespace) -> list[dict[str, Any]]:
    if not args.variants_json:
        return list(DEFAULT_VARIANTS)
    raw = json.loads(args.variants_json)
    if not isinstance(raw, list):
        raise ValueError("--variants-json must be a JSON array.")
    variants: list[dict[str, Any]] = []
    for item in raw:
        if not isinstance(item, dict):
            raise ValueError("Each variant must be a JSON object.")
        variants.append(item)
    return variants


def wait_for_port(host: str, port: int, timeout_seconds: int) -> bool:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=2):
                return True
        except OSError:
            time.sleep(1)
    return False


def endpoint_ready(endpoint: str, timeout_seconds: int) -> bool:
    deadline = time.monotonic() + timeout_seconds
    request = urllib.request.Request(endpoint, method="GET")
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(request, timeout=3):
                return True
        except Exception:
            time.sleep(1)
    return False


def latest_run_dir(review_root: Path, before: set[str]) -> Path | None:
    current = {item.name for item in review_root.iterdir() if item.is_dir()} if review_root.exists() else set()
    new_items = sorted(current - before)
    if not new_items:
        return None
    return review_root / new_items[-1]


def load_json_if_exists(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def summarize_run(run_dir: Path | None) -> dict[str, Any]:
    if run_dir is None:
        return {
            "run_dir": None,
            "finding_count": None,
            "chunk_count": None,
            "empty_response_count": None,
            "status": "missing_run_dir",
        }

    manifest = load_json_if_exists(run_dir / "manifest.json") or {}
    master = load_json_if_exists(run_dir / "all_rounds_result.json") or {}
    findings = master.get("findings", [])
    empty_response_count = sum(
        1
        for item in findings
        if item.get("file") == "__llm_response__" or item.get("summary") == "Model response was not valid JSON."
    )
    return {
        "run_dir": str(run_dir),
        "finding_count": len(findings) if isinstance(findings, list) else None,
        "chunk_count": manifest.get("chunk_count"),
        "empty_response_count": empty_response_count,
        "status": master.get("status", "ok" if master else "missing_master_report"),
    }


def terminate_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def render_table(results: list[dict[str, Any]]) -> str:
    headers = [
        "variant",
        "wall_s",
        "findings",
        "chunks",
        "empty_json",
        "status",
        "run_dir",
    ]
    rows = [headers]
    for item in results:
        rows.append(
            [
                item["variant_name"],
                f"{item['wall_seconds']:.1f}" if item.get("wall_seconds") is not None else "n/a",
                str(item.get("finding_count", "n/a")),
                str(item.get("chunk_count", "n/a")),
                str(item.get("empty_response_count", "n/a")),
                str(item.get("status", "n/a")),
                str(item.get("run_dir", "n/a")),
            ]
        )

    widths = [max(len(str(row[col])) for row in rows) for col in range(len(headers))]
    lines = []
    for index, row in enumerate(rows):
        lines.append("  ".join(str(cell).ljust(widths[col]) for col, cell in enumerate(row)))
        if index == 0:
            lines.append("  ".join("-" * widths[col] for col in range(len(headers))))
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    review_root = repo_root / "scripts" / "llama_review_runs"
    review_root.mkdir(parents=True, exist_ok=True)
    variants = load_variants(args)

    benchmark_dir = repo_root / "scripts" / "llama_benchmark_runs" / dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    benchmark_dir.mkdir(parents=True, exist_ok=True)

    results: list[dict[str, Any]] = []

    for index, variant in enumerate(variants):
        ctx = int(variant["ctx"])
        ub = int(variant["ub"])
        port = args.port_base + index
        variant_name = f"c{ctx}_ub{ub}"
        server_cmd = args.server_cmd.format(ctx=ctx, ub=ub, port=port)
        server_args = shlex.split(os.path.expanduser(server_cmd))
        server_log = benchmark_dir / f"{variant_name}_server.log"
        review_log = benchmark_dir / f"{variant_name}_review.log"

        print(f"[benchmark] starting {variant_name} on port {port}", file=sys.stderr)
        with server_log.open("w", encoding="utf-8") as server_handle:
            server_process = subprocess.Popen(
                server_args,
                cwd=repo_root,
                stdout=server_handle,
                stderr=subprocess.STDOUT,
                text=True,
                preexec_fn=os.setsid,
            )

        try:
            if not wait_for_port("127.0.0.1", port, args.startup_timeout):
                raise RuntimeError(f"llama-server did not open port {port} for {variant_name}")

            before_runs = {item.name for item in review_root.iterdir() if item.is_dir()}
            review_cmd = [
                sys.executable,
                str(repo_root / "scripts" / "document_review" / "llama_cpp_review_loop.py"),
                "--endpoint",
                f"http://127.0.0.1:{port}/v1/chat/completions",
                "--model",
                args.model,
                "--max-rounds",
                str(args.max_rounds),
                "--chunk-chars",
                str(args.chunk_chars),
                "--request-timeout",
                str(args.request_timeout),
                "--progress-interval",
                str(args.progress_interval),
                "--slot-id",
                str(args.slot_id),
            ]
            if not args.disable_cache_prompt:
                review_cmd.append("--cache-prompt")
            else:
                review_cmd.append("--no-cache-prompt")
            if not args.disable_prewarm:
                review_cmd.append("--prewarm-slot")
            review_cmd.extend(args.review_paths)

            started = time.monotonic()
            with review_log.open("w", encoding="utf-8") as review_handle:
                review_proc = subprocess.run(
                    review_cmd,
                    cwd=repo_root,
                    stdout=review_handle,
                    stderr=subprocess.STDOUT,
                    text=True,
                    check=False,
                )
            wall_seconds = time.monotonic() - started
            run_dir = latest_run_dir(review_root, before_runs)
            summary = summarize_run(run_dir)
            summary.update(
                {
                    "variant_name": variant_name,
                    "ctx": ctx,
                    "ub": ub,
                    "port": port,
                    "wall_seconds": wall_seconds,
                    "review_returncode": review_proc.returncode,
                    "server_log": str(server_log),
                    "review_log": str(review_log),
                }
            )
            results.append(summary)
        finally:
            terminate_process(server_process)

    payload = {
        "benchmark_dir": str(benchmark_dir),
        "variants": variants,
        "results": results,
    }
    (benchmark_dir / "results.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    print(render_table(results))
    print()
    print(f"results.json: {benchmark_dir / 'results.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
