#!/bin/bash
# Patch merged TQL to compile vaiml natively (no Docker) for local RDI trial runs.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.defaults.sh
source "${SCRIPT_DIR}/config.defaults.sh"

usage() {
  cat <<'EOF'
Usage: patch_merged_tql_no_docker.sh --sprite-dir DIR [--suite SUITE]

Appends a GLOBAL APPLY block that sets user.use_docker = 0 and drops the
(vai_docker) constraint from tasks.vaiml.select_resource.

Set XDP_CSR_USE_DOCKER=1 to skip this patch (production docker compile path).
EOF
}

SPRITE_DIR=""
SUITE="${CSR_SUITE}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --sprite-dir)
      SPRITE_DIR="$2"
      shift 2
      ;;
    --suite)
      SUITE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ -z "${SPRITE_DIR}" ]; then
  echo "Missing required --sprite-dir" >&2
  usage >&2
  exit 1
fi

if [ "${XDP_CSR_USE_DOCKER:-0}" = "1" ]; then
  echo "XDP_CSR_USE_DOCKER=1; keeping suite default docker compile (no patch)"
  exit 0
fi

shopt -s nullglob
TQL_FILES=( "${SPRITE_DIR}"/*"${SUITE}"*.tql )
shopt -u nullglob

if [ "${#TQL_FILES[@]}" -eq 0 ]; then
  echo "No merged TQL matching suite ${SUITE} under ${SPRITE_DIR}" >&2
  exit 1
fi

TQL_FILE="${TQL_FILES[0]}"
MARKER="# XDP CI: native vaiml compile (user.use_docker = 0)"

if grep -qF "${MARKER}" "${TQL_FILE}"; then
  echo "TQL already patched for no-docker compile: ${TQL_FILE}"
  exit 0
fi

cat >> "${TQL_FILE}" <<'EOF'

# XDP CI: native vaiml compile (user.use_docker = 0)
GLOBAL APPLY * MODIFY
    user.use_docker = 0,
    tasks.vaiml.select_resource = "(ostype == ubuntu2204 || ostype == ubuntu22044 || ostype == ubuntu24041)";
EOF

echo "Patched ${TQL_FILE} for native vaiml compile"
echo "  user.use_docker = 0"
echo "  tasks.vaiml.select_resource = (ubuntu hosts, no vai_docker tag)"
