#!/bin/bash
# Launch CSR RDI directly on XCO and wait for completion (no Jenkins API).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.defaults.sh
source "${SCRIPT_DIR}/config.defaults.sh"

usage() {
  cat <<'EOF'
Usage: submit_csr_local.sh --sprite-dir DIR

Runs the generated *_rdi.sh in SPRITE_DIR and polls the RDI log until complete.
Requires LSF permission and RDI env on the self-hosted runner host.
EOF
}

SPRITE_DIR=""
POLL_SEC="${CSR_LOCAL_POLL_SEC:-60}"
TIMEOUT_SEC="${CSR_WAIT_TIMEOUT_SEC:-14400}"

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

if [ -z "${SPRITE_DIR}" ] || [ ! -d "${SPRITE_DIR}" ]; then
  echo "Missing or invalid --sprite-dir" >&2
  exit 1
fi

shopt -s nullglob
RDI_SCRIPTS=( "${SPRITE_DIR}"/*_rdi.sh )
shopt -u nullglob

if [ "${#RDI_SCRIPTS[@]}" -eq 0 ]; then
  echo "No *_rdi.sh found under ${SPRITE_DIR}" >&2
  exit 1
fi

RDI_SCRIPT="${RDI_SCRIPTS[0]}"
RESULTS_DIR="${SPRITE_DIR}/results"
mkdir -p "${RESULTS_DIR}"
chmod a+w "${SPRITE_DIR}" "${RESULTS_DIR}" "$(dirname "${SPRITE_DIR}")" 2>/dev/null || true

echo "Launching CSR RDI locally: ${RDI_SCRIPT}"
cd "${SPRITE_DIR}"

# setRDIEnv.csh is tcsh; sourcing it from bash does not put rdi on PATH.
tcsh -f <<EOF
source /proj/xtools/dsv/rdi2/utils/setRDIEnv.csh
bash ${RDI_SCRIPT}
EOF

mapfile -t LOG_FILES < <(find "${RESULTS_DIR}" -maxdepth 1 -name 'rdi_*.log' -type f | sort)
if [ "${#LOG_FILES[@]}" -eq 0 ]; then
  echo "No rdi_*.log files found under ${RESULTS_DIR}" >&2
  exit 1
fi

echo "Monitoring ${#LOG_FILES[@]} RDI log(s):"
printf '  %s\n' "${LOG_FILES[@]}"

for log in "${LOG_FILES[@]}"; do
  if grep -q 'command not found' "${log}" 2>/dev/null; then
    echo "RDI launch failed in ${log}:" >&2
    cat "${log}" >&2
    exit 1
  fi
done

deadline=$(( $(date +%s) + TIMEOUT_SEC ))
pending=("${LOG_FILES[@]}")
failed=0

while [ "${#pending[@]}" -gt 0 ]; do
  now=$(date +%s)
  if [ "${now}" -ge "${deadline}" ]; then
    echo "Timed out after ${TIMEOUT_SEC}s waiting for RDI logs" >&2
    exit 1
  fi

  still_pending=()
  for log in "${pending[@]}"; do
    if grep -q 'Finished at:' "${log}" 2>/dev/null; then
      echo "Completed: ${log}"
      if grep -Eiq 'ERROR|CRITICAL WARNING|RDI regression failed|exit code [1-9]' "${log}"; then
        echo "Failure indicators found in ${log}" >&2
        failed=1
      fi
    else
      still_pending+=( "${log}" )
      if [ "$(wc -c < "${log}")" -lt 512 ] && \
         find "${log}" -mmin +2 2>/dev/null | grep -q .; then
        echo "RDI log stalled (no progress): ${log}" >&2
        cat "${log}" >&2
        exit 1
      fi
      echo "Still running: ${log}"
    fi
  done

  pending=( "${still_pending[@]}" )
  if [ "${#pending[@]}" -gt 0 ]; then
    sleep "${POLL_SEC}"
  fi
done

if [ "${failed}" -ne 0 ]; then
  echo "One or more RDI runs reported failures" >&2
  exit 1
fi

# RDI can finish cleanly while individual tests fail; check suite summaries.
check_suite_test_results() {
  local results_dir="$1"
  local summary fail_count stats line
  local -a summaries=()
  local saw_summary=0
  local saw_failure=0

  shopt -s nullglob
  summaries=( "${results_dir}"/*/*.summary "${results_dir}"/*.summary )
  shopt -u nullglob

  if [ "${#summaries[@]}" -eq 0 ]; then
    echo "No CSR suite .summary files found under ${results_dir}" >&2
    return 1
  fi

  for summary in "${summaries[@]}"; do
    [ -f "${summary}" ] || continue
    saw_summary=1
    stats="$(sed -n '2p' "${summary}")"
    fail_count="$(awk '{print ($3 ~ /^[0-9]+$/) ? $3 : 0}' <<< "${stats}")"
    if [ "${fail_count:-0}" -gt 0 ]; then
      saw_failure=1
      echo "CSR test failure(s) reported in ${summary}" >&2
      awk '/^ishitag:/,0' "${summary}" >&2 || true
    fi
  done

  if [ "${saw_summary}" -eq 0 ]; then
    echo "No readable CSR suite .summary files under ${results_dir}" >&2
    return 1
  fi

  if [ "${saw_failure}" -ne 0 ]; then
    while IFS= read -r line; do
      echo "XOAH: ${line}" >&2
    done < <(grep -rh 'xoah_report_url:' "${results_dir}" 2>/dev/null | sed 's/.*xoah_report_url:[[:space:]]*//' | sort -u)
    return 1
  fi

  return 0
}

if ! check_suite_test_results "${RESULTS_DIR}"; then
  echo "CSR suite reported test failure(s); see summary and XOAH links above" >&2
  exit 1
fi

echo "All RDI runs completed successfully"
