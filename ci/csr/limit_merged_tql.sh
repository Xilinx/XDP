#!/bin/bash
# Limit merged CSR TQL to a single testcase directory (trial/smoke runs).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.defaults.sh
source "${SCRIPT_DIR}/config.defaults.sh"

usage() {
  cat <<'EOF'
Usage: limit_merged_tql.sh --sprite-dir DIR [--test NAME]

Environment:
  XDP_CSR_SINGLE_TEST   Test subdirectory name under XDP_Suites/test_repo
                        (default: ResNet18_MLTimeline). Set empty to run all tests.
EOF
}

SPRITE_DIR=""
SINGLE_TEST="${XDP_CSR_SINGLE_TEST:-ResNet18_MLTimeline}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --sprite-dir)
      SPRITE_DIR="$2"
      shift 2
      ;;
    --test)
      SINGLE_TEST="$2"
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
  exit 1
fi

if [ -z "${SINGLE_TEST}" ]; then
  echo "XDP_CSR_SINGLE_TEST unset; running full suite TQL unchanged"
  exit 0
fi

shopt -s nullglob
TQL_FILES=( "${SPRITE_DIR}"/*"${CSR_SUITE}"*.tql )
shopt -u nullglob

if [ "${#TQL_FILES[@]}" -eq 0 ]; then
  echo "No merged TQL for suite ${CSR_SUITE} under ${SPRITE_DIR}" >&2
  exit 1
fi

TQL_FILE="${TQL_FILES[0]}"

python3 - <<'PY' "${TQL_FILE}" "${SINGLE_TEST}"
import re
import sys

tql_path, single_test = sys.argv[1:3]
text = open(tql_path, encoding="utf-8").read()

# Split trailing GLOBAL APPLY blocks (e.g. XRT RPM patch) from query blocks.
global_idx = text.find("\nGLOBAL APPLY")
if global_idx == -1:
    body, tail = text, ""
else:
    body, tail = text[:global_idx], text[global_idx:]

# Each testcase query starts at a line beginning with FROM.
parts = re.split(r"(?m)^(?=FROM )", body)
header = parts[0] if parts and not parts[0].lstrip().startswith("FROM ") else ""
queries = [p for p in parts if p.lstrip().startswith("FROM ")]

kept = [q for q in queries if single_test in q.splitlines()[0]]
if not kept:
    available = [q.splitlines()[0].strip() for q in queries]
    raise SystemExit(
        f"No FROM block matches test '{single_test}'.\n"
        f"Available FROM lines:\n  " + "\n  ".join(available)
    )

new_body = header + "".join(kept)
open(tql_path, "w", encoding="utf-8").write(new_body + tail)
print(f"Limited {tql_path} to 1 test: {single_test}")
print(f"  kept FROM: {kept[0].splitlines()[0].strip()}")
print(f"  removed {len(queries) - len(kept)} other FROM block(s)")
PY
