# Sourced by the end-to-end scripts. e2e_relay_start <work-dir> sets RELAY_URL.
#
# Without RELAY_URL it starts a local moq5-relay (draft 22) on loopback with a
# throwaway certificate, so a run publishes to no public service. It stops with
# the script when the script's cleanup kills "${pids[@]}". With RELAY_URL set,
# that relay is used as is.
#
# Environment:
#   RELAY_URL                  an existing relay, e.g. moqt://127.0.0.1:4433;
#                              skips the local relay
#   MOQ5_RELAY                 the moq5-relay binary (needed without RELAY_URL)
#   MOQ5_RELAY_LD_LIBRARY_PATH library path the relay needs, e.g. the MsQuic
#                              build directory (optional)
#   RELAY_PORT                 UDP port of the local relay (default: a free one)

e2e_relay_start() {
  local work="$1"
  if [ -n "${RELAY_URL:-}" ]; then
    echo "using relay $RELAY_URL"
    return 0
  fi
  : "${MOQ5_RELAY:?set RELAY_URL to a draft-22 relay, or MOQ5_RELAY to the moq5-relay binary to start a local one}"
  command -v openssl > /dev/null || { echo "missing required tool: openssl" >&2; return 1; }
  local port="${RELAY_PORT:-}"
  if [ -z "$port" ]; then
    port="$(python3 -c 'import socket; s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')"
  fi
  local dir="$work/relay"
  mkdir -p "$dir"
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout "$dir/key.pem" -out "$dir/cert.pem" -days 2 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" 2> /dev/null
  cat > "$dir/relay.json" <<JSON
{
  "listener": { "host": "127.0.0.1", "port": $port, "versions": [22], "lanes": 1,
                "cert": "$dir/cert.pem", "key": "$dir/key.pem" },
  "auth": { "mode": "allow_all" }
}
JSON
  LD_LIBRARY_PATH="${MOQ5_RELAY_LD_LIBRARY_PATH:+$MOQ5_RELAY_LD_LIBRARY_PATH:}${LD_LIBRARY_PATH:-}" \
    "$MOQ5_RELAY" serve --config "$dir/relay.json" > "$dir/relay.log" 2>&1 &
  local relay=$!
  pids+=("$relay")
  local i
  for i in $(seq 1 50); do
    grep -q "listening on 127.0.0.1:$port" "$dir/relay.log" 2> /dev/null && break
    kill -0 "$relay" 2> /dev/null || { echo "the local relay exited at start-up:" >&2; tail -n 8 "$dir/relay.log" >&2; return 1; }
    sleep 0.2
  done
  grep -q "listening on 127.0.0.1:$port" "$dir/relay.log" || { echo "the local relay did not start" >&2; return 1; }
  RELAY_URL="moqt://127.0.0.1:$port"
  echo "started a local relay on $RELAY_URL"
}
