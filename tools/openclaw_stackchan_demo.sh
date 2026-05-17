#!/usr/bin/env bash
# Smoke test for the v0.3.0 StackChan firmware.
# Exercises display, face, LED effects, motion choreography, mic, sensors.
set -euo pipefail

STACKCHAN_URL="${STACKCHAN_URL:-http://stackchan.local}"
STACKCHAN_TOKEN="${STACKCHAN_TOKEN:-}"
MESSAGE="${1:-你好，我是 StackChan，来自 OpenClaw。}"

auth_header=""
[[ -n "$STACKCHAN_TOKEN" ]] && auth_header="X-StackChan-Token: $STACKCHAN_TOKEN"

call() {
  local method="$1" path="$2"; shift 2
  local args=(-sS -m 6 --noproxy '*')
  [[ -n "$auth_header" ]] && args+=(-H "$auth_header")
  if [[ "$method" != "GET" ]]; then
    args+=(-H 'Content-Type: application/json' -X "$method")
  fi
  curl "${args[@]}" "$STACKCHAN_URL$path" "$@"
  echo
}

echo "==> status"; call GET /status
echo "==> battery"; call GET /battery

echo "==> face happy"; call POST /display/face -d '{"expression":"happy"}'
echo "==> LEDs rainbow"; call POST /led/effect -d '{"name":"rainbow","speed_ms":50}'
echo "==> dance"; call POST /action -d '{"name":"dance"}'

sleep 2
echo "==> long message"; call POST /display \
  -d "{\"title\":\"OpenClaw\",\"text\":\"$MESSAGE\"}"
echo "==> nod"; call POST /action -d '{"name":"nod"}'

sleep 3
echo "==> face love + breathing LED"; call POST /display/face -d '{"expression":"love"}'
call POST /led/effect -d '{"name":"breathing","r":255,"g":40,"b":120,"speed_ms":40}'

sleep 2
echo "==> idle motion on"; call POST /idle -d '{"enabled":true}'
sleep 8
echo "==> idle motion off"; call POST /idle -d '{"enabled":false}'

echo "==> mic record 3s (no upload)"; call POST /mic/record -d '{"seconds":3}'
sleep 5
echo "==> mic status"; call GET /mic/status

echo "==> sensors"; call GET /sensors
echo "==> events"; call GET "/events?since=0"

echo "==> camera status"; call GET /camera/status
echo "==> camera capture (QVGA → /tmp/stackchan.jpg)"
out=$(curl --noproxy '*' -sS -m 30 -o /tmp/stackchan.jpg -w 'h=%{http_code} b=%{size_download} t=%{time_total}\n' "$STACKCHAN_URL/camera/capture")
echo "  $out"
file /tmp/stackchan.jpg 2>/dev/null || true

echo "==> return to status screen"; call POST /display/status
call POST /led/effect -d '{"name":"breathing","r":0,"g":120,"b":255,"speed_ms":40}'
call POST /home
