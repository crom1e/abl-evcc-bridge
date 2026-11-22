#!/usr/bin/env bash
# salto.sh - build, OTA and remote debugging for salto_bridge.
# Runs from Git Bash / Linux / macOS. Secrets are read from environment variables,
# otherwise from salto_bridge/config.h. They are never printed.
set -euo pipefail

usage() {
  cat <<'EOF'
tools/salto.sh build [version]     compile into build/ (version from manifest.json, see 'version')
tools/salto.sh version             print the version the next build gets (X.Y.Z or X.Y.Z-dev.<hash>)
tools/salto.sh release X.Y.Z       set version, commit, tag vX.Y.Z, push and create a GitHub release
tools/salto.sh ota                 build + upload via arduino-cli/espota (ArduinoOTA)
tools/salto.sh ota-http            build + upload build/*.bin via curl to /update
tools/salto.sh verify [version]    wait until /api/status reports the version (default: last build)
tools/salto.sh status | log | anomalies
tools/salto.sh anomalies-clear    clear the persistent anomaly log
tools/salto.sh current <A>         set charging current (6..MAX_A) - affects a real charger!
tools/salto.sh level <debug|info|warn> | reboot
tools/salto.sh telnet "<cmd>" ...  log in over telnet, run commands, print output
tools/salto.sh tail [seconds]      stream the telnet log (default 30 s)
tools/salto.sh ip                  resolve the bridge's IP address

Environment: SALTO_HOST, SALTO_TOKEN, SALTO_OTA_PASS, SALTO_UPDATE_USER, SALTO_UPDATE_PASS
EOF
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKETCH="$ROOT/salto_bridge"
CONFIG="$SKETCH/config.h"
BUILD="$ROOT/build"
FQBN="esp8266:esp8266:d1_mini"

cfg() {  # cfg NAME -> value of '#define NAME "..."' in config.h
  [ -f "$CONFIG" ] || { echo "missing $CONFIG" >&2; exit 1; }
  sed -nE "s/^[[:space:]]*#define[[:space:]]+$1[[:space:]]+\"(.*)\".*/\1/p" "$CONFIG" | head -1
}

HOST="${SALTO_HOST:-$(cfg HOSTNAME 2>/dev/null || echo salto).local}"
TOKEN="${SALTO_TOKEN:-$(cfg API_TOKEN)}"
OTA_PASS="${SALTO_OTA_PASS:-$(cfg OTA_PASSWORD)}"
UPD_USER="${SALTO_UPDATE_USER:-$(cfg UPDATE_USER)}"
UPD_PASS="${SALTO_UPDATE_PASS:-$(cfg UPDATE_PASS)}"

find_cli() {
  if command -v arduino-cli >/dev/null 2>&1; then echo arduino-cli; return; fi
  for p in "/c/Program Files/Arduino CLI/arduino-cli.exe" "/usr/local/bin/arduino-cli"; do
    [ -x "$p" ] && { echo "$p"; return; }
  done
  echo "arduino-cli not found" >&2; exit 1
}
CLI="$(find_cli)"

arduino_data() {
  "$CLI" config get directories.data 2>/dev/null | tr -d '\r'
}

find_python() {
  local d; d="$(arduino_data)"
  local p
  p="$(ls "$d"/packages/esp8266/tools/python3/*/python3.exe "$d"/packages/esp8266/tools/python3/*/python3 2>/dev/null | head -1 || true)"
  if [ -n "$p" ]; then echo "$p"; return; fi
  command -v python3 || command -v python || { echo "python not found" >&2; exit 1; }
}

resolve_ip() {  # mDNS name -> IP (curl/python on Windows cannot resolve .local; ping can)
  if [[ "$HOST" =~ ^[0-9.]+$ ]]; then echo "$HOST"; return; fi
  local ip=""
  ip="$(ping -n 1 -w 2000 "$HOST" 2>/dev/null | sed -nE 's/.*\[([0-9.]+)\].*/\1/p' | head -1 || true)"
  [ -z "$ip" ] && ip="$(ping -c 1 -W 2 "$HOST" 2>/dev/null | sed -nE 's/.*\(([0-9.]+)\).*/\1/p' | head -1 || true)"
  if [ -z "$ip" ]; then
    local py; py="$(find_python)"
    ip="$("$py" -I -c "import socket,sys;print(socket.gethostbyname(sys.argv[1]))" "$HOST" 2>/dev/null || true)"
  fi
  [ -n "$ip" ] || { echo "could not resolve $HOST - set SALTO_HOST=<ip>" >&2; exit 1; }
  echo "$ip"
}

MANIFEST="$ROOT/custom_components/salto_bridge/manifest.json"

# The single version number for firmware and integration lives in manifest.json.
manifest_version() {
  sed -nE 's/^[[:space:]]*"version":[[:space:]]*"([^"]+)".*/\1/p' "$MANIFEST" | head -1
}

# Firmware version string:
#   X.Y.Z                         clean tree, HEAD tagged vX.Y.Z (a release)
#   X.Y.Z-dev.<hash>              between releases
#   X.Y.Z-dev.<hash>-dirty.HHMMSS uncommitted changes
version_string() {
  if [ -n "${1:-}" ]; then echo "$1"; return; fi
  local base; base="$(manifest_version)"
  [ -n "$base" ] || base="0.0.0"
  if ! git -C "$ROOT" rev-parse --short HEAD >/dev/null 2>&1; then
    echo "$base-dev.$(date +%Y%m%d%H%M%S)"; return
  fi
  local hash dirty=""
  hash="$(git -C "$ROOT" rev-parse --short HEAD)"
  git -C "$ROOT" diff --quiet HEAD -- 2>/dev/null || dirty="-dirty.$(date +%H%M%S)"
  if [ -z "$dirty" ] && [ "$(git -C "$ROOT" tag --points-at HEAD | grep -cx "v$base")" = 1 ]; then
    echo "$base"
  else
    echo "$base-dev.$hash$dirty"
  fi
}

