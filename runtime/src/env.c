/* Ovyth runtime :: src/env.c -- environment variables and process args. */
/* getenv/setenv are POSIX; _POSIX_C_SOURCE exposes them without leaving -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "ovrt.h"

#include <stdlib.h>
#include <string.h>

OvStr* ov_env_get(const char* name) {
  const char* v = getenv(name);
  return v ? ov_str_cstr(v) : NULL;
}

OvValue ov_env_get_val(const char* name) {
  const char* v = getenv(name);
  return v ? ov_str_val(v) : ov_nil();
}

void ov_env_set(const char* name, OvStr* value) {
  setenv(name, ov_str_data(value), 1);
}

int       ov_argc(void)           { return g_argc; }
const char* ov_argv(int i)        { return (i >= 0 && i < g_argc) ? g_argv[i] : ""; }

OvValue ov_args(void) {
  OvList* out = ov_list_new_cap((uint32_t)(g_argc > 0 ? g_argc : 1));
  if (g_argc > 0) ov_list_push(out, ov_str_val(g_argv[0]));  /* program name */
  for (int i = 1; i < g_argc; i++) ov_list_push(out, ov_str_val(g_argv[i]));
  return ov_list(out);
}