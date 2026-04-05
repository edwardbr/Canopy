#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Convert copied vendor trees under c++/submodules back into real git submodules,
preserving the currently pinned commit whenever possible.

Usage:
  scripts/repair-cpp-submodules.sh [--discard-existing] [path ...]

Options:
  --discard-existing
      Remove the existing worktree copy instead of moving it aside to
      <path>.pre-submodule-backup before re-adding the submodule.

  -h, --help
      Show this help.

If no paths are provided, every submodule declared in .gitmodules whose path is
under c++/submodules/ will be processed.
EOF
}

discard_existing=0

while (($#)); do
  case "$1" in
    --discard-existing)
      discard_existing=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      break
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

cd "${repo_root}"

if [[ ! -f .gitmodules ]]; then
  echo "Expected to run inside the Canopy repository root." >&2
  exit 1
fi

declare -A submodule_url_by_path
declare -a declared_cpp_submodules

while IFS= read -r key; do
  section="${key%.path}"
  path="$(git config -f .gitmodules --get "${key}")"
  url="$(git config -f .gitmodules --get "${section}.url")"
  if [[ "${path}" == c++/submodules/* ]]; then
    declared_cpp_submodules+=("${path}")
    submodule_url_by_path["${path}"]="${url}"
  fi
done < <(git config -f .gitmodules --name-only --get-regexp '^submodule\..*\.path$')

if ((${#declared_cpp_submodules[@]} == 0)); then
  echo "No c++/submodules entries found in .gitmodules." >&2
  exit 1
fi

declare -a requested_paths
if (($#)); then
  for path in "$@"; do
    if [[ -z "${submodule_url_by_path["${path}"]+x}" ]]; then
      echo "Path is not declared in .gitmodules as a c++ submodule: ${path}" >&2
      exit 1
    fi
    requested_paths+=("${path}")
  done
else
  requested_paths=("${declared_cpp_submodules[@]}")
fi

git_module_dirs_for_path() {
  local path="$1"
  local basename="${path##*/}"
  local candidate_one=".git/modules/${path}"
  local candidate_two=".git/modules/submodules/${basename}"

  [[ -e "${candidate_one}" ]] && printf '%s\n' "${candidate_one}"
  if [[ -e "${candidate_two}" && "${candidate_two}" != "${candidate_one}" ]]; then
    printf '%s\n' "${candidate_two}"
  fi
}

is_real_gitlink() {
  local path="$1"
  local entries
  entries="$(git ls-files --stage -- "${path}")" || return 1
  [[ -n "${entries}" ]] || return 1
  [[ "$(printf '%s\n' "${entries}" | wc -l | tr -d ' ')" == "1" ]] || return 1
  [[ "$(printf '%s\n' "${entries}" | awk 'NR==1 { print $1 }')" == "160000" ]]
}

resolve_target_commit() {
  local path="$1"
  local commit=""

  if [[ -d "${path}/.git" || -f "${path}/.git" ]]; then
    commit="$(git -C "${path}" rev-parse HEAD 2>/dev/null || true)"
  fi

  if [[ -z "${commit}" ]]; then
    commit="$(git ls-files --stage -- "${path}" 2>/dev/null | awk 'NR==1 && $1 == "160000" { print $2 }')"
  fi

  if [[ -z "${commit}" ]]; then
    while IFS= read -r module_dir; do
      [[ -n "${module_dir}" ]] || continue
      commit="$(git --git-dir="${module_dir}" rev-parse HEAD 2>/dev/null || true)"
      [[ -n "${commit}" ]] && break
    done < <(git_module_dirs_for_path "${path}")
  fi

  printf '%s\n' "${commit}"
}

echo "Repository root: ${repo_root}"
echo "Processing ${#requested_paths[@]} c++ submodule path(s)"

for path in "${requested_paths[@]}"; do
  url="${submodule_url_by_path["${path}"]}"
  target_commit="$(resolve_target_commit "${path}")"

  echo
  echo "==> ${path}"
  [[ -n "${target_commit}" ]] && echo "Preserving commit ${target_commit}"

  if is_real_gitlink "${path}"; then
    current_commit="$(git ls-files --stage -- "${path}" | awk 'NR==1 { print $2 }')"
    if [[ -n "${target_commit}" && "${current_commit}" == "${target_commit}" ]]; then
      echo "Already a git submodule at the desired commit; skipping."
      continue
    fi
  fi

  if git ls-files --error-unmatch -- "${path}" >/dev/null 2>&1; then
    echo "Removing existing index entries for ${path}"
    git rm -r --cached -- "${path}"
  else
    echo "No existing index entries found for ${path}"
  fi

  declare -a module_dirs_to_remove=()
  while IFS= read -r module_dir; do
    [[ -n "${module_dir}" ]] || continue
    module_dirs_to_remove+=("${module_dir}")
  done < <(git_module_dirs_for_path "${path}")

  if [[ -e "${path}" ]]; then
    if ((discard_existing)); then
      echo "Discarding existing worktree at ${path}"
      rm -rf -- "${path}"
    else
      backup_path="${path}.pre-submodule-backup"
      if [[ -e "${backup_path}" ]]; then
        echo "Backup path already exists: ${backup_path}" >&2
        exit 1
      fi
      echo "Moving existing worktree to ${backup_path}"
      mv -- "${path}" "${backup_path}"
    fi
  fi

  for module_dir in "${module_dirs_to_remove[@]}"; do
    echo "Removing stale git module metadata ${module_dir}"
    rm -rf -- "${module_dir}"
  done

  mkdir -p -- "$(dirname "${path}")"

  echo "Adding submodule ${url} -> ${path}"
  git submodule add --force "${url}" "${path}"

  if [[ -n "${target_commit}" ]]; then
    echo "Checking out preserved commit ${target_commit}"
    git -C "${path}" fetch --depth 1 origin "${target_commit}" || git -C "${path}" fetch origin "${target_commit}"
    git -C "${path}" checkout --detach "${target_commit}"
    git add -- "${path}"
  fi

  if ! is_real_gitlink "${path}"; then
    echo "Expected ${path} to become a gitlink, but it did not." >&2
    exit 1
  fi
done

echo
echo "Repair complete."
echo "Verify with:"
echo "  git ls-files --stage | rg 'c\\+\\+/submodules/[^/]+$'"
echo
echo "Each c++ submodule root should now appear once with mode 160000."
