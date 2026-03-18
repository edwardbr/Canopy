# Llama.cpp Review Loop

This script runs an iterative review loop against a local `llama-server` from `llama.cpp`.

It is designed for:

- stale or inaccurate documentation
- comments that no longer match the code
- example code that no longer matches public APIs
- user-facing correctness issues

It does not prove correctness on its own. Use it together with real build and test commands.

## Files

- `scripts/llama_cpp_review_loop.py`
- `scripts/llama_cpp_fix_queue.py`
- `scripts/llama_cpp_fix_prompts.py`
- `scripts/llama_cpp_apply_prompts.py`
- `scripts/llama_cpp_stage_candidates.py`
- `scripts/llama_cpp_status.py`
- run output is written under `scripts/llama_review_runs/<timestamp>/`

Each round stores:

- the raw response from the model for each chunk
- the extracted text content for each chunk
- the parsed JSON for each chunk
- a deduplicated `report.json`
- optional `fix_command.json`
- optional `check_command.json`

If the model returns empty or invalid JSON, the review loop no longer crashes. It records the raw content, creates a low-severity fallback finding, and continues so you can inspect the bad response later.

The fix queue helper reads a `report.json` and groups findings by file for manual editing or downstream automation.
The prompt helper turns a report or fix queue into one edit prompt per file.
The apply helper sends those prompts to `llama-server` and stores candidate outputs.
The staging helper applies those candidates to a separate staging tree instead of the repo.
The status helper gives a one-page summary of the latest review, patch, and stage runs.

## Prerequisites

You need:

- a local `llama-server`
- Python 3.9+

`rg` is preferred for faster file discovery, but it is optional. If it is not installed, the review script falls back to Python directory walking.

## Start llama-server

Example:

```bash
llama-server -m /path/to/model.gguf --port 8081 -c 16384 -np 2
```

The script defaults to:

```text
http://127.0.0.1:8081/v1/chat/completions
```

## Basic usage

From the repo root:

```bash
python3 scripts/llama_cpp_review_loop.py
```

Default review scope:

- `README.md`
- `documents`
- `rpc/include`

## Review only a subset

```bash
python3 scripts/llama_cpp_review_loop.py rpc/include documents
```

Or just public headers:

```bash
python3 scripts/llama_cpp_review_loop.py rpc/include
```

## Change the endpoint or model name

```bash
python3 scripts/llama_cpp_review_loop.py \
  --model qwen2.5-coder
```

The `--model` value is only the request field sent to the OpenAI-compatible server. It does not load the model by itself.

The script also requests JSON output explicitly and uses low-variance defaults that tend to behave better for review loops:

- `--temperature 0.0`
- `--top-p 0.9`
- `--top-k 20`
- `--repeat-penalty 1.05`
- `--request-timeout 1800`
- `--progress-interval 30`
- `--auto-retry-splits 2`

If your local model is slow to produce a first token, increase the request timeout:

```bash
python3 scripts/llama_cpp_review_loop.py \
  --request-timeout 3600 \
  --model qwen3.5-27b-q8 \
  README.md documents rpc/include
```

Use no timeout and print a heartbeat every minute:

```bash
python3 scripts/llama_cpp_review_loop.py \
  --request-timeout 0 \
  --progress-interval 60 \
  --auto-retry-splits 2 \
  --model qwen3.5-27b-q8 \
  README.md documents rpc/include
```

If the model returns empty content or malformed JSON for a chunk, the review loop can automatically split that chunk into smaller pieces and retry. Each retry level halves the chunk again until the configured split depth is exhausted.

## Strict findings-only mode

If you want easier downstream automation, use:

```bash
python3 scripts/llama_cpp_review_loop.py \
  --strict-findings-only \
  --model qwen3.5-27b-q8 \
  README.md documents rpc/include
```

In this mode, the model is asked to return only:

```json
{
  "findings": []
}
```

with no extra top-level fields. This is more reliable if another script is consuming the output directly.

## Add a check command

This is the useful mode for iterative cleanup. After each round, the script runs the command and feeds its output into the next round.

Example:

```bash
python3 scripts/llama_cpp_review_loop.py rpc/include documents \
  --check-command "cmake --build build_debug --target rpc_test"
```

Available placeholders inside `--check-command`:

- `{repo_root}`
- `{run_dir}`
- `{round}`
- `{report_json}`

Example using the report file:

```bash
python3 scripts/llama_cpp_review_loop.py rpc/include documents \
  --check-command "python3 tools/inspect_report.py {report_json}"
```

## Add a fix command

If you have another local tool or agent that can consume the JSON report and edit files, you can run it between rounds.

Example:

```bash
python3 scripts/llama_cpp_review_loop.py rpc/include documents \
  --fix-command "python3 tools/apply_doc_fixes.py {report_json}" \
  --check-command "cmake --build build_debug --target rpc_test"
```

The loop is:

