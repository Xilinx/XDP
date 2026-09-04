#!/bin/bash
# Patch CSR merged TQL to copy custom XRT RPMs and install them on the board.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.defaults.sh
source "${SCRIPT_DIR}/config.defaults.sh"

usage() {
  cat <<'EOF'
Usage: patch_merged_tql.sh --sprite-dir DIR --rpm-dir DIR [--suite SUITE]

Appends a GLOBAL APPLY block to the merged VE2_XDP TQL in the sprite directory.
EOF
}

SPRITE_DIR=""
RPM_DIR=""
SUITE="${CSR_SUITE}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --sprite-dir)
      SPRITE_DIR="$2"
      shift 2
      ;;
    --rpm-dir)
      RPM_DIR="$2"
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

if [ -z "${SPRITE_DIR}" ] || [ -z "${RPM_DIR}" ]; then
  echo "Missing required --sprite-dir and/or --rpm-dir" >&2
  usage >&2
  exit 1
fi

if [ ! -d "${RPM_DIR}" ] || [ -z "$(find "${RPM_DIR}" -maxdepth 1 -name '*.rpm' -print -quit)" ]; then
  echo "RPM directory is empty or missing: ${RPM_DIR}" >&2
  exit 1
fi

shopt -s nullglob
TQL_FILES=( "${SPRITE_DIR}"/*"${SUITE}"*.tql )
shopt -u nullglob

if [ "${#TQL_FILES[@]}" -eq 0 ]; then
  echo "No merged TQL matching suite ${SUITE} under ${SPRITE_DIR}" >&2
  ls -la "${SPRITE_DIR}" >&2 || true
  exit 1
fi

TQL_FILE="${TQL_FILES[0]}"
RPM_BASENAME="$(basename "${RPM_DIR}")"
MARKER="# XDP CI: install custom XRT RPMs on Telluride before board run"

if grep -qF "${MARKER}" "${TQL_FILE}"; then
  echo "TQL already patched: ${TQL_FILE}"
  exit 0
fi

cat >> "${TQL_FILE}" <<EOF

${MARKER}
GLOBAL APPLY * MODIFY
    copy += ['${RPM_DIR}'],
    tasks.board.pre_exec = 'cd ${RPM_BASENAME} && dnf --disablerepo="*" -y install ${VE2_DNF_PACKAGES}';
EOF

echo "Patched ${TQL_FILE}"
echo "  copy += ['${RPM_DIR}']"
echo "  tasks.board.pre_exec = cd ${RPM_BASENAME} && dnf ... ${VE2_DNF_PACKAGES}"
