#!/bin/bash
# Prepare a persistent Telluride + Yocto workspace on the self-hosted runner.
# Skips clone + create_yocto.sh -i when cache is younger than VE2_YOCTO_CACHE_TTL_DAYS.
set -euo pipefail

CACHE_ROOT="${VE2_YOCTO_CACHE_ROOT:-/scratch/snigupta/ve2_yocto_cache}"
CACHE_TTL_DAYS="${VE2_YOCTO_CACHE_TTL_DAYS:-7}"
TELLURIDE_REPO="${TELLURIDE_REPO:-IPS-SSW/Vitis-AI-Telluride.git}"
GITENTERPRISE_HOST="${GITENTERPRISE_HOST:-gitenterprise.xilinx.com}"
YOCTO_REL="${VE2_YOCTO_REL:-versal_2ve/reference_design/vek385/rev-b/sw/yocto}"
FORCE_REFRESH="${VE2_FORCE_CACHE_REFRESH:-false}"

TELLURIDE_DIR="${CACHE_ROOT}/Vitis-AI-Telluride"
YOCTO_DIR="${TELLURIDE_DIR}/${YOCTO_REL}"
MARKER="${CACHE_ROOT}/.initialized"

cache_is_valid() {
  [ "${FORCE_REFRESH}" = "true" ] && return 1
  [ -f "${MARKER}" ] || return 1
  [ -d "${YOCTO_DIR}/build" ] || return 1
  [ -f "${YOCTO_DIR}/edf-init-build-env" ] || return 1

  local now marker_age_sec age_days
  now="$(date +%s)"
  marker_age_sec=$((now - $(stat -c %Y "${MARKER}")))
  age_days=$((marker_age_sec / 86400))
  echo "Cache age: ${age_days} day(s) (ttl ${CACHE_TTL_DAYS} day(s))"
  [ "${age_days}" -lt "${CACHE_TTL_DAYS}" ]
}

mkdir -p "${CACHE_ROOT}"

if cache_is_valid; then
  echo "Reusing cached Yocto workspace at ${YOCTO_DIR}"
else
  if [ "${FORCE_REFRESH}" = "true" ]; then
    echo "Force refresh: rebuilding Yocto cache at ${CACHE_ROOT}"
  else
    echo "Cache missing or stale: initializing Yocto workspace at ${CACHE_ROOT}"
  fi

  rm -rf "${TELLURIDE_DIR}"
  git clone --recurse-submodules \
    "https://${GITENTERPRISE_HOST}/${TELLURIDE_REPO}" \
    "${TELLURIDE_DIR}"

  cd "${YOCTO_DIR}"
  chmod +x create_yocto.sh
  ./create_yocto.sh -i
  touch "${MARKER}"
  date -u +"%Y-%m-%dT%H:%M:%SZ" > "${CACHE_ROOT}/.initialized_at"
fi

if [ -n "${GITHUB_ENV:-}" ]; then
  echo "YOCTO_DIR=${YOCTO_DIR}" >> "${GITHUB_ENV}"
fi

echo "${YOCTO_DIR}"
