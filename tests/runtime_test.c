/* Ovyth runtime smoke test -- built by `make runtime-test`. */
#include "ovrt.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, what)                                            \
  do {                                                               \
    if (!(cond)) { printf("FAIL %s\n", what); failures++; }          \
    else printf("ok   %s\n", what);                                  \
  } while (0)

int main(void) {
  ov_runtime_init(0, NULL);

  /* strings */
  OvValue a = ov_str_val("Hello, ");
  OvValue b = ov_str_val("world");
  OvValue c = ov_add(a, b);
  CHECK(ov_str_eq(c.str, ov_str_cstr("Hello, world")), "string concat");
  CHECK(ov_str_len(ov_str_slice(c.str, 0, 5)) == 5, "string slice");
  CHECK(ov_str_contains(c.str, ov_str_cstr("lo, w")), "string contains");

  /* lists survive a collection */
  OvList* l = ov_list_new();
  OvValue lr = ov_list(l);
  ov_gc_register_root(&lr);
  for (int i = 0; i < 5000; i++) ov_list_push(l, ov_int(i));
  ov_gc_collect();
  CHECK(ov_list_len(l) == 5000, "list survives gc");
  CHECK(ov_list_get(l, 4999).i == 4999, "list contents survive gc");

  /* maps, including growth and a string key that must stay alive */
  OvMap* m = ov_map_new();
  OvValue mr = ov_map(m);
  ov_gc_register_root(&mr);
  for (int i = 0; i < 2000; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key-%d", i);
    ov_map_set(m, ov_str_val(key), ov_int(i * 3));
  }
  ov_gc_collect();
  bool ok = true;
  for (int i = 0; i < 2000; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key-%d", i);
    if (ov_map_get(m, ov_str_val(key)).i != i * 3) ok = false;
  }
  CHECK(ok, "2000-entry map survives gc");
  CHECK(ov_map_len(m) == 2000, "map length");

  /* a nested structure reachable only through the map */
  OvList* inner = ov_list_new();
  OvValue ir = ov_list(inner);
  ov_gc_register_root(&ir);
  ov_list_push(inner, ov_str_val("deep"));
  ov_map_set(m, ov_str_val("nested"), ir);
  ov_gc_collect();
  OvValue nested = ov_map_get(m, ov_str_val("nested"));
  CHECK(ov_tagof(nested) == OV_LIST &&
            ov_str_eq(ov_list_get(nested.list, 0).str, ov_str_cstr("deep")),
        "nested value survives gc");

  /* json */
  const char* doc = "{\"a\":[1,2,3],\"b\":\"hi\",\"c\":{\"d\":true},\"e\":null}";
  OvValue parsed = ov_json_parse(doc, strlen(doc));
  CHECK(ov_tagof(parsed) == OV_MAP, "json parses to a map");
  CHECK(ov_list_get(ov_map_get(parsed.map, ov_str_val("a")).list, 2).i == 3,
        "json nested list");
  CHECK(ov_str_eq(ov_map_get(parsed.map, ov_str_val("b")).str, ov_str_cstr("hi")),
        "json string");
  CHECK(ov_is(ov_map_get(parsed.map, ov_str_val("e")), OV_NIL), "json null");
  OvStr* js = ov_json_stringify(parsed);
  CHECK(ov_json_valid(ov_str_data(js), ov_str_len(js)), "round-trip is valid JSON");
  CHECK(ov_str_find(js, ov_str_cstr("\"d\":true"), 0) >= 0, "round-trip content");

  /* arithmetic */
  CHECK(ov_div(ov_int(10), ov_int(4)).i == 2, "int division");
  CHECK(ov_div(ov_float(10), ov_int(4)).f == 2.5, "float division");
  CHECK(ov_add(ov_int(1), ov_float(0.5)).f == 1.5, "int + float widens");
  CHECK(ov_cmp(ov_str_val("a"), ov_str_val("b")) < 0, "string compare");
  CHECK(ov_truthy(ov_int(0)) == 0, "0 is falsy");

  /* nil keys are stored in a reserved slot rather than as a hole */
  OvMap* nm = ov_map_new();
  OvValue nr = ov_map(nm);
  ov_gc_register_root(&nr);
  ov_map_set(nm, ov_nil(), ov_str_val("nil value"));
  ov_map_set(nm, ov_str_val("real"), ov_str_val("ok"));
  CHECK(ov_str_eq(ov_map_get(nm, ov_nil()).str, ov_str_cstr("nil value")),
        "nil key round-trips");
  CHECK(ov_str_eq(ov_map_get(nm, ov_str_val("real")).str, ov_str_cstr("ok")),
        "non-nil key survives beside a nil key");
  CHECK(ov_map_len(nm) == 2, "nil key counts toward length");

  printf("\nlive=%zu heap=%zu allocs=%zu\n", ov_gc_live_bytes(),
         ov_gc_heap_bytes(), ov_heap_alloc_count());
  ov_runtime_shutdown();
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}