/* Vayu runtime :: src/env.c -- environment variables and process args. */
/* getenv/setenv are POSIX; _POSIX_C_SOURCE exposes them without leaving -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "vyrt.h"

#include <stdlib.h>
#include <string.h>

VyStr* vy_env_get(const char* name) {
  const char* v = getenv(name);
  return v ? vy_str_cstr(v) : NULL;
}

VyValue vy_env_get_val(const char* name) {
  const char* v = getenv(name);
  return v ? vy_str_val(v) : vy_nil();
}

void vy_env_set(const char* name, VyStr* value) {
  setenv(name, vy_str_data(value), 1);
}

int       vy_argc(void)           { return g_argc; }
const char* vy_argv(int i)        { return (i >= 0 && i < g_argc) ? g_argv[i] : ""; }

VyValue vy_args(void) {
  VyList* out = vy_list_new_cap((uint32_t)(g_argc > 0 ? g_argc : 1));
  if (g_argc > 0) vy_list_push(out, vy_str_val(g_argv[0]));  /* program name */
  for (int i = 1; i < g_argc; i++) vy_list_push(out, vy_str_val(g_argv[i]));
  return vy_list(out);
}