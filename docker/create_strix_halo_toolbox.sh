#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

image_name="${IMAGE_NAME:-localhost/canopy-strix-halo:rocm7-nightlies}"
toolbox_name="${TOOLBOX_NAME:-canopy-strix-halo}"

podman build \
  -f "${script_dir}/Dockerfile.strix_halo" \
  -t "${image_name}" \
  "${repo_root}"

toolbox create "${toolbox_name}" \
  --image "${image_name}" \
  -- \
  --device /dev/dri \
  --device /dev/kfd \
  --device /dev/snd \
  --group-add video \
  --group-add render \
  --group-add audio \
  --group-add sudo \
  --security-opt seccomp=unconfined
