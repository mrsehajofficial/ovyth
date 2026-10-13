#!/usr/bin/env bash
# net.* regression: the TCP primitives, and the agent's remote mode.
#
# The agent's remote control is built on four calls (listen/accept/read/write)
# plus a line-assembling function, and everything else -- tools, guard, history
# -- is reused unchanged. So this tests the transport on its own terms: does a
# byte sent arrive, does a line survive a split read, does the port actually
# bind, and does an agent behind it refuse an unauthenticated client.
#
# Two things this suite learned the hard way and now does deliberately:
#
#   * Ports come from the kernel (bind 0), not a hard-coded number. A
#     hard-coded port races with whatever else is listening on the machine, and
#     a collision presents as a broken server rather than a busy port.
#   * The server reports its port through a FILE, not stdout. stdout is
#     block-buffered whenever it is a pipe, so a server blocked in accept()
#     holds its "PORT 12345" line in the stdio buffer forever and a test
#     polling the log waits on a line that has already been written. The
#     runtime's file.write is unbuffered, so the port appears immediately.
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
if [[ ! -x "$OVC" ]]; then
  echo "ovc not built ($OVC); run 'make' first" >&2
  exit 2
fi

BIN="$(mktemp -d)"
cleanup() {
  rm -rf "$BIN"
  pkill -9 -f "ovc run $BIN" 2>/dev/null
}
trap cleanup EXIT

pass=0
fail=0
check_eq() {
  if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1" "got [$3] want [$2]"; fi
}
ok()  { printf 'PASS %s\n' "$1"; pass=$((pass+1)); }
bad() { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail+1)); }

# Poll a file for a value, since the writer flushes when it writes.
wait_file() {
  local path="$1" tries="${2:-80}" i=0 val=""
  while (( i < tries )); do
    if [[ -f "$path" ]]; then
      val="$(<"$path")"
      [[ -n "$val" ]] && { printf '%s' "$val"; return 0; }
    fi
    sleep 0.2
    i=$((i+1))
  done
  return 1
}

# ---------------------------------------------------------------- primitives
# A server that binds port 0, reports the port the kernel chose to a file,
# echoes one line, and exits.
cat > "$BIN/echo.ov" <<EOF
fd = net.listen(0, host = "127.0.0.1")
if fd < 0 { print("listen failed"); exit(1) }
file.write("$BIN/echo.port", str(net.port(fd)))
c = net.accept(fd)
got = net.read(c, 4096)
if got == null { print("read failed"); exit(1) }
net.write(c, "echo:" + got.trim() + "\n")
net.close(c)
net.close(fd)
print("done")
EOF

"$OVC" run "$BIN/echo.ov" > "$BIN/echo.log" 2>&1 &
echo_pid=$!

EPORT="$(wait_file "$BIN/echo.port" || true)"
if [[ -z "$EPORT" ]]; then
  bad "echo server published a port" "$(cat "$BIN/echo.log")"
else
  ok "echo server published a port ($EPORT)"
  got="$(timeout 25 python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $EPORT), timeout=15)
s.settimeout(15)
s.sendall(b'hello\n')
print(s.recv(200).decode().strip())
s.close()
" 2>&1)"
  check_eq "a line sent arrives back byte for byte" "echo:hello" "$got"
fi
wait $echo_pid 2>/dev/null
check_eq "echo server ran clean" "done" "$(tail -1 "$BIN/echo.log")"

# ---------------------------------------------------------------- line I/O
# A TCP stream delivers bytes, not lines: one read may carry half a line or
# three. The assembler is what makes a client's messages usable, and a
# single-read test would not notice if it were wrong.
cat > "$BIN/line.ov" <<EOF
function socket_line(conn) {
    if conn["buffer"] == null { conn["buffer"] = "" }
    while true {
        nl = conn["buffer"].indexof("\n")
        if nl >= 0 {
            line = conn["buffer"].slice(0, nl)
            rest = conn["buffer"].slice(nl + 1, len(conn["buffer"]))
            conn["buffer"] = rest
            return line
        }
        chunk = net.read(conn["fd"], 4096)
        if chunk == null or chunk == "" { return null }
        conn["buffer"] = conn["buffer"] + chunk
    }
}
fd = net.listen(0, host = "127.0.0.1")
file.write("$BIN/line.port", str(net.port(fd)))
conn = {"fd": net.accept(fd)}
i = 0
while i < 3 {
    line = socket_line(conn)
    if line == null { break }
    net.write(conn["fd"], "got:" + line + "\n")
    i = i + 1
}
// A disconnect is the signal to stop, not an error worth reporting.
net.close(conn["fd"])
net.close(fd)
print("done")
EOF

"$OVC" run "$BIN/line.ov" > "$BIN/line.log" 2>&1 &
line_pid=$!
LPORT="$(wait_file "$BIN/line.port" || true)"
if [[ -z "$LPORT" ]]; then
  bad "line server published a port" "$(cat "$BIN/line.log")"
else
  ok "line server published a port ($LPORT)"
  burst="$(timeout 25 python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $LPORT), timeout=15)
