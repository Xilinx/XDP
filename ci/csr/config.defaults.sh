# Defaults for XDP VE2 CSR board testing on XCO.
# Override any value via environment before calling the ci/csr scripts.

: "${CSR_ROOT:=/wrk/csr/snigupta/xdp_ci}"
: "${CSR_SPEC:=/proj/testcases/xtc/HEAD/auto/ymls/config/VAI_6_3_GEN2_REGRESSION.yml}"
: "${CSR_DAY:=sunday}"
: "${CSR_SUITE:=VE2_XDP}"
: "${CSR_TESTMODE:=hw}"
: "${CSR_TQL_REPO:=/proj/testcases/xtc/HEAD/auto/tql/vaiml/}"
: "${CSR_TA:=/proj/xbuilds/HEAD_INT_flexml_verified_vitis/installs}"
: "${CSR_CREATE_SPRITE_RUN:=/proj/testcases/xtc/HEAD/auto/scripts/createSpriteRun/createSpriteRun.sh}"

# VE2_XDP is commented out in VAI_6_2 runs.yml; it is active under eachday in 6.3.
: "${CSR_SUPER_SUITE:=VAI_6_3_GEN2_REGRESSION}"

: "${VE2_RPM_DEPLOY_SUBDIR:=cortexa72_cortexa53}"
: "${VE2_RPM_GLOB:=xrt*.rpm}"
: "${VE2_DNF_PACKAGES:=xrt-20261*.rpm xrt-dbg*.rpm xrt-src*.rpm}"

: "${JENKINS_URL:=http://acasops:8080}"
: "${JENKINS_JOB:=csr_submit_job}"
: "${JENKINS_BOARD_SELECT:=VE2}"
: "${JENKINS_COMPILE_JOBS_ONLY:=False}"
: "${JENKINS_LNX_TA:=DEFAULT}"

: "${CSR_WAIT_TIMEOUT_SEC:=14400}"
: "${XDP_CSR_SUBMIT_MODE:=local}"
