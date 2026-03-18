#!/usr/bin/env python3
"""
Send prompt files to a local llama.cpp server and store candidate patch responses.
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_SYSTEM_PROMPT = """\
You are editing a single file in a C++ codebase.

Rules:
- Only change the target file described in the prompt.
- Keep edits minimal and focused on the listed findings.
- Preserve behavior unless the prompt requires a correction.
- Prefer exact replacement content for the full file.
- Do not describe the patch outside the requested output.
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send prompt files to a local llama.cpp server and store candidate patch responses."
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Path to a prompt file or a directory containing *.prompt.txt files.",
    )
    parser.add_argument(
        "--output-dir",
        default="",
        help="Directory to write responses into. Defaults to scripts/llama_patch_runs/<timestamp>/",
    )
    parser.add_argument(
        "--endpoint",
        default="http://127.0.0.1:8081/v1/chat/completions",
        help="llama-server OpenAI-compatible chat completions endpoint.",
    )
    parser.add_argument(
        "--model",
        default="qwen3.5-27b-q8",
        help="Model name sent to the OpenAI-compatible endpoint.",
    )
    parser.add_argument(
        "--temperature",
        type=float,
        default=0.0,
        help="Sampling temperature.",
    )
    parser.add_argument(
        "--top-p",
        type=float,
        default=0.9,
        help="Sampling top-p.",
    )
    parser.add_argument(
        "--top-k",
        type=int,
        default=20,
        help="Sampling top-k.",
    )
    parser.add_argument(
        "--repeat-penalty",
        type=float,
        default=1.05,
        help="Repeat penalty.",
    )
    parser.add_argument(
        "--max-output-tokens",
        type=int,
        default=8192,
        help="Maximum generation tokens sent as max_tokens.",
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
        "--system-prompt",
        default=DEFAULT_SYSTEM_PROMPT,
        help="Optional system prompt override.",
    )
    parser.add_argument(
        "--suffix",
        default=".candidate.txt",
        help="Suffix for extracted text outputs.",
    )
    return parser.parse_args()


def discover_prompt_files(input_path: Path) -> list[Path]:
    if input_path.is_file():
        return [input_path]
    if input_path.is_dir():
        return sorted(input_path.glob("*.prompt.txt"))
    raise FileNotFoundError(f"Input does not exist: {input_path}")


def call_llama(
    endpoint: str,
    model: str,
    temperature: float,
    top_p: float,
    top_k: int,
    repeat_penalty: float,
    max_output_tokens: int,
    request_timeout: int,
    progress_interval: int,
    system_prompt: str,
    user_prompt: str,
) -> dict[str, Any]:
    payload = {
        "model": model,
        "temperature": temperature,
        "top_p": top_p,
        "top_k": top_k,
        "repeat_penalty": repeat_penalty,
        "max_tokens": max_output_tokens,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
    }
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
                print(f"[llama_cpp_apply_prompts] waiting for response... {elapsed}s elapsed", file=sys.stderr)

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
    content = parsed["choices"][0]["message"]["content"]
    return {"raw_response": parsed, "content": content}


def write_outputs(output_dir: Path, prompt_file: Path, raw_response: dict[str, Any], content: str, suffix: str) -> None:
    base = prompt_file.name.removesuffix(".prompt.txt")
    raw_path = output_dir / f"{base}.raw.json"
    candidate_path = output_dir / f"{base}{suffix}"
    raw_path.write_text(json.dumps(raw_response, indent=2) + "\n", encoding="utf-8")
    candidate_path.write_text(content, encoding="utf-8")


def main() -> int:
    args = parse_args()
    input_path = Path(args.input).expanduser().resolve()

    try:
        prompt_files = discover_prompt_files(input_path)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if not prompt_files:
        print(f"No prompt files found under {input_path}", file=sys.stderr)
        return 2

    if args.output_dir:
        output_dir = Path(args.output_dir).expanduser().resolve()
    else:
        output_dir = Path(__file__).resolve().parents[1] / "scripts" / "llama_patch_runs" / input_path.stem
    output_dir.mkdir(parents=True, exist_ok=True)

    manifest = {
        "input": str(input_path),
        "prompt_files": [str(path) for path in prompt_files],
        "endpoint": args.endpoint,
        "model": args.model,
        "max_output_tokens": args.max_output_tokens,
    }
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    for prompt_file in prompt_files:
        prompt_text = prompt_file.read_text(encoding="utf-8")
        print(f"Running {prompt_file.name}")
        try:
            response = call_llama(
                endpoint=args.endpoint,
                model=args.model,
                temperature=args.temperature,
                top_p=args.top_p,
                top_k=args.top_k,
                repeat_penalty=args.repeat_penalty,
                max_output_tokens=args.max_output_tokens,
                request_timeout=args.request_timeout,
                progress_interval=args.progress_interval,
                system_prompt=args.system_prompt,
                user_prompt=prompt_text,
            )
        except RuntimeError as exc:
            print(str(exc), file=sys.stderr)
            return 1

        write_outputs(
            output_dir=output_dir,
            prompt_file=prompt_file,
            raw_response=response["raw_response"],
            content=response["content"],
            suffix=args.suffix,
        )

    print(f"Wrote candidate responses to {output_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
