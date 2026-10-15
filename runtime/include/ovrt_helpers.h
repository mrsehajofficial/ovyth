// Ovyth runtime :: include/ovrt_helpers.h
//
// Prototypes for the thin C layer the native codegen emits calls into. These
// are the *only* non-ovrt.h entry points generated code uses; everything else
// is plain C against the documented runtime API.
#ifndef OVRT_HELPERS_H
#define OVRT_HELPERS_H

#include "ovrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Value-method dispatch: base.method(a0, a1). out receives the method's
 * return value. Returns 0 on success, 1 on failure (call ov_h_err_value()). */
int ov_h_value_method(OvValue base, const char* name, OvValue a0, OvValue a1,
                      OvValue* out);

/* http.<METHOD>(url, headers=..., json=..., params=..., body=...,
 * content_type=..., timeout=...). out receives the response map
 * {status, ok, body, headers, elapsed_ms}. Returns 0 on success, 1 on
 * failure. */
/* The builtin error channel: record a message, return 1, and the caller reads
 * it back through ov_h_err_value(). Shared with http_builtin.c. */
int ov_h_fail(const char* msg, size_t n);
int ov_h_fail_with(OvValue v);

/* os.run(): run a command through /bin/sh, and report what it produced.
 * out receives {cmd, code, ok, stdout, stderr}. Returns 0 when the command
 * ran, 1 when it could not be started at all. */
int ov_os_run(const char* cmd, int* threw, OvValue* out);
int ov_os_tty(void);   /* is standard output a terminal? */

/* os.exec(): spawn a program without a shell, with a deadline and control
 * over the child's streams, cwd and environment. out receives
 * {ok, status, code, pid, stdout, stderr, elapsed_ms}; status is one of
 * "ok", "failed", "signal", "timeout", "truncated". timeout is milliseconds
 * (0 = none), max_output is bytes (64MB default). Returns 0 when the program
 * ran, 1 when it could not be started or the arguments were invalid. */
int ov_os_exec(OvValue argv, OvValue timeout, OvValue cwd, OvValue env,
               OvValue stdin_v, OvValue stdout_v, OvValue stderr_v,
               OvValue max_output, int* threw, OvValue* out);

/* sleep(ms): wait without burning a core. Monotonic, so a wall-clock change
 * cannot shorten or extend it; EINTR resumes the remainder. */
void ov_os_sleep(OvValue ms);

/* ------------------------------------------------- file descriptors (W3/W4)
 *
 * The four primitives that make an fd a first-class thing: make one, read it,
 * write it, close it. A pipe from os.pipe(), a socket from net.listen() and a
 * file from file.open() are all fds, and all four work on all of them.
 *
 * fds are integers by design -- they are native handles -- which means a
 * program *can* name fd 0, 1 or 2. Treat them as opaque: use what the runtime
 * handed you. */
int  ov_os_pipe(OvValue* out);              /* -> {"read", "write"}      */
int  ov_os_read_fd(int fd, int64_t max, OvValue* out);   /* -> bytes, "" at EOF */
int  ov_os_write_fd(int fd, OvValue data, int64_t* wrote);
void ov_os_close(int fd);

/* file.open(path, mode) -> fd (modes r, w, a, rw); file.seek(fd, off,
 * whence) -> the new position or -1 (whence: SEEK_SET/CUR/END). */
int      ov_file_open_fd(const char* path, const char* mode, int64_t* out_fd);
int64_t  ov_file_seek_fd(int fd, int64_t off, int whence);

/* os.stat(path) -> {size, mode, mtime, is_dir, is_file, is_link} */
int ov_os_stat_path(const char* path, OvValue* out);

/* net.c -- a TCP server, so the agent can be driven from another device.
 *
 * The client side of any connection these can serve is plain TCP: a line sent
 * is a line read back. host "0.0.0.0" listens on every interface (what a phone
 * on the same network needs), "127.0.0.1" keeps the port private to this
 * machine. err receives a static description on failure, never NULL.
 */
