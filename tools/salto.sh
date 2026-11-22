#!/usr/bin/env bash
# salto.sh - build, OTA and remote debugging for salto_bridge.
# Runs from Git Bash / Linux / macOS. Secrets are read from environment variables,
# otherwise from salto_bridge/config.h. They are never printed.
set -euo pipefail

usage() {
  cat <<'EOF'
tools/salto.sh build [version]     compile into build/ (version: git hash or date)
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

version_string() {
  if [ -n "${1:-}" ]; then echo "$1"; return; fi
  if git -C "$ROOT" rev-parse --short HEAD >/dev/null 2>&1; then
    local v; v="$(git -C "$ROOT" rev-parse --short HEAD)"
    git -C "$ROOT" diff --quiet HEAD -- 2>/dev/null || v="$v-dirty-$(date +%H%M%S)"
    echo "$v"
  else
    date +%Y%m%d-%H%M%S
  fi
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
