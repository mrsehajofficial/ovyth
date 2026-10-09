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
double ov_h_now(void);

/* Numeric builtins that need a coerced numeric value. Each throws (sets the
 * pending error) when the argument is not a number. */
OvValue ov_h_num1(OvValue v, int* threw, char op);   /* abs sqrt floor ceil round */
OvValue ov_h_sum(OvValue list, int* threw);          /* sum(list) */
OvValue ov_h_join(OvValue sep, OvValue list);        /* join(sep, list) */

#ifdef __cplusplus
}
#endif

#endif /* OVRT_HELPERS_H */