int  ov_net_listen(const char* host, int port, int backlog, char** err);
int  ov_net_accept(int listen_fd);      /* blocking; -1 on error        */
int  ov_net_read(int fd, char* buf, int len);   /* blocking; 0 = closed */
int  ov_net_write(int fd, const char* data, int len); /* 0 = all written */
void ov_net_close(int fd);
int  ov_net_port(int fd);  /* port a listening socket actually bound (bind 0) */

int ov_h_http(const char* method, OvValue url, OvValue headers, OvValue json_body,
              OvValue params, OvValue named_params, OvValue body, OvValue ctype,
              OvValue timeout, OvValue* out);

/* Indexing: list[int], map[key], string[int], string[string]. Mirrors the
 * interpreter's index_get. Returns 0 on success (out set), 1 on failure. */
int ov_h_index(OvValue base, OvValue idx, OvValue* out);

/* Field access: m.name, s.length, l.length. Mirrors member_get. Returns 0
 * on success (out set), 1 on failure. */
int ov_h_member(OvValue base, const char* name, OvValue* out);

/* Console: prompt + one line (nil on EOF). prompt may be nil. */
OvValue ov_h_input(OvValue prompt);

/* Environment: returns the value, or throws (set *err) when unset. */
OvValue ov_h_env(OvValue key, int* threw);

/* JSON: parse a Ovyth string value; throws (set *err) on invalid JSON. */
OvValue ov_h_json_parse(OvValue s, int* threw);

/* int(value): a Ovyth Int stays, a Float truncates, a Bool is 0/1, a String
 * is parsed. Throws (set *err) when it cannot read a number. */
OvValue ov_h_to_int(OvValue v, int* threw);
OvValue ov_h_to_float(OvValue v, int* threw);

/* len(value): string/list/map size. Throws (set *err) on the wrong kind. */
OvValue ov_h_len(OvValue v, int* threw);

/* pad(s, width[, fill[, side]]): see ov_str_pad. `side` pads on the right when
 * it is the string "right"; anything else (including nil) pads on the left,
 * the default that matches the s.pad(width) method form. Throws (sets *err)
 * when the first argument is not a string. */
OvValue ov_h_pad(OvValue s, OvValue width, OvValue fill, OvValue side, int* threw);

/* gc([mode]): collect by default, report with "stats", clear counters with
 * "reset" -- byte-for-byte the same three modes Interp::call_builtin has. */
OvValue ov_h_gc(OvValue mode);

/* The pending error from a failing helper (valid only when one returned 1). */
OvValue ov_h_err_value(void);

/* ------------------------------------------------------------ closures */
// Wraps a generated C function in a OvFunc. arity is the parameter count.
OvFunc* ov_h_make_func(int arity, OvFnPtr fn);
OvFunc* ov_h_make_closure(int arity, OvFnPtr fn, void* upvals, int num_upvals);
OvValue ov_h_make_func_value(const char* name, int arity, OvFnPtr fn);

/* Calls a OvValue that is known to hold a OvFunc, passing `args` (count `n`).
 * Sets *slot to a freshly allocated argv the callee may use for `...`. */
OvValue ov_h_call(OvValue callee, OvValue args[], int n, OvValue** slot, int* argc);

/* clock_gettime(CLOCK_MONOTONIC) in seconds. */
double ov_h_now(void);          /* monotonic seconds — for measuring        */
int64_t ov_h_now_realtime(void); /* unix seconds — for timestamps           */

/* Numeric builtins that need a coerced numeric value. Each throws (sets the
 * pending error) when the argument is not a number. */
OvValue ov_h_num1(OvValue v, int* threw, char op);   /* abs sqrt floor ceil round */
OvValue ov_h_sum(OvValue list, int* threw);          /* sum(list) */
OvValue ov_h_join(OvValue sep, OvValue list);        /* join(sep, list) */

#ifdef __cplusplus
}
#endif

#endif /* OVRT_HELPERS_H */
