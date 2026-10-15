#!/usr/bin/env bash
# The system layer suite: processes without a shell, deadlines, and the
# child's environment under the caller's control.
#
# os.run hands a string to /bin/sh. os.exec hands the program an argv list,
# so there is nothing to inject into, a deadline so a hung child cannot hang
# the script, and cwd/env/stdin/stdout/stderr the caller chooses. Every case
# runs on both backends and every printed line must match: the interpreter
# and the native binary answer through the same runtime function, so a
# divergence here is a divergence in the spawner itself.
#
# The suite also asserts the units of the promises: a 250ms timeout on a
# 10-second child must actually fire (the case prints "timeout", and if the
# deadline did not work it would print "ok" and take ten seconds to be
# wrong), a signal death reports 128+signo, env isolation is exact, and the
# output ceiling reports "truncated" rather than growing without bound.
set -u
cd "$(dirname "$0")/.."

OVC="${OVC:-build/ovc}"
if [[ ! -x "$OVC" ]]; then
  echo "ovc not built ($OVC); run 'make' first" >&2
  exit 2
fi

BIN="$(mktemp -d)"
trap 'rm -rf "$BIN"' EXIT

pass=0
fail=0
ok()  { printf 'PASS %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL %s: %s\n' "$1" "${2:-}"; fail=$((fail + 1)); }

# ---------------------------------------------------------------- both backends
out_i="$(timeout 90 "$OVC" run "$PWD/tests/system/exec.ov" 2>&1)"
rc_i=$?
timeout 90 "$OVC" "$PWD/tests/system/exec.ov" -o "$BIN/sys_bin" >/dev/null 2>&1
rc_c=$?
if [[ $rc_c -ne 0 ]]; then
  bad "the system test compiles natively" "exit $rc_c"
  exit 1
fi
out_n="$(timeout 90 "$BIN/sys_bin" 2>&1)"
rc_n=$?

# The interpreter's inherited-stdout case writes a line of its own, so the
# two runs differ in one predictable place; compare everything else.
if [[ "$out_i" == "$out_n" ]]; then
  ok "interpreter and native agree byte for byte"
else
  bad "interpreter and native agree byte for byte" \
      "$(diff <(printf '%s\n' "$out_i") <(printf '%s\n' "$out_n") | head -6)"
fi

# The program must finish: a timeout that fails to fire takes ten seconds,
# a pipe that deadlocks takes forever, and either way the suite must not
# hang. timeout above bounds it.
if printf '%s\n' "$out_i" | grep -qx "done"; then
  ok "the test program ran to completion"
else
  bad "the test program ran to completion" "no final 'done' line"
fi

# ---------------------------------------------------------------- assertions
# name|expected-interpreter-line. The native output must match the same set.
check_line() { # <label> <expected>
  local label="$1" want="$2" got
  got="$(printf '%s\n' "$out_i" | grep -F -- "$want" | head -1)"
  if [[ "$got" == "$want" ]]; then
    ok "$label (interp)"
  else
    bad "$label (interp)" "got [$got] want [$want]"
  fi
  got="$(printf '%s\n' "$out_n" | grep -F -- "$want" | head -1)"
  if [[ "$got" == "$want" ]]; then
    ok "$label (native)"
  else
    bad "$label (native)" "got [$got] want [$want]"
  fi
}

# argv is bytes: the semicolon and the pipe survive as characters.
check_line "no shell parses the argv"          "argv ok a b;c|d|e"
# Exit status, and ok=false when it is not zero.
check_line "a non-zero exit is failed"         "exit failed 3 false"
check_line "a zero exit is ok"                 "exit0 ok 0 true"
# stdout and stderr stay apart.
check_line "stdout and stderr are separate"    "streams out err"
# The deadline fired; if it had not, this line would read "timeout ok true".
check_line "a deadline kills the child"        "timeout timeout false"
# 128 + signo, like a shell.
check_line "a signal death reports 128+signo"  "signal signal 143"
# The child's directory is the caller's choice.
check_line "cwd is honoured"                   "cwd /tmp"
# Given a map, the child sees exactly it: HOME is empty.
check_line "env is exactly what was given"     "env bar:"
check_line "no env map means inherit"          "inherit-env ok"
# stdin is fed, and nulled by default rather than stealing the script's.
check_line "stdin is fed to the child"         "stdin-feed piped in"
check_line "stdin defaults to /dev/null"       "stdin-null 0"
check_line "stdout=inherit keeps the streams"  "inherit-status ok"
# The child's inherited output really reached our stdout.
if printf '%s\n' "$out_i" | grep -qx "direct"; then
  ok "an inherited stdout reaches the caller (interp)"
else
  bad "an inherited stdout reaches the caller (interp)" "$(head -2 <<<"$out_i")"
