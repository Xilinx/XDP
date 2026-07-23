#!/bin/bash
# End-to-end VE2 board test via CSR for one XDP PR build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.defaults.sh
source "${SCRIPT_DIR}/config.defaults.sh"

usage() {
  cat <<'EOF'
Usage: run_ve2_board_test.sh --run-id ID [--rpm-source DIR]

Environment:
  CSR_ROOT                 Base NFS workspace (default: /wrk/csr/snigupta/xdp_ci)
  VE2_YOCTO_DIR            Yocto workspace used by build-xrt-ve2
  JENKINS_USER             Required for Jenkins submission
  JENKINS_API_TOKEN        Required for Jenkins submission
  XDP_CSR_SKIP_JENKINS=1   Generate CSR only; do not submit
  XDP_CSR_SUBMIT_MODE      local (default) or jenkins
EOF
}

RUN_ID=""
RPM_SOURCE=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --run-id)
      RUN_ID="$2"
      shift 2
      ;;
    --rpm-source)
      RPM_SOURCE="$2"
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

if [ -z "${RUN_ID}" ]; then
  echo "Missing required --run-id" >&2
  usage >&2
  exit 1
fi

SPRITE_DIR="${CSR_ROOT}/pr-${RUN_ID}/sprite"
RPM_DIR="${CSR_ROOT}/pr-${RUN_ID}/rpms"
DESCRIPTION="${XDP_CSR_DESCRIPTION:-XDP PR ${RUN_ID} VE2_XDP board test}"

mkdir -p "${CSR_ROOT}/pr-${RUN_ID}"
chmod a+w "${CSR_ROOT}" "${CSR_ROOT}/pr-${RUN_ID}" 2>/dev/null || true

STAGE_ARGS=( --dest "${RPM_DIR}" )
if [ -n "${RPM_SOURCE}" ]; then
  STAGE_ARGS+=( --source "${RPM_SOURCE}" )
fi

echo "== Stage VE2 XRT RPMs =="
bash "${SCRIPT_DIR}/stage_ve2_rpms.sh" "${STAGE_ARGS[@]}"

echo "== Generate CSR sprite run =="
bash "${SCRIPT_DIR}/generate_csr_run.sh" --sprite-dir "${SPRITE_DIR}"

echo "== Patch merged TQL for custom XRT install =="
bash "${SCRIPT_DIR}/patch_merged_tql.sh" \
  --sprite-dir "${SPRITE_DIR}" \
  --rpm-dir "${RPM_DIR}"

if [ "${XDP_CSR_SKIP_JENKINS:-0}" = "1" ]; then
  echo "XDP_CSR_SKIP_JENKINS=1; CSR artifacts ready at ${SPRITE_DIR}"
  exit 0
fi

SUBMIT_MODE="${XDP_CSR_SUBMIT_MODE:-local}"
case "${SUBMIT_MODE}" in
  jenkins)
    echo "== Submit CSR run to Jenkins =="
    bash "${SCRIPT_DIR}/submit_csr_jenkins.sh" \
      --sprite-dir "${SPRITE_DIR}" \
      --description "${DESCRIPTION}"
    ;;
  local)
    echo "== Submit CSR run locally (RDI) =="
    bash "${SCRIPT_DIR}/submit_csr_local.sh" \
      --sprite-dir "${SPRITE_DIR}"
    ;;
  *)
    echo "Unknown XDP_CSR_SUBMIT_MODE=${SUBMIT_MODE} (use local or jenkins)" >&2
    exit 1
    ;;
esac

echo "VE2 CSR board test completed for run ${RUN_ID}"
