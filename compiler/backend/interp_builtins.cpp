#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "interp.h"

namespace vy {
using namespace ast;

// The stdlib. Every builtin is a plain function here, and the LLVM backend
// emits calls to exactly the same C entry points, so the two backends cannot
// drift apart in behaviour.

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static std::string str_arg(VyValue v) {
  if (vy_tagof(v) == VY_STRING) return std::string(v.str->bytes, v.str->len);
  return std::string(vy_str_data(vy_render(v)));
}

static VyStr* mk_str(const std::string& s) { return vy_str_new(s.data(), s.size()); }

static VyValue to_number(VyValue v) {
  switch (vy_tagof(v)) {
    case VY_INT:   return v;
    case VY_FLOAT: return v;
    case VY_BOOL:  return vy_int(v.b);
    case VY_STRING: {
      const char* p = vy_str_data(v.str);
      char* end = nullptr;
      double d = strtod(p, &end);
      if (end && end != p && *end == '\0')
        return (strchr(p, '.') || strchr(p, 'e') || strchr(p, 'E'))
                   ? vy_float(d)
                   : vy_int((int64_t)d);
      break;
    }
    default: break;
  }
  Interp::Throw t;
  std::string what = vy_tagof(v) == VY_STRING
                         ? "\"" + str_arg(v) + "\""
                         : std::string(vy_type_name(v));
  t.value = vy_s(std::string("cannot read a number from ") + what);
  throw t;
}

static double as_double(VyValue v) {
  VyValue n = to_number(v);
  return vy_tagof(n) == VY_FLOAT ? n.f : (double)n.i;
}

// ---------------------------------------------------------------------------
// http.*  ->  a map shaped the way the PRD's chatbot example reads it:
//            response.json["choices"][0]["message"]["content"]
// ---------------------------------------------------------------------------
static VyValue http_call(const std::string& method, VyValue url_v,
                         VyValue headers, VyValue params, VyValue json_body,
                         VyValue body, VyValue content_type, VyValue timeout) {
  std::string url = str_arg(url_v);
  if (url.empty()) {
    Interp::Throw t;
    t.value = vy_s(std::string("http.") + method + " requires a url");
    throw t;
  }

  std::string body_str;
  std::string ctype;
  VyStr* body_keep = nullptr;  // keep the serialised body alive across the call
  if (!vy_isnil(json_body)) {
    body_keep = vy_json_stringify(json_body);
    body_str = vy_str_data(body_keep);
    ctype = "application/json";
    if (!vy_isnil(content_type)) ctype = str_arg(content_type);
  } else if (!vy_isnil(body)) {
    if (vy_tagof(body) == VY_STRING) body_str = str_arg(body);
    else {
      body_keep = vy_json_stringify(body);
      body_str = vy_str_data(body_keep);
      ctype = "application/json";
    }
    if (!vy_isnil(content_type)) ctype = str_arg(content_type);
  }

  // Keep the serialised header/param/body JSON alive until vy_http_request
  // returns: the char* views below point into those allocations. The GC can
  // only see VyValue slots (plus what we hand to vy_gc_collect_ex), so pin
  // them explicitly around the request.
  VyStr* hdr_keep = nullptr;
  std::string hdr_json;
  bool has_hdr = false;
  if (!vy_isnil(headers)) {
    hdr_keep = vy_json_stringify(headers);
    hdr_json = vy_str_data(hdr_keep);
    has_hdr = true;
  }
  VyStr* par_keep = nullptr;
  std::string par_json;
  bool has_par = false;
  if (!vy_isnil(params)) {
    par_keep = vy_json_stringify(params);
    par_json = vy_str_data(par_keep);
    has_par = true;
  }
  double tmo = vy_isnil(timeout)
                   ? 30.0
                   : (vy_tagof(timeout) == VY_FLOAT ? timeout.f : (double)timeout.i);

  VyValue extra[3];
  int ne = 0;
  if (body_keep) extra[ne++] = vy_str(body_keep);
  if (hdr_keep) extra[ne++] = vy_str(hdr_keep);
  if (par_keep) extra[ne++] = vy_str(par_keep);
  // The C library below allocates (headers, buffers); a collection triggered
  // by those would only see raw char* views, so hold the heap quiet and keep
  // the owning strings in `extra` for the explicit collection points.
  vy_gc_begin_mutation();
  VyHttpResponse* r =
      vy_http_request(method.c_str(), url.c_str(),
                      body_str.empty() ? nullptr : body_str.c_str(),
                      ctype.empty() ? nullptr : ctype.c_str(),
                      has_hdr ? hdr_json.c_str() : nullptr,
                      has_par ? par_json.c_str() : nullptr, tmo);
  vy_gc_end_mutation();
  if (!r) {
    Interp::Throw t;
    t.value = vy_str_val("http request could not be allocated");
    throw t;
  }

  /* A transport failure (DNS, TLS, timeout) is raised as a Vayu error so the
   * program can `try { ... } catch err { }` exactly like a runtime panic. */
  if (r->error) {
    Interp::Throw t;
    t.value = vy_str(r->error);
    throw t;
  }

  VyMap* m = vy_map_new();
  vy_map_set(m, vy_str_val("status"), vy_int(r->status));
  vy_map_set(m, vy_str_val("ok"), vy_bool(r->status >= 200 && r->status < 400));
  vy_map_set(m, vy_str_val("body"), vy_str(r->body));
  vy_map_set(m, vy_str_val("headers"), r->headers);
  vy_map_set(m, vy_str_val("elapsed_ms"), vy_float(r->elapsed_ms));
  return vy_map(m);
}

// ---------------------------------------------------------------------------
// builtin dispatch
// ---------------------------------------------------------------------------
VyValue Interp::call_builtin(const std::string& qualifier, const Expr* site, Env& env,
                             const ExprList& args,
                             const std::vector<NamedArg>& named, bool* handled) {
  *handled = true;
  std::string q = qualifier;

  auto ARG = [&](size_t i) -> VyValue {
    return i < args.size() ? eval(args[i], env) : vy_nil();
  };
  auto NAMED = [&](const char* key) -> VyValue {
    for (const auto& na : named)
      if (na.name == key) return eval(na.value, env);
    return vy_nil();
  };
  auto bad = [&](const std::string& msg) -> VyValue {
    Throw t;
    t.value = vy_s(msg);
    throw t;
  };

  if (q.empty()) {
    const Expr* callee = site->a;
    if (callee->kind == ExprKind::Member) {
      // A member call: base.method(args). If the base names one of the known
      // namespaces, route it to the qualified builtin; otherwise evaluate the
      // base to a value and dispatch the method on that value (string / list /
      // map methods live on the values themselves, not on global names).
      const bool base_is_ident = callee->a && callee->a->kind == ExprKind::Identifier;
      const std::string ns = base_is_ident ? callee->a->name : std::string();
      const bool is_ns = (ns == "http" || ns == "json" || ns == "math" ||
                         ns == "str"  || ns == "os"   || ns == "time");
      if (is_ns) {
        // `http.get(...)`: the namespace has no field of that name, so call the
        // qualified builtin `ns.method`; but a real field (e.g. a user variable
        // shadowing the namespace) is resolved as a value.
        VyValue* slot = env.find(ns);
        bool is_ns_map = slot && vy_tagof(*slot) == VY_MAP;
        if (is_ns_map && !vy_map_has(slot->map, vy_s(callee->name)))
          q = ns + "." + callee->name;
        else {
          VyValue base = eval(callee->a, env);
          return member_get(base, callee->name, callee);
        }
      } else {
        VyValue base = eval(callee->a, env);
        const std::string& f = callee->name;
        VyValue a0 = ARG(0), a1 = ARG(1);
        if (vy_tagof(base) == VY_STRING) {
          VyStr* s = base.str;
          if (f == "upper")      return vy_str(vy_str_upper(s));
          if (f == "lower")      return vy_str(vy_str_lower(s));
          if (f == "trim")       return vy_str(vy_str_trim(s));
          if (f == "split") { std::string sep = str_arg(a0);
                             return vy_str_split(s, mk_str(sep)); }
          if (f == "startswith") return vy_bool(vy_str_find(s, a0.str, 0) == 0);
          if (f == "endswith") {
            int64_t at = vy_str_find(s, a0.str, 0);
            return vy_bool(at >= 0 && (size_t)at + a0.str->len == s->len);
          }
          if (f == "contains")   return vy_bool(vy_str_contains(s, a0.str));
          if (f == "replace")    return vy_str(vy_str_replace(s, a0.str, a1.str));
          if (f == "indexof" || f == "find") return vy_int(vy_str_find(s, a0.str, 0));
          if (f == "chars")      return vy_str_chars(s);
          if (f == "bytes")      return vy_str_bytes(s);
          if (f == "repeat")     return vy_str(vy_str_repeat(s, a0.i));
          if (f == "length")     return vy_int(s->len);
          if (f == "slice")      return vy_str(vy_str_slice(s, a0.i, a1.i));
          if (f == "pad")        return vy_str(vy_str_pad(s, a0.i, a1.i, 1));
        }
        if (vy_tagof(base) == VY_LIST) {
          if (f == "push" || f == "append") { vy_list_push(base.list, a0); return base; }
          if (f == "pop")     return vy_list_pop(base.list);
          if (f == "insert")  { vy_list_insert(base.list, a0.i, a1); return base; }
          if (f == "remove")  return vy_bool(vy_list_remove(base.list, a0.i));
          if (f == "sort")    { vy_list_sort(base.list); return base; }
          if (f == "reverse") { vy_list_reverse(base.list); return base; }
          if (f == "contains")return vy_bool(vy_list_contains(base.list, a0));
          if (f == "indexof") return vy_int(vy_list_index(base.list, a0));
          if (f == "extend") {
            if (vy_tagof(a0) == VY_LIST)
              for (uint32_t i = 0; i < a0.list->len; i++)
                vy_list_push(base.list, a0.list->items[i]);
            return base;
          }
          if (f == "length")  return vy_int(base.list->len);
        }
        if (vy_tagof(base) == VY_MAP) {
          VyMap* mp = base.map;
          if (f == "get")     return vy_map_get(mp, a0);
          if (f == "set")     { vy_map_set(mp, a0, a1); return base; }
          if (f == "has")     return vy_bool(vy_map_has(mp, a0));
          if (f == "delete" || f == "remove") return vy_bool(vy_map_del(mp, a0));
          if (f == "keys")    return vy_map_keys(mp);
          if (f == "values")  return vy_map_values(mp);
          if (f == "items")   return vy_list(vy_map_pairs(mp));
          if (f == "count" || f == "length") return vy_int(mp->len);
          if (f == "merge") {
            if (vy_tagof(a0) == VY_MAP) {
              VyList* pr = vy_map_pairs(a0.map);
              for (uint32_t i = 0; i + 1 < pr->len; i += 2)
                vy_map_set(mp, pr->items[i], pr->items[i + 1]);
            }
            return base;
          }
        }
        return bad(std::string(vy_type_name(base)) + " has no method '" + f + "'");
      }
    } else if (callee->kind != ExprKind::Identifier) {
      *handled = false;
      return vy_nil();
    } else {
      q = callee->name;
    }
  }

  // ---------------------------------------------------------------- console
  if (q == "print") {
    for (size_t i = 0; i < args.size(); i++) {
      if (i) fputc(' ', stdout);
      VyStr* r = vy_render(eval(args[i], env));
      fwrite(r->bytes, 1, r->len, stdout);
    }
    fputc('\n', stdout);
    return vy_nil();
  }
  if (q == "eprint") {
    std::string out;
    for (size_t i = 0; i < args.size(); i++) {
      if (i) out += ' ';
      out += str_arg(eval(args[i], env));
    }
    fprintf(stderr, "%s\n", out.c_str());
    return vy_nil();
  }
  if (q == "input") {
    if (!args.empty()) {
      std::string prompt = str_arg(eval(args[0], env));
      fputs(prompt.c_str(), stdout);
    }
    fflush(stdout);
    VyStr* line = vy_read_line();
    if (!line) return vy_nil();
    return vy_str(line);
  }

  // ------------------------------------------------------------ environment
  if (q == "env" || q == "getenv") {
    std::string key = str_arg(ARG(0));
    const char* v = getenv(key.c_str());
    if (!v) return bad("environment variable '" + key + "' is not set");
    return vy_str_val(v);
  }
  if (q == "env_or" || q == "getenv_or") {
    std::string key = str_arg(ARG(0));
    const char* v = getenv(key.c_str());
    return v ? vy_str_val(v) : ARG(1);
  }
  if (q == "setenv") {
    std::string k = str_arg(ARG(0)), v = str_arg(ARG(1));
    setenv(k.c_str(), v.c_str(), 1);
    return vy_nil();
  }
  if (q == "args") return vy_args();

  // --------------------------------------------------------------- generic
  if (q == "len") {
    VyValue v = ARG(0);
    switch (vy_tagof(v)) {
      case VY_STRING: return vy_int(v.str->len);
      case VY_LIST:   return vy_int(v.list->len);
      case VY_MAP:    return vy_int(v.map->len);
      default: return bad(std::string("len() needs a String, List or Map, got ") +
                         vy_type_name(v));
    }
  }
  if (q == "type")  return vy_str_val(vy_type_name(ARG(0)));
  if (q == "str")   return vy_str(vy_render(ARG(0)));
  if (q == "int")   { VyValue n = to_number(ARG(0));
                      return vy_tagof(n) == VY_FLOAT ? vy_int((int64_t)n.f) : n; }
  if (q == "float") { VyValue n = to_number(ARG(0));
                      return vy_tagof(n) == VY_INT ? vy_float((double)n.i) : n; }
  if (q == "bool")  return vy_bool(vy_truthy(ARG(0)));
  if (q == "assert") {
    if (!vy_truthy(ARG(0)))
      return bad(args.size() > 1 ? str_arg(eval(args[1], env)) : "assertion failed");
    return vy_nil();
  }
  if (q == "exit") {
    VyValue v = ARG(0);
    signal_.flow = Flow::Exit;
    exit_code_ = vy_isnil(v) ? 0 : (vy_tagof(v) == VY_FLOAT ? (int)v.f : (int)v.i);
    return vy_nil();
  }
  if (q == "throw") {
    Throw t;
    t.value = args.empty() ? vy_str_val("thrown") : eval(args[0], env);
    throw t;
  }

  // ------------------------------------------------------------------- json
  if (q == "json.parse" || q == "parseJson") {
    std::string text = str_arg(ARG(0));
    if (!vy_json_valid(text.c_str(), text.size()))
      return bad("invalid JSON: " + text.substr(0, text.size() < 80 ? text.size() : 80));
    return vy_json_parse(text.c_str(), text.size());
  }
  if (q == "json.stringify") {
    VyValue v = ARG(0);
    return vy_str(vy_json_stringify(v));
  }
  if (q == "json.valid") {
    std::string text = str_arg(ARG(0));
    return vy_bool(vy_json_valid(text.c_str(), text.size()));
  }

  // ------------------------------------------------------------------- http
  if (q.rfind("http.", 0) == 0) {
    std::string m = q.substr(5);
    for (auto& c : m) c = (char)toupper((unsigned char)c);
    VyValue url = ARG(0);
    VyValue json_body = vy_nil(), params = vy_nil();
    if (args.size() >= 2 && m != "GET" && m != "DELETE" && m != "HEAD")
      json_body = ARG(1);
    else if (args.size() >= 2)
      params = ARG(1);
    VyValue body = NAMED("body");
    if (vy_isnil(json_body)) json_body = NAMED("json");
    VyValue headers = NAMED("headers");
    VyValue timeout = NAMED("timeout");
    VyValue ctype = NAMED("content_type");
    VyValue named_params = NAMED("params");
    if (!vy_isnil(named_params)) params = named_params;
    return http_call(m, url, headers, params, json_body, body, ctype, timeout);
  }

  // --------------------------------------------------------------- strings
  if (q == "upper")   return vy_str(vy_str_upper(ARG(0).str));
  if (q == "lower")   return vy_str(vy_str_lower(ARG(0).str));
  if (q == "trim")    return vy_str(vy_str_trim(ARG(0).str));
  if (q == "split") {
    std::string sep = str_arg(ARG(1));
    return vy_str_split(ARG(0).str, mk_str(sep));
  }
  if (q == "join") {
    VyValue sep = ARG(0), list = ARG(1);
    if (vy_tagof(list) != VY_LIST) return vy_str_val("");
    VyStr* acc = vy_str_new("", 0);
    for (uint32_t i = 0; i < list.list->len; i++) {
      if (i) acc = vy_str_concat(acc, sep.str);
      acc = vy_str_concat(acc, vy_render(list.list->items[i]));
    }
    return vy_str(acc);
  }
  if (q == "startswith") return vy_bool(vy_str_find(ARG(0).str, ARG(1).str, 0) == 0);
  if (q == "endswith") {
    VyStr* s = ARG(0).str;
    VyStr* t = ARG(1).str;
    int64_t at = vy_str_find(s, t, 0);
    return vy_bool(at >= 0 && (size_t)at + t->len == s->len);
  }
  if (q == "contains") {
    VyValue needle = ARG(0), hay = ARG(1);
    if (vy_tagof(hay) == VY_STRING && vy_tagof(needle) == VY_STRING)
      return vy_bool(vy_str_contains(hay.str, needle.str));
    return vy_bool(vy_in(needle, hay));
  }
  if (q == "replace") return vy_str(vy_str_replace(ARG(0).str, ARG(1).str, ARG(2).str));
  if (q == "indexof" || q == "find") {
    VyValue needle = ARG(0), hay = ARG(1);
    if (vy_tagof(hay) == VY_STRING) return vy_int(vy_str_find(hay.str, needle.str, 0));
    if (vy_tagof(hay) == VY_LIST)   return vy_int(vy_list_index(hay.list, needle));
    return vy_int(-1);
  }
  if (q == "repeat")  return vy_str(vy_str_repeat(ARG(0).str, ARG(1).i));
  if (q == "pad") {
    if (vy_tagof(ARG(0)) != VY_STRING) return bad("pad() needs a String first argument");
    VyValue w = ARG(1);
    int64_t width = vy_tagof(w) == VY_FLOAT ? (int64_t)w.f : w.i;
    /* The optional fourth argument names the side; the default pads on the
     * left, matching the s.pad(width) method form. */
    int left = 1;
    if (args.size() > 3 && str_arg(ARG(3)) == "right") left = 0;
    return vy_str(vy_str_pad(ARG(0).str, width, ARG(2).i, left));
  }

  // ------------------------------------------------------------------ lists
  if (q == "range") {
    int64_t lo = 0, hi = 0, step = 1;
    if (args.empty()) return vy_nil();
    if (args.size() == 1) hi = ARG(0).i;
    else if (args.size() == 2) { lo = ARG(0).i; hi = ARG(1).i; }
    else { lo = ARG(0).i; hi = ARG(1).i; step = ARG(2).i; }
    return vy_range(lo, hi, step);
  }
  if (q == "push" || q == "append") {
    VyValue list = ARG(0), v = ARG(1);
    if (vy_tagof(list) != VY_LIST) return bad("push() needs a List");
    vy_list_push(list.list, v);
    return list;
  }
  if (q == "pop")    return vy_list_pop(ARG(0).list);
  if (q == "sort")   { VyValue l = ARG(0); vy_list_sort(l.list); return l; }
  if (q == "reverse"){ VyValue l = ARG(0); vy_list_reverse(l.list); return l; }
  if (q == "list") {
    VyValue v = ARG(0);
    if (vy_tagof(v) == VY_LIST) return v;
    if (vy_isnil(v)) return vy_list(vy_list_new());
    VyList* l = vy_list_new();
    vy_list_push(l, v);
    return vy_list(l);
  }

  // ------------------------------------------------------------------- maps
  if (q == "keys")   { VyValue m = ARG(0); return vy_tagof(m) == VY_MAP ? vy_map_keys(m.map) : vy_nil(); }
  if (q == "values") { VyValue m = ARG(0); return vy_tagof(m) == VY_MAP ? vy_map_values(m.map) : vy_nil(); }
  if (q == "items")  { VyValue m = ARG(0); return vy_tagof(m) == VY_MAP ? vy_list(vy_map_pairs(m.map)) : vy_nil(); }
  if (q == "get")    { VyValue m = ARG(0); return vy_tagof(m) == VY_MAP ? vy_map_get(m.map, ARG(1)) : vy_nil(); }
  if (q == "set") {
    VyValue m = ARG(0);
    if (vy_tagof(m) != VY_MAP) return bad("set() needs a Map");
    vy_map_set(m.map, ARG(1), ARG(2));
    return m;
  }
  if (q == "has")    { VyValue m = ARG(0); return vy_bool(vy_tagof(m) == VY_MAP && vy_map_has(m.map, ARG(1))); }
  if (q == "delete") { VyValue m = ARG(0); return vy_bool(vy_tagof(m) == VY_MAP && vy_map_del(m.map, ARG(1))); }
  if (q == "merge") {
    VyValue a = ARG(0), b = ARG(1);
    if (vy_tagof(a) == VY_MAP && vy_tagof(b) == VY_MAP) {
      VyList* pr = vy_map_pairs(b.map);
      for (uint32_t i = 0; i + 1 < pr->len; i += 2)
        vy_map_set(a.map, pr->items[i], pr->items[i + 1]);
    }
    return a;
  }

  // ------------------------------------------------------------------- math
  if (q == "abs") {
    VyValue v = to_number(ARG(0));
    return vy_tagof(v) == VY_INT ? vy_int(v.i < 0 ? -v.i : v.i) : vy_float(fabs(v.f));
  }
  if (q == "sqrt")  return vy_float(sqrt(as_double(ARG(0))));
  if (q == "floor") { VyValue v = to_number(ARG(0));
                      return vy_tagof(v) == VY_INT ? v : vy_int((int64_t)floor(as_double(v))); }
  if (q == "ceil")  { VyValue v = to_number(ARG(0));
                      return vy_tagof(v) == VY_INT ? v : vy_int((int64_t)ceil(as_double(v))); }
  if (q == "round") { VyValue v = to_number(ARG(0));
                      return vy_tagof(v) == VY_INT ? v : vy_int((int64_t)llround(as_double(v))); }
  if (q == "pow")   return vy_float(pow(as_double(ARG(0)), as_double(ARG(1))));
  if (q == "min" || q == "max") {
    VyValue a = ARG(0), b = ARG(1);
    int c = vy_cmp(a, b);
    if (c == 0) return a;
    return ((q == "min") == (c < 0)) ? a : b;
  }
  if (q == "sum") {
    VyValue v = ARG(0);
    if (vy_tagof(v) != VY_LIST) return vy_int(0);
    bool all_int = true;
    double d = 0;
    int64_t i = 0;
    for (uint32_t k = 0; k < v.list->len; k++) {
      VyValue n = to_number(v.list->items[k]);
      if (vy_tagof(n) == VY_FLOAT) all_int = false;
      double dn = vy_tagof(n) == VY_FLOAT ? n.f : (double)n.i;
      d += dn;
      i += (int64_t)dn;
    }
    return all_int ? vy_int(i) : vy_float(d);
  }

  // ------------------------------------------------------------ time / heap
  if (q == "time.clock" || q == "clock") {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return vy_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
  }
  if (q == "time.now" || q == "now") {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return vy_int((int64_t)ts.tv_sec);
  }
  if (q == "gc") {
    std::string what = args.empty() ? std::string("collect") : str_arg(eval(args[0], env));
    if (what == "stats") {
      std::string out = "live=" + std::to_string(vy_gc_live_bytes()) +
                        " heap=" + std::to_string(vy_gc_heap_bytes()) +
                        " allocs=" + std::to_string(vy_heap_alloc_count());
      return vy_s(out);
    }
    if (what == "reset") { vy_heap_reset_stats(); return vy_nil(); }
    vy_gc_collect();
    return vy_nil();
  }

  *handled = false;
  return vy_nil();
}

}  // namespace vy