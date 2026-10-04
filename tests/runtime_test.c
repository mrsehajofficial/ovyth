/* Vayu runtime smoke test -- built by `make runtime-test`. */
#include "vyrt.h"

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
  vy_runtime_init(0, NULL);

  /* strings */
  VyValue a = vy_str_val("Hello, ");
  VyValue b = vy_str_val("world");
  VyValue c = vy_add(a, b);
  CHECK(vy_str_eq(c.str, vy_str_cstr("Hello, world")), "string concat");
  CHECK(vy_str_len(vy_str_slice(c.str, 0, 5)) == 5, "string slice");
  CHECK(vy_str_contains(c.str, vy_str_cstr("lo, w")), "string contains");

  /* lists survive a collection */
  VyList* l = vy_list_new();
  VyValue lr = vy_list(l);
  vy_gc_register_root(&lr);
  for (int i = 0; i < 5000; i++) vy_list_push(l, vy_int(i));
  vy_gc_collect();
  CHECK(vy_list_len(l) == 5000, "list survives gc");
  CHECK(vy_list_get(l, 4999).i == 4999, "list contents survive gc");

  /* maps, including growth and a string key that must stay alive */
  VyMap* m = vy_map_new();
  VyValue mr = vy_map(m);
  vy_gc_register_root(&mr);
  for (int i = 0; i < 2000; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key-%d", i);
    vy_map_set(m, vy_str_val(key), vy_int(i * 3));
  }
  vy_gc_collect();
  bool ok = true;
  for (int i = 0; i < 2000; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key-%d", i);
    if (vy_map_get(m, vy_str_val(key)).i != i * 3) ok = false;
  }
  CHECK(ok, "2000-entry map survives gc");
  CHECK(vy_map_len(m) == 2000, "map length");

  /* a nested structure reachable only through the map */
  VyList* inner = vy_list_new();
  VyValue ir = vy_list(inner);
  vy_gc_register_root(&ir);
  vy_list_push(inner, vy_str_val("deep"));
  vy_map_set(m, vy_str_val("nested"), ir);
  vy_gc_collect();
  VyValue nested = vy_map_get(m, vy_str_val("nested"));
  CHECK(vy_tagof(nested) == VY_LIST &&
            vy_str_eq(vy_list_get(nested.list, 0).str, vy_str_cstr("deep")),
        "nested value survives gc");

  /* json */
  const char* doc = "{\"a\":[1,2,3],\"b\":\"hi\",\"c\":{\"d\":true},\"e\":null}";
  VyValue parsed = vy_json_parse(doc, strlen(doc));
  CHECK(vy_tagof(parsed) == VY_MAP, "json parses to a map");
  CHECK(vy_list_get(vy_map_get(parsed.map, vy_str_val("a")).list, 2).i == 3,
        "json nested list");
  CHECK(vy_str_eq(vy_map_get(parsed.map, vy_str_val("b")).str, vy_str_cstr("hi")),
        "json string");
  CHECK(vy_is(vy_map_get(parsed.map, vy_str_val("e")), VY_NIL), "json null");
  VyStr* js = vy_json_stringify(parsed);
  CHECK(vy_json_valid(vy_str_data(js), vy_str_len(js)), "round-trip is valid JSON");
  CHECK(vy_str_find(js, vy_str_cstr("\"d\":true"), 0) >= 0, "round-trip content");

  /* arithmetic */
  CHECK(vy_div(vy_int(10), vy_int(4)).i == 2, "int division");
  CHECK(vy_div(vy_float(10), vy_int(4)).f == 2.5, "float division");
  CHECK(vy_add(vy_int(1), vy_float(0.5)).f == 1.5, "int + float widens");
  CHECK(vy_cmp(vy_str_val("a"), vy_str_val("b")) < 0, "string compare");
  CHECK(vy_truthy(vy_int(0)) == 0, "0 is falsy");

  /* nil keys are stored in a reserved slot rather than as a hole */
  VyMap* nm = vy_map_new();
  VyValue nr = vy_map(nm);
  vy_gc_register_root(&nr);
  vy_map_set(nm, vy_nil(), vy_str_val("nil value"));
  vy_map_set(nm, vy_str_val("real"), vy_str_val("ok"));
  CHECK(vy_str_eq(vy_map_get(nm, vy_nil()).str, vy_str_cstr("nil value")),
        "nil key round-trips");
  CHECK(vy_str_eq(vy_map_get(nm, vy_str_val("real")).str, vy_str_cstr("ok")),
        "non-nil key survives beside a nil key");
  CHECK(vy_map_len(nm) == 2, "nil key counts toward length");

  printf("\nlive=%zu heap=%zu allocs=%zu\n", vy_gc_live_bytes(),
         vy_gc_heap_bytes(), vy_heap_alloc_count());
  vy_runtime_shutdown();
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}