s.settimeout(15)
s.sendall(b'one\ntwo\nthree\n')
buf = b''
while buf.count(b'\n') < 3:
    c = s.recv(4096)
    if not c: break
    buf += c
print(buf.decode().strip().replace('\n', ' '))
s.close()
" 2>&1)"
  check_eq "a burst of three lines arrives as three" "got:one got:two got:three" "$burst"
fi
wait $line_pid 2>/dev/null

# ------------------------------------------------- agent in remote mode
# The real agent program, binding a port, refusing a bad token, and serving a
# clean disconnect. Deliberately no model turn: what is under test is the
# transport, and a model call would make this suite depend on a network.
AGENT_HOME="$BIN/home"
# The agent needs the ai module to compile. It is written here rather than
# copied out of ~/.ovyth because the suite has to be hermetic: CI starts on a
# clean runner with no package store, and a test that reads the developer's
# home directory passes locally and fails there.
#
# This is the message/history half of the ai module, which is all the agent
# touches in this test — it never makes a model call, because what is under
# test is the transport, and a model call would make the suite depend on a
# network. ai_request answers as a failure instead, so a test that ever did
# reach it would say so loudly rather than hang.
mkdir -p "$AGENT_HOME/packages/ai"
cat > "$AGENT_HOME/packages/ai/ai.ov" <<'EOF'
// Test fixture: the message/history half of the ai module.
function ai_message(role, content) {
    return {"role": role, "content": content}
}
function ai_conversation(system) {
    return [ai_message("system", system)]
}
function ai_ask(messages, text) {
    messages.push(ai_message("user", text))
    return messages
}
function ai_told(messages, text) {
    messages.push(ai_message("assistant", text))
    return messages
}
function ai_trim(messages, keep) {
    if len(messages) <= keep { return messages }
    head = []
    if len(messages) > 0 and messages[0]["role"] == "system" {
        head.push(messages[0])
    }
    start = len(messages) - (keep - len(head))
    if start < 0 { start = 0 }
    i = start
    while i < len(messages) {
        head.push(messages[i])
        i = i + 1
    }
    return head
}
function ai_request(url, key, model, messages, timeout_s) {
    return {"ok": false, "text": "", "status": 0,
            "error": "test fixture: no model call available", "elapsed_ms": 0.0}
}
EOF

OVYTH_HOME="$AGENT_HOME" \
OVYTH_AGENT_SERVE=0 \
OVYTH_AGENT_HOST="127.0.0.1" \
OVYTH_AGENT_TOKEN="sek" \
OVYTH_AGENT_PORT_FILE="$BIN/agent.port" \
"$OVC" run "$PWD/examples/agent/tui.ov" > "$BIN/agent.log" 2>&1 &
agent_pid=$!

# Same buffering trap as the servers above: the banner sits in the stdio
# buffer while the agent blocks in accept(), so the port is read from the file
# the runtime wrote it to.
APORT="$(wait_file "$BIN/agent.port" || true)"
if [[ -z "$APORT" ]]; then
  kill $agent_pid 2>/dev/null
  wait $agent_pid 2>/dev/null
  bad "agent published a listening port" "$(cat "$BIN/agent.log")"
  echo
  echo "ovc net regression: $pass passed, $fail failed"
  exit 1
fi
ok "agent published a listening port ($APORT)"

# A wrong token is refused before any turn is served.
  out="$(timeout 30 python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $APORT), timeout=20)
s.settimeout(20)
print(s.recv(100).decode().strip())    # 'token: '
s.sendall(b'wrong-token\n')
print(s.recv(100).decode().strip())    # 'auth failed'
s.close()
" 2>&1)"
  if grep -q "auth failed" <<<"$out"; then
    ok "a bad token is refused before any turn"
  else
    bad "a bad token is refused before any turn" "$out"
  fi

  # The right token reaches the agent and disconnects cleanly.
  out="$(timeout 30 python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', $APORT), timeout=20)
s.settimeout(20)
s.recv(100)                             # 'token: '
s.sendall(b'sek\n')
buf = b''
while b'\n' not in buf:
    c = s.recv(200)
    if not c: break
    buf += c
print(buf.decode().strip())              # banner
s.sendall(b':quit\n')
s.recv(100)                              # 'bye'
s.close()
" 2>&1)"
  if grep -q "ovyth agent" <<<"$out"; then
    ok "a good token reaches the agent and disconnects cleanly"
  else
    bad "a good token reaches the agent and disconnects cleanly" "$out"
  fi

  # A phone that drops off and comes back reaches the agent again: the accept
  # loop re-arms rather than exiting after the first session.
  out2="$(timeout 30 python3 -c "
import socket
for _ in range(2):
    s = socket.create_connection(('127.0.0.1', $APORT), timeout=20)
    s.settimeout(20)
    s.recv(100)                          # 'token: '
    s.sendall(b'sek\n')
    s.recv(200)                          # banner
    s.sendall(b':quit\n')
    s.recv(100)                          # 'bye'
    s.close()
print('two sessions')
" 2>&1)"
  check_eq "the agent re-arms for a second connection" "two sessions" "$out2"

kill $agent_pid 2>/dev/null
wait $agent_pid 2>/dev/null

echo
echo "ovc net regression: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