find_gh() {
  if command -v gh >/dev/null 2>&1; then echo gh; return; fi
  local p="${LOCALAPPDATA:-}/Programs/gh/bin/gh.exe"
  p="$(cygpath -u "$p" 2>/dev/null || echo "$p")"
  [ -x "$p" ] && { echo "$p"; return; }
  echo "gh (GitHub CLI) not found" >&2; exit 1
}

# release X.Y.Z: set the version in manifest.json, commit, tag vX.Y.Z, push, create a GitHub release.
release() {
  local v="${1:?usage: release X.Y.Z}"
  [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "version must be X.Y.Z" >&2; exit 1; }
  git -C "$ROOT" diff --quiet HEAD -- || { echo "commit or stash your changes first" >&2; exit 1; }
  git -C "$ROOT" rev-parse -q --verify "refs/tags/v$v" >/dev/null && { echo "tag v$v already exists" >&2; exit 1; }
  local gh; gh="$(find_gh)"
  if [ "$(manifest_version)" != "$v" ]; then
    sed -i -E "s/^([[:space:]]*\"version\":[[:space:]]*\")[^\"]+(\".*)/\1$v\2/" "$MANIFEST"
    git -C "$ROOT" add "$MANIFEST"
    git -C "$ROOT" commit -q -m "Release $v"
  fi
  git -C "$ROOT" tag -a "v$v" -m "Release $v"
  git -C "$ROOT" push -q
  git -C "$ROOT" push -q origin "v$v"
  (cd "$ROOT" && "$gh" release create "v$v" --title "$v" --generate-notes)
  echo ">> released $v - build and flash with: tools/salto.sh ota-http && tools/salto.sh verify"
}

build() {
  local ver; ver="$(version_string "${1:-}")"
  echo ">> building FW_VERSION=$ver"
  "$CLI" compile --fqbn "$FQBN" --output-dir "$BUILD" \
    --build-property "compiler.cpp.extra_flags=-DFW_VERSION_RAW=$ver" "$SKETCH"
  echo "$ver" > "$BUILD/version.txt"
  echo ">> $BUILD/salto_bridge.ino.bin ($ver)"
}

ota() {
  build "${1:-}"
  local ip; ip="$(resolve_ip)"
  echo ">> OTA (espota) to $HOST ($ip)"
  if "$CLI" upload -p "$ip" -l network -F "password=$OTA_PASS" --fqbn "$FQBN" --input-dir "$BUILD" "$SKETCH"; then
    return 0
  fi
  echo ">> arduino-cli upload failed - trying espota.py directly"
  local py espota; py="$(find_python)"
  espota="$(ls "$(arduino_data)"/packages/esp8266/hardware/esp8266/*/tools/espota.py | tail -1)"
  "$py" -I "$espota" -i "$ip" -p 8266 "--auth=$OTA_PASS" -f "$BUILD/salto_bridge.ino.bin"
}

ota_http() {
  build "${1:-}"
  echo ">> HTTP OTA to http://$(resolve_ip)/update"
  curl -sS --fail -u "$UPD_USER:$UPD_PASS" -F "firmware=@$BUILD/salto_bridge.ino.bin" "http://$(resolve_ip)/update"
  echo
}

verify() {
  local want="${1:-$(cat "$BUILD/version.txt" 2>/dev/null || true)}"
  [ -n "$want" ] || { echo "no version to check" >&2; exit 1; }
  echo ">> waiting for fw_version=$want"
  local got=""
  for i in $(seq 1 45); do
    got="$(curl -s -m 3 -H "Authorization: Bearer $TOKEN" "http://$(resolve_ip)/api/status" \
      | sed -nE 's/.*"fw_version":"([^"]*)".*/\1/p' || true)"
    if [ "$got" = "$want" ]; then echo ">> OK: running $got"; return 0; fi
    sleep 2
  done
  echo ">> FAILED: did not find $want (last reply: ${got:-none})" >&2
  exit 1
}

api() {  # api METHOD PATH [json]
  local m="$1" p="$2" body="${3:-}"
  if [ -n "$body" ]; then
    curl -sS -m 10 -X "$m" -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d "$body" "http://$(resolve_ip)$p"
  else
    curl -sS -m 10 -X "$m" -H "Authorization: Bearer $TOKEN" "http://$(resolve_ip)$p"
  fi
  echo
}

telnet_py() {
  local py; py="$(find_python)"
  SALTO_TOKEN="$TOKEN" "$py" -I "$ROOT/tools/telnet.py" "$(resolve_ip)" "$@"
}

cmd="${1:-help}"; shift || true
case "$cmd" in
  build)    build "${1:-}" ;;
  ota)      ota "${1:-}" ;;
  ota-http) ota_http "${1:-}" ;;
  verify)   verify "${1:-}" ;;
  version)  version_string ;;
  release)  release "${1:-}" ;;
  status)   api GET /api/status ;;
  log)      api GET /api/log ;;
  anomalies) api GET /api/anomalies ;;
  anomalies-clear) api POST /api/anomalies/clear ;;
  current)  api POST /api/current "{\"amps\": ${1:?amps}}" ;;
  level)    api POST /api/debug/loglevel "{\"level\": \"${1:?level}\"}" ;;
  reboot)   api POST /api/debug/reboot ;;
  telnet)   telnet_py --wait 3 "$@" ;;
  tail)     telnet_py --wait "${1:-30}" ;;
  ip)       resolve_ip ;;
  *)        usage ;;
esac