1. review the selected files
2. write `report.json`
3. run `--fix-command` if provided
4. run `--check-command` if provided
5. re-review with the previous findings and latest command output in context

## Build a compact fix queue

After a review run, create a grouped queue from the latest report:

```bash
python3 scripts/llama_cpp_fix_queue.py
```

JSON output:

```bash
python3 scripts/llama_cpp_fix_queue.py --format json
```

Use a specific report:

```bash
python3 scripts/llama_cpp_fix_queue.py \
  --report scripts/llama_review_runs/20260318_120000/round_01/report.json
```

Filter out weak findings:

```bash
python3 scripts/llama_cpp_fix_queue.py --min-confidence 0.8
```

Limit queue size per file:

```bash
python3 scripts/llama_cpp_fix_queue.py --max-items-per-file 3
```

## Generate per-file edit prompts

From the latest review report:

```bash
python3 scripts/llama_cpp_fix_prompts.py
```

From a fix-queue JSON:

```bash
python3 scripts/llama_cpp_fix_queue.py --format json > /tmp/fix-queue.json
python3 scripts/llama_cpp_fix_prompts.py --input /tmp/fix-queue.json
```

Write one prompt file per source file:

```bash
python3 scripts/llama_cpp_fix_prompts.py \
  --output-dir /tmp/fix-prompts
```

Include current file content in each prompt:

```bash
python3 scripts/llama_cpp_fix_prompts.py \
  --include-file-content \
  --content-char-limit 12000
```

Machine-friendly JSON output:

```bash
python3 scripts/llama_cpp_fix_prompts.py --format json
```

## Generate candidate patches with llama-server

From a directory of prompt files:

```bash
python3 scripts/llama_cpp_apply_prompts.py \
  --input /tmp/fix-prompts
```

For slower models:

```bash
python3 scripts/llama_cpp_apply_prompts.py \
  --input /tmp/fix-prompts \
  --request-timeout 3600
```

Use no timeout and progress heartbeats:

```bash
python3 scripts/llama_cpp_apply_prompts.py \
  --input /tmp/fix-prompts \
  --request-timeout 0 \
  --progress-interval 60
```

From one prompt file:

```bash
python3 scripts/llama_cpp_apply_prompts.py \
  --input /tmp/fix-prompts/README.md.prompt.txt
```

Use a custom output directory:

```bash
python3 scripts/llama_cpp_apply_prompts.py \
  --input /tmp/fix-prompts \
  --output-dir /tmp/llama-candidates
```

The script writes:

- `*.raw.json` with the full server response
- `*.candidate.txt` with the extracted model output

Recommended end-to-end flow:

```bash
python3 scripts/llama_cpp_review_loop.py \
  --strict-findings-only \
  README.md documents rpc/include

python3 scripts/llama_cpp_fix_queue.py --format json > /tmp/fix-queue.json

python3 scripts/llama_cpp_fix_prompts.py \
  --input /tmp/fix-queue.json \
  --output-dir /tmp/fix-prompts \
  --include-file-content

python3 scripts/llama_cpp_apply_prompts.py \
  --input /tmp/fix-prompts \
  --output-dir /tmp/llama-candidates

python3 scripts/llama_cpp_stage_candidates.py \
  --input /tmp/llama-candidates
```

## Stage candidate patches safely

Apply candidates into a separate staging directory:

```bash
python3 scripts/llama_cpp_stage_candidates.py \
  --input /tmp/llama-candidates
```

Use a custom staging directory:

```bash
python3 scripts/llama_cpp_stage_candidates.py \
  --input /tmp/llama-candidates \
  --stage-dir /tmp/llama-stage
```

The script writes a `manifest.json` describing which candidates were staged, which were treated as full replacements, and which failed to apply.

## Show the latest workflow status

Text summary:

```bash
python3 scripts/llama_cpp_status.py
```

JSON summary:

```bash
python3 scripts/llama_cpp_status.py --format json
```

## Suggested first runs

Public docs and API surface:

```bash
python3 scripts/llama_cpp_review_loop.py README.md documents rpc/include
```

Docs plus a real build:

```bash
python3 scripts/llama_cpp_review_loop.py README.md documents rpc/include \
  --check-command "cmake --build build_debug"
```

Narrow doc drift check:

```bash
python3 scripts/llama_cpp_review_loop.py README.md documents/09-api-reference.md rpc/include
```

## Recommended workflow

Use the loop in three passes instead of sending the entire repo every time:

1. `README.md`, `documents`, and `rpc/include`
2. generated interface examples and transport docs
3. code plus a targeted build or test command

This keeps the prompt smaller and the findings more reliable.

## Stopping conditions

The script stops when:

- the model reports zero findings
- the same deduplicated findings repeat unchanged
- `--max-rounds` is reached

## Notes

- The script uses only the Python standard library.
- The reviewer prompt is intentionally strict and asks for JSON only.
- Findings are deduplicated by kind, file, line, summary, and suggested fix.
- Very large files are chunked, but smaller focused scopes usually work better.
