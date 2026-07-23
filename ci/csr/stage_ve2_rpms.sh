#!/bin/bash
# Copy VE2 XRT RPMs into a CSR run directory for board-side dnf install.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.defaults.sh
source "${SCRIPT_DIR}/config.defaults.sh"

usage() {
  cat <<'EOF'
Usage: stage_ve2_rpms.sh --dest DIR [--source DIR]

  --dest DIR     Destination rpms/ directory under the CSR sprite run
  --source DIR   Yocto RPM deploy directory (default: ${VE2_YOCTO_DIR}/build/tmp/deploy/rpm/${VE2_RPM_DEPLOY_SUBDIR})
EOF
}

DEST=""
SOURCE=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dest)
      DEST="$2"
      shift 2
      ;;
    --source)
      SOURCE="$2"
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

if [ -z "${DEST}" ]; then
  echo "Missing required --dest" >&2
  usage >&2
  exit 1
fi

if [ -z "${SOURCE}" ]; then
  if [ -z "${VE2_YOCTO_DIR:-}" ]; then
    echo "Set VE2_YOCTO_DIR or pass --source" >&2
    exit 1
  fi
  SOURCE="${VE2_YOCTO_DIR}/build/tmp/deploy/rpm/${VE2_RPM_DEPLOY_SUBDIR}"
fi

if [ ! -d "${SOURCE}" ]; then
  echo "RPM source directory not found: ${SOURCE}" >&2
  exit 1
fi

shopt -s nullglob
RPMS=( "${SOURCE}"/${VE2_RPM_GLOB} )
shopt -u nullglob

if [ "${#RPMS[@]}" -eq 0 ]; then
  echo "No RPMs matching ${VE2_RPM_GLOB} under ${SOURCE}" >&2
  exit 1
fi

mkdir -p "${DEST}"
rm -f "${DEST}"/*.rpm
cp -f "${RPMS[@]}" "${DEST}/"
chmod -R a+rX "${DEST}"
chmod a+w "${DEST}"

echo "Staged ${#RPMS[@]} RPM(s) to ${DEST}:"
ls -1 "${DEST}"/*.rpm
