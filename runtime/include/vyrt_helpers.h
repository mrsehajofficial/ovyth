// Vayu runtime :: include/vyrt_helpers.h
//
// Prototypes for the thin C layer the native codegen emits calls into. These
// are the *only* non-vyrt.h entry points generated code uses; everything else
// is plain C against the documented runtime API.
#ifndef VYRT_HELPERS_H
#define VYRT_HELPERS_H

#include "vyrt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Value-method dispatch: base.method(a0, a1). out receives the method's
 * return value. Returns 0 on success, 1 on failure (call vy_h_err_value()). */
int vy_h_value_method(VyValue base, const char* name, VyValue a0, VyValue a1,
                      VyValue* out);

/* http.<METHOD>(url, headers=..., json=..., params=..., body=...,
 * content_type=..., timeout=...). out receives the response map
 * {status, ok, body, headers, elapsed_ms}. Returns 0 on success, 1 on
 * failure. */
int vy_h_http(const char* method, VyValue url, VyValue headers, VyValue json_body,
              VyValue params, VyValue named_params, VyValue body, VyValue ctype,
              VyValue timeout, VyValue* out);

/* Indexing: list[int], map[key], string[int], string[string]. Mirrors the
 * interpreter's index_get. Returns 0 on success (out set), 1 on failure. */
int vy_h_index(VyValue base, VyValue idx, VyValue* out);

/* Field access: m.name, s.length, l.length. Mirrors member_get. Returns 0
 * on success (out set), 1 on failure. */
int vy_h_member(VyValue base, const char* name, VyValue* out);

/* Console: prompt + one line (nil on EOF). prompt may be nil. */
VyValue vy_h_input(VyValue prompt);

/* Environment: returns the value, or throws (set *err) when unset. */
VyValue vy_h_env(VyValue key, int* threw);

/* JSON: parse a Vayu string value; throws (set *err) on invalid JSON. */
VyValue vy_h_json_parse(VyValue s, int* threw);

/* int(value): a Vayu Int stays, a Float truncates, a Bool is 0/1, a String
 * is parsed. Throws (set *err) when it cannot read a number. */
VyValue vy_h_to_int(VyValue v, int* threw);
VyValue vy_h_to_float(VyValue v, int* threw);

/* len(value): string/list/map size. Throws (set *err) on the wrong kind. */
VyValue vy_h_len(VyValue v, int* threw);

/* pad(s, width[, fill[, side]]): see vy_str_pad. `side` pads on the right when
 * it is the string "right"; anything else (including nil) pads on the left,
 * the default that matches the s.pad(width) method form. Throws (sets *err)
 * when the first argument is not a string. */
VyValue vy_h_pad(VyValue s, VyValue width, VyValue fill, VyValue side, int* threw);

/* gc([mode]): collect by default, report with "stats", clear counters with
 * "reset" -- byte-for-byte the same three modes Interp::call_builtin has. */
VyValue vy_h_gc(VyValue mode);

/* The pending error from a failing helper (valid only when one returned 1). */
VyValue vy_h_err_value(void);

/* ------------------------------------------------------------ closures */
// Wraps a generated C function in a VyFunc. arity is the parameter count.
VyFunc* vy_h_make_func(int arity, VyFnPtr fn);
VyValue vy_h_make_func_value(const char* name, int arity, VyFnPtr fn);

/* Calls a VyValue that is known to hold a VyFunc, passing `args` (count `n`).
 * Sets *slot to a freshly allocated argv the callee may use for `...`. */
VyValue vy_h_call(VyValue callee, VyValue args[], int n, VyValue** slot, int* argc);

/* clock_gettime(CLOCK_MONOTONIC) in seconds. */
double vy_h_now(void);

/* Numeric builtins that need a coerced numeric value. Each throws (sets the
 * pending error) when the argument is not a number. */
VyValue vy_h_num1(VyValue v, int* threw, char op);   /* abs sqrt floor ceil round */
VyValue vy_h_sum(VyValue list, int* threw);          /* sum(list) */
VyValue vy_h_join(VyValue sep, VyValue list);        /* join(sep, list) */

#ifdef __cplusplus
}
#endif

#endif /* VYRT_HELPERS_H */
