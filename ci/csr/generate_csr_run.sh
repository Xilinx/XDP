#!/bin/bash
# Generate CSR sprite files for a VE2_XDP hw run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.defaults.sh
source "${SCRIPT_DIR}/config.defaults.sh"

usage() {
  cat <<'EOF'
Usage: generate_csr_run.sh --sprite-dir DIR

Creates CSR merged TQL and launch scripts under --sprite-dir.
EOF
}

SPRITE_DIR=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --sprite-dir)
      SPRITE_DIR="$2"
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

mkdir -p "${SPRITE_DIR}/results"
chmod a+w "${SPRITE_DIR}" "${SPRITE_DIR}/results"
chmod a+w "$(dirname "${SPRITE_DIR}")" 2>/dev/null || true

# createSpriteRunLaunch sets up RDI/XTC. Do not source g_profile here: under bash
# nounset it fails when LD_LIBRARY_PATH is unset (common in GHA runner env).
umask 022

cd "${SPRITE_DIR}"

"${CSR_CREATE_SPRITE_RUN}" \
  --day "${CSR_DAY}" \
  --spec "${CSR_SPEC}" \
  --results "${SPRITE_DIR}/results" \
  --flexml \
  --tql-repo "${CSR_TQL_REPO}" \
  --xtc-no-lut \
  --testsuites "${CSR_SUITE}" \
  --testmode "${CSR_TESTMODE}" \
  --ta "${CSR_TA}"

RDI_SCRIPT="${SPRITE_DIR}/${CSR_SUPER_SUITE}_${CSR_DAY}_rdi.sh"
if [ ! -s "${RDI_SCRIPT}" ]; then
  echo "CSR generation did not produce a non-empty RDI script: ${RDI_SCRIPT}" >&2
  ls -la "${SPRITE_DIR}" >&2 || true
  exit 1
fi

chmod -R a+w "${SPRITE_DIR}" 2>/dev/null || true
echo "Generated CSR run in ${SPRITE_DIR}"
ls -1 "${SPRITE_DIR}"/*"${CSR_SUITE}"*.tql
echo "Launch script: ${RDI_SCRIPT}"
