#!/bin/bash
# Submit a CSR sprite directory to Jenkins csr_submit_job and wait for completion.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.defaults.sh
source "${SCRIPT_DIR}/config.defaults.sh"

usage() {
  cat <<'EOF'
Usage: submit_csr_jenkins.sh --sprite-dir DIR [--description TEXT]

Requires:
  JENKINS_USER
  JENKINS_API_TOKEN

Optional:
  JENKINS_URL (default: http://acasops:8080)
  CSR_WAIT_TIMEOUT_SEC (default: 14400)
EOF
}

SPRITE_DIR=""
DESCRIPTION=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --sprite-dir)
      SPRITE_DIR="$2"
      shift 2
      ;;
    --description)
      DESCRIPTION="$2"
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

if [ ! -d "${SPRITE_DIR}" ]; then
  echo "Sprite directory not found: ${SPRITE_DIR}" >&2
  exit 1
fi

if [ -z "${JENKINS_USER:-}" ] || [ -z "${JENKINS_API_TOKEN:-}" ]; then
  echo "Set JENKINS_USER and JENKINS_API_TOKEN to submit ${JENKINS_JOB}" >&2
  exit 1
fi

chmod a+w "${SPRITE_DIR}" "$(dirname "${SPRITE_DIR}")" 2>/dev/null || true

if [ -z "${DESCRIPTION}" ]; then
  DESCRIPTION="XDP VE2 CSR: ${SPRITE_DIR}"
fi

python3 - <<'PY' "${JENKINS_URL}" "${JENKINS_JOB}" "${SPRITE_DIR}" "${DESCRIPTION}" \
  "${JENKINS_LNX_TA}" "${JENKINS_COMPILE_JOBS_ONLY}" "${JENKINS_BOARD_SELECT}" \
  "${JENKINS_USER}" "${JENKINS_API_TOKEN}" "${CSR_WAIT_TIMEOUT_SEC}"
import base64
import http.cookiejar
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

(
    jenkins_url,
    job_name,
    sprite_dir,
    description,
    lnx_ta,
    compile_jobs_only,
    board_select,
    user,
    token,
    timeout_sec,
) = sys.argv[1:11]
user = user.strip()
token = token.strip()
timeout_sec = int(timeout_sec)
base = jenkins_url.rstrip("/")
auth = base64.b64encode(f"{user}:{token}".encode()).decode()

cookie_jar = http.cookiejar.CookieJar()
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cookie_jar))


def request(url, method="GET", data=None, extra_headers=None):
    headers = {
        "Authorization": f"Basic {auth}",
        "Accept": "application/json",
    }
    if extra_headers:
        headers.update(extra_headers)
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with opener.open(req, timeout=120) as resp:
            body = resp.read().decode(errors="replace")
            return resp, body
    except urllib.error.HTTPError as exc:
        body = exc.read().decode(errors="replace")
        snippet = " ".join(body.split())[:500]
        raise SystemExit(
            f"Jenkins HTTP {exc.code} for {method} {url}\n{snippet}"
        ) from exc


def request_json(url, method="GET", data=None, extra_headers=None):
    _, body = request(url, method=method, data=data, extra_headers=extra_headers)
    return json.loads(body)


whoami = request_json(f"{base}/me/api/json")
print(f"Jenkins user: {whoami.get('fullName', whoami.get('id', user))}")

crumb = request_json(f"{base}/crumbIssuer/api/json")
crumb_field = crumb["crumbRequestField"]
crumb_value = crumb["crumb"]

params = {
    "SPRITE_DIR": sprite_dir,
    "DESCRIPTION": description[:256],
    "LNX_TA": lnx_ta,
    "WIN_TA": "DEFAULT",
    "COMPILE_JOBS_ONLY": compile_jobs_only,
    "LINUX_COMPILE": "False",
    "BOARD_SELECT_RESOURCE": board_select,
    "VE2_DOCKER": "False",
    "VE2_BOOT_IMAGE_PATH": "DEFAULT",
}
trigger_url = f"{base}/job/{job_name}/buildWithParameters"
form_body = urllib.parse.urlencode(params).encode()

trigger_resp, _ = request(
    trigger_url,
    method="POST",
    data=form_body,
    extra_headers={
        crumb_field: crumb_value,
        "Content-Type": "application/x-www-form-urlencoded",
    },
)
queue_url = trigger_resp.headers.get("Location")
if not queue_url:
    raise SystemExit("Jenkins did not return a queue Location header")

print(f"Queued Jenkins job: {queue_url.rstrip('/')}/api/json")

build_url = None
deadline = time.time() + min(timeout_sec, 600)
while time.time() < deadline:
    data = request_json(f"{queue_url.rstrip('/')}/api/json")
    if data.get("executable"):
        build_url = data["executable"]["url"]
        break
    if data.get("cancelled"):
        raise SystemExit("Jenkins queue item was cancelled")
    time.sleep(5)

if not build_url:
    raise SystemExit("Timed out waiting for Jenkins to start the build")

print(f"Jenkins build started: {build_url}")

deadline = time.time() + timeout_sec
result = None
while time.time() < deadline:
    data = request_json(f"{build_url.rstrip('/')}/api/json")
    if not data.get("building") and data.get("result") is not None:
        result = data["result"]
        break
    time.sleep(60)

if result is None:
    raise SystemExit(f"Timed out waiting for Jenkins build after {timeout_sec}s: {build_url}")

print(f"Jenkins result: {result}")
print(f"Console: {build_url}console")

_, console = request(f"{build_url.rstrip('/')}/consoleText")
for line in console.splitlines():
    if "Check XOAH Link for details:" in line:
        print(line.strip())
    if "All (" in line and "runs finished at:" in line:
        print(line.strip())

if result != "SUCCESS":
    raise SystemExit(1)
PY

echo "Jenkins ${JENKINS_JOB} completed successfully"