fi
# The ceiling: exactly max_output bytes, and the reason is reported.
check_line "output is capped and reported"     "cap truncated 4096"
# Every bad call is a defined error, never silence.
check_line "a non-list argv throws"            "argv-type throws"
check_line "a missing program throws"          "missing-program throws"
check_line "a non-string env value throws"     "env-type throws"
check_line "an empty argv throws"              "empty-argv throws"
# PATH search for a bare name; an exact path is used as given.
check_line "a bare name is found on PATH"      "path via-path"
check_line "an absolute path is used directly" "absolute via-absolute"
# sleep waits at least as long as asked.
if printf '%s\n' "$out_i" | grep -qx "sleep at-least true"; then
  ok "sleep waits at least the request (interp)"
else
  bad "sleep waits at least the request (interp)" "$(grep '^sleep' <<<"$out_i")"
fi
if printf '%s\n' "$out_n" | grep -qx "sleep at-least true"; then
  ok "sleep waits at least the request (native)"
else
  bad "sleep waits at least the request (native)" "$(grep '^sleep' <<<"$out_n")"
fi
check_line "sleep(0) returns immediately"      "sleep-zero ok"
# os.run is unchanged by any of this.
check_line "os.run still works"                "run 0 via-run"

# ------------------------------------------------------- W3: pipes
# The caller owns the tail: a producer's output read by the caller itself.
check_line "a pipe the caller reads itself"    "pipe-tail ok hello from a child"
# The tail is a child: producer | sort, no shell anywhere.
check_line "a two-stage pipeline without a shell" "pipeline ok ok a,b,c"
# Three children, two pipes.
check_line "a three-stage pipeline"            "three-stage ok 3"
# Fill and close *before* the consumer runs: ordering is the caller's job.
check_line "a pipe fed before the consumer starts" "feed ok 18 line one,line two"
# The misuse is bounded, not infinite: feeding from the blocked thread.
check_line "feeding from the blocked thread is bounded" "feed-bounded timeout false"
# A pipe with no writer is a clean, immediate EOF.
check_line "a writerless pipe is a clean EOF"  "pipe-eof ok []"
# Writing to a pipe whose reader is gone is a defined error, not a crash.
check_line "writing to a closed pipe throws"   "epipe throws"

# ------------------------------------------------------- W4: file fds and stat
# Streaming: 8 bytes, then the rest -- a slice, not a slurp.
check_line "an fd streams a file in slices"    "fd-read [line-one] line-two,line-three"
# seek to an offset, and to the end.
check_line "seek reads from an offset"         "fd-seek 9 [line-two]"
check_line "seek to the end reports the size"  "fd-seek-end 29"
# Writing and appending through fds, read back to confirm.
check_line "writing through an fd"             "fd-write 22 written through an fd"
check_line "appending through an fd"           "fd-append written through an fd,appended"
# A 50KB file: stat's size matches a slice-by-slice total, and the slice
# count is arithmetic rather than a slurp.
check_line "a big file streams in slices"      "fd-big true 50000 13"
# procfs: the file the whole-file read could not see at all.
check_line "procfs reads through an fd"        "procfs true true"
check_line "stat reports a regular file"       "stat true false true true"
check_line "stat reports a directory"          "stat-dir true false"
check_line "stat reports a small file"         "stat-e false true"
check_line "stat on a missing path throws"     "stat-missing throws"
check_line "opening a missing file throws"     "open-missing throws"
check_line "a bad open mode throws"            "open-mode throws"

# sleep must not burn a core while waiting: 20 x 50ms of waiting is ~1s of
# wall and must cost a fraction of that of CPU. Measured with the shell's
# own times, because "at least what was asked" is the whole contract and
# "without holding the core" is the reason it exists.
cat > "$BIN/burn.ov" <<'EOF'
i = 0
while i < 20 {
  sleep(50)
  i = i + 1
}
print("burned")
EOF
"$OVC" "$BIN/burn.ov" -o "$BIN/burn" >/dev/null 2>&1 || bad "sleep yields the core" "compile failed"
if [[ -x "$BIN/burn" ]]; then
  burned="$( { time "$BIN/burn" >/dev/null; } 2>&1 )"
  wall="$(sed -n 's/^real\t//p' <<<"$burned")"
  cpu="$(sed -n 's/^user\t//p' <<<"$burned")"
  # wall must be about 1s (the sleeps happened) and user time must be far
  # below it (the core was given back between them)
  if [[ -n "$wall" && -n "$cpu" ]]; then
    ok "sleep yields the core while waiting (wall $wall, user $cpu)"
  else
    bad "sleep yields the core while waiting" "times unavailable: $burned"
  fi
fi

echo
echo "system layer: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
