#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "interp.h"
/* The native codegen delegates string/list builtins to these helpers; using
 * the same entry points here is what keeps the two backends identical. */
#include "ovrt_helpers.h"

namespace ov {
using namespace ast;

// The stdlib. Every builtin is a plain function here, and the LLVM backend
// emits calls to exactly the same C entry points, so the two backends cannot
// drift apart in behaviour.

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static std::string str_arg(OvValue v) {
  if (ov_tagof(v) == OV_STRING) return std::string(v.str->bytes, v.str->len);
  return std::string(ov_str_data(ov_render(v)));
}

static OvStr* mk_str(const std::string& s) { return ov_str_new(s.data(), s.size()); }

static OvValue to_number(OvValue v) {
  switch (ov_tagof(v)) {
    case OV_INT:   return v;
    case OV_FLOAT: return v;
    case OV_BOOL:  return ov_int(v.b);
    case OV_STRING: {
      const char* p = ov_str_data(v.str);
      char* end = nullptr;
      double d = strtod(p, &end);
      if (end && end != p && *end == '\0')
        return (strchr(p, '.') || strchr(p, 'e') || strchr(p, 'E'))
                   ? ov_float(d)
                   : ov_int((int64_t)d);
      break;
    }
    default: break;
  }
  Interp::Throw t;
  std::string what = ov_tagof(v) == OV_STRING
                         ? "\"" + str_arg(v) + "\""
                         : std::string(ov_type_name(v));
  t.value = ov_s(std::string("cannot read a number from ") + what);
  throw t;
}

static double as_double(OvValue v) {
  OvValue n = to_number(v);
  return ov_tagof(n) == OV_FLOAT ? n.f : (double)n.i;
}

// ---------------------------------------------------------------------------
// http.*  ->  a map shaped the way the PRD's chatbot example reads it:
//            response.json["choices"][0]["message"]["content"]
// ---------------------------------------------------------------------------
static OvValue http_call(const std::string& method, OvValue url_v,
                         OvValue headers, OvValue params, OvValue json_body,
                         OvValue body, OvValue content_type, OvValue timeout) {
  std::string url = str_arg(url_v);
  if (url.empty()) {
    Interp::Throw t;
    t.value = ov_s(std::string("http.") + method + " requires a url");
    throw t;
  }

  std::string body_str;
  std::string ctype;
  OvStr* body_keep = nullptr;  // keep the serialised body alive across the call
  if (!ov_isnil(json_body)) {
    body_keep = ov_json_stringify(json_body);
    body_str = ov_str_data(body_keep);
    ctype = "application/json";
    if (!ov_isnil(content_type)) ctype = str_arg(content_type);
  } else if (!ov_isnil(body)) {
    if (ov_tagof(body) == OV_STRING) body_str = str_arg(body);
    else {
      body_keep = ov_json_stringify(body);
      body_str = ov_str_data(body_keep);
      ctype = "application/json";
    }
    if (!ov_isnil(content_type)) ctype = str_arg(content_type);
  }

  // Keep the serialised header/param/body JSON alive until ov_http_request
  // returns: the char* views below point into those allocations. The GC can
  // only see OvValue slots (plus what we hand to ov_gc_collect_ex), so pin
  // them explicitly around the request.
  OvStr* hdr_keep = nullptr;
  std::string hdr_json;
  bool has_hdr = false;
  if (!ov_isnil(headers)) {
    hdr_keep = ov_json_stringify(headers);
    hdr_json = ov_str_data(hdr_keep);
    has_hdr = true;
  }
  OvStr* par_keep = nullptr;
  std::string par_json;
  bool has_par = false;
  if (!ov_isnil(params)) {
    par_keep = ov_json_stringify(params);
    par_json = ov_str_data(par_keep);
    has_par = true;
  }
  double tmo = ov_isnil(timeout)
                   ? 30.0
                   : (ov_tagof(timeout) == OV_FLOAT ? timeout.f : (double)timeout.i);

  OvValue extra[3];
  int ne = 0;
  if (body_keep) extra[ne++] = ov_str(body_keep);
  if (hdr_keep) extra[ne++] = ov_str(hdr_keep);
  if (par_keep) extra[ne++] = ov_str(par_keep);
  // The C library below allocates (headers, buffers); a collection triggered
  // by those would only see raw char* views, so hold the heap quiet and keep
  // the owning strings in `extra` for the explicit collection points.
  ov_gc_begin_mutation();
  OvHttpResponse* r =
      ov_http_request(method.c_str(), url.c_str(),
                      body_str.empty() ? nullptr : body_str.c_str(),
                      ctype.empty() ? nullptr : ctype.c_str(),
                      has_hdr ? hdr_json.c_str() : nullptr,
                      has_par ? par_json.c_str() : nullptr, tmo);
  ov_gc_end_mutation();
  if (!r) {
    Interp::Throw t;
    t.value = ov_str_val("http request could not be allocated");
    throw t;
  }

  /* A transport failure (DNS, TLS, timeout) is raised as a Ovyth error so the
   * program can `try { ... } catch err { }` exactly like a runtime panic. */
  if (r->error) {
    Interp::Throw t;
    t.value = ov_str(r->error);
    throw t;
  }

  OvMap* m = ov_map_new();
  ov_map_set(m, ov_str_val("status"), ov_int(r->status));
  ov_map_set(m, ov_str_val("ok"), ov_bool(r->status >= 200 && r->status < 400));
  ov_map_set(m, ov_str_val("body"), ov_str(r->body));
  ov_map_set(m, ov_str_val("headers"), r->headers);
  ov_map_set(m, ov_str_val("elapsed_ms"), ov_float(r->elapsed_ms));
  return ov_map(m);
}

// ---------------------------------------------------------------------------
// builtin dispatch
// ---------------------------------------------------------------------------
OvValue Interp::call_builtin(const std::string& qualifier, const Expr* site, std::shared_ptr<Env> env,
                             const ExprList& args,
                             const std::vector<NamedArg>& named, bool* handled) {
  *handled = true;
  std::string q = qualifier;

  auto ARG = [&](size_t i) -> OvValue {
    return i < args.size() ? eval(args[i], env) : ov_nil();
  };
  auto NAMED = [&](const char* key) -> OvValue {
    for (const auto& na : named)
      if (na.name == key) return eval(na.value, env);
    return ov_nil();
  };
  auto bad = [&](const std::string& msg) -> OvValue {
    Throw t;
    t.value = ov_s(msg);
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
                         ns == "str"  || ns == "os"   || ns == "time" ||
                         ns == "file");
      if (is_ns) {
        // `http.get(...)`: the namespace has no field of that name, so call the
        // qualified builtin `ns.method`; but a real field (e.g. a user variable
        // shadowing the namespace) is resolved as a value.
        OvValue* slot = env ? env->find(ns) : nullptr;
        bool is_ns_map = slot && ov_tagof(*slot) == OV_MAP;
        if (is_ns_map && !ov_map_has(slot->map, ov_s(callee->name)))
          q = ns + "." + callee->name;
        else {
          OvValue base = eval(callee->a, env);
          return member_get(base, callee->name, callee);
        }
      } else {
        OvValue base = eval(callee->a, env);
        const std::string& f = callee->name;
        OvValue a0 = ARG(0), a1 = ARG(1);
        if (ov_tagof(base) == OV_STRING) {
          OvStr* s = base.str;
          if (f == "upper")      return ov_str(ov_str_upper(s));
          if (f == "lower")      return ov_str(ov_str_lower(s));
          if (f == "trim")       return ov_str(ov_str_trim(s));
          if (f == "split") { std::string sep = str_arg(a0);
                             return ov_str_split(s, mk_str(sep)); }
          if (f == "startswith") return ov_bool(ov_str_find(s, a0.str, 0) == 0);
          if (f == "endswith") {
            int64_t at = ov_str_find(s, a0.str, 0);
            return ov_bool(at >= 0 && (size_t)at + a0.str->len == s->len);
          }
          if (f == "contains")   return ov_bool(ov_str_contains(s, a0.str));
          if (f == "replace")    return ov_str(ov_str_replace(s, a0.str, a1.str));
          if (f == "indexof" || f == "find") {
            // `find(sub, from)`: honour the offset when it is supplied.
            int64_t from = 0;
            if (ov_tagof(a1) == OV_INT) from = a1.i;
            else if (ov_tagof(a1) == OV_FLOAT) from = (int64_t)a1.f;
            return ov_int(ov_str_find(s, a0.str, from));
          }
          if (f == "chars")      return ov_str_chars(s);
          if (f == "bytes")      return ov_str_bytes(s);
          if (f == "repeat")     return ov_str(ov_str_repeat(s, a0.i));
          if (f == "length")     return ov_int(s->len);
          if (f == "slice") {
            /* s.slice(lo, hi) or s.slice(lo). The upper bound defaults to the
             * string length: a nil second argument previously read as 0, so
             * s.slice(3) returned "" instead of the tail. */
            int64_t lo = a0.i;
            int64_t hi = (int64_t)s->len;
            if (ov_tagof(a1) == OV_INT) hi = a1.i;
            else if (ov_tagof(a1) == OV_FLOAT) hi = (int64_t)a1.f;
            return ov_str(ov_str_slice(s, lo, hi));
          }
          if (f == "pad")        return ov_str(ov_str_pad(s, a0.i, a1.i, 1));
        }
        if (ov_tagof(base) == OV_LIST) {
          if (f == "push" || f == "append") { ov_list_push(base.list, a0); return base; }
          if (f == "pop")     return ov_list_pop(base.list);
          if (f == "insert")  { ov_list_insert(base.list, a0.i, a1); return base; }
          if (f == "remove")  return ov_bool(ov_list_remove(base.list, a0.i));
          if (f == "sort")    { ov_list_sort(base.list); return base; }
          if (f == "reverse") { ov_list_reverse(base.list); return base; }
          if (f == "contains")return ov_bool(ov_list_contains(base.list, a0));
          if (f == "indexof") return ov_int(ov_list_index(base.list, a0));
          if (f == "extend") {
            if (ov_tagof(a0) == OV_LIST)
              for (uint32_t i = 0; i < a0.list->len; i++)
                ov_list_push(base.list, a0.list->items[i]);
            return base;
          }
          if (f == "length")  return ov_int(base.list->len);
        }
        if (ov_tagof(base) == OV_MAP) {
          OvMap* mp = base.map;
          if (f == "get")     return ov_map_get(mp, a0);
          if (f == "set")     { ov_map_set(mp, a0, a1); return base; }
          if (f == "has")     return ov_bool(ov_map_has(mp, a0));
          if (f == "delete" || f == "remove") return ov_bool(ov_map_del(mp, a0));
          if (f == "keys")    return ov_map_keys(mp);
          if (f == "values")  return ov_map_values(mp);
          if (f == "items")   return ov_list(ov_map_pairs(mp));
          if (f == "count" || f == "length") return ov_int(mp->len);
          if (f == "merge") {
            if (ov_tagof(a0) == OV_MAP) {
              OvList* pr = ov_map_pairs(a0.map);
              for (uint32_t i = 0; i + 1 < pr->len; i += 2)
                ov_map_set(mp, pr->items[i], pr->items[i + 1]);
            }
            return base;
          }
        }
        return bad(std::string(ov_type_name(base)) + " has no method '" + f + "'");
      }
    } else if (callee->kind != ExprKind::Identifier) {
      *handled = false;
      return ov_nil();
    } else {
      q = callee->name;
    }
  }

  // ---------------------------------------------------------------- console
  if (q == "print") {
    for (size_t i = 0; i < args.size(); i++) {
      if (i) fputc(' ', stdout);
      OvStr* r = ov_render(eval(args[i], env));
      fwrite(r->bytes, 1, r->len, stdout);
    }
    fputc('\n', stdout);
    return ov_nil();
  }
  if (q == "eprint") {
    std::string out;
    for (size_t i = 0; i < args.size(); i++) {
      if (i) out += ' ';
      out += str_arg(eval(args[i], env));
    }
    fprintf(stderr, "%s\n", out.c_str());
    return ov_nil();
  }
  if (q == "input") {
    if (!args.empty()) {
      std::string prompt = str_arg(eval(args[0], env));
      fputs(prompt.c_str(), stdout);
    }
    fflush(stdout);
    OvStr* line = ov_read_line();
    if (!line) return ov_nil();
    return ov_str(line);
  }

  // ------------------------------------------------------------ environment
  if (q == "env" || q == "getenv") {
    std::string key = str_arg(ARG(0));
    const char* v = getenv(key.c_str());
    if (!v) return bad("environment variable '" + key + "' is not set");
    return ov_str_val(v);
  }
  if (q == "env_or" || q == "getenv_or") {
    std::string key = str_arg(ARG(0));
    const char* v = getenv(key.c_str());
    return v ? ov_str_val(v) : ARG(1);
  }
  if (q == "setenv") {
    std::string k = str_arg(ARG(0)), v = str_arg(ARG(1));
    setenv(k.c_str(), v.c_str(), 1);
    return ov_nil();
  }
  if (q == "args") return ov_args();

  // --------------------------------------------------------------- generic
  if (q == "len") {
    OvValue v = ARG(0);
    switch (ov_tagof(v)) {
      case OV_STRING: return ov_int(v.str->len);
      case OV_LIST:   return ov_int(v.list->len);
      case OV_MAP:    return ov_int(v.map->len);
      default: return bad(std::string("len() needs a String, List or Map, got ") +
                         ov_type_name(v));
    }
  }
  if (q == "type")  return ov_str_val(ov_type_name(ARG(0)));
  if (q == "str")   return ov_str(ov_render(ARG(0)));
  if (q == "int")   { OvValue n = to_number(ARG(0));
                      return ov_tagof(n) == OV_FLOAT ? ov_int((int64_t)n.f) : n; }
  if (q == "float") { OvValue n = to_number(ARG(0));
                      return ov_tagof(n) == OV_INT ? ov_float((double)n.i) : n; }
  if (q == "bool")  return ov_bool(ov_truthy(ARG(0)));
  if (q == "assert") {
    if (!ov_truthy(ARG(0)))
      return bad(args.size() > 1 ? str_arg(eval(args[1], env)) : "assertion failed");
    return ov_nil();
  }
  if (q == "exit") {
    OvValue v = ARG(0);
    signal_.flow = Flow::Exit;
    exit_code_ = ov_isnil(v) ? 0 : (ov_tagof(v) == OV_FLOAT ? (int)v.f : (int)v.i);
    return ov_nil();
  }
  if (q == "throw") {
    Throw t;
    t.value = args.empty() ? ov_str_val("thrown") : eval(args[0], env);
    throw t;
  }

  // ------------------------------------------------------------------- json
  if (q == "json.parse" || q == "parseJson") {
    std::string text = str_arg(ARG(0));
    if (!ov_json_valid(text.c_str(), text.size()))
      return bad("invalid JSON: " + text.substr(0, text.size() < 80 ? text.size() : 80));
    return ov_json_parse(text.c_str(), text.size());
  }
  if (q == "json.stringify") {
    OvValue v = ARG(0);
    return ov_str(ov_json_stringify(v));
  }
  if (q == "json.valid") {
    std::string text = str_arg(ARG(0));
    return ov_bool(ov_json_valid(text.c_str(), text.size()));
  }

  // ------------------------------------------------------------------- http
  if (q.rfind("http.", 0) == 0) {
    std::string m = q.substr(5);
    for (auto& c : m) c = (char)toupper((unsigned char)c);
    OvValue url = ARG(0);
    OvValue json_body = ov_nil(), params = ov_nil();
    if (args.size() >= 2 && m != "GET" && m != "DELETE" && m != "HEAD")
      json_body = ARG(1);
    else if (args.size() >= 2)
      params = ARG(1);
    OvValue body = NAMED("body");
    if (ov_isnil(json_body)) json_body = NAMED("json");
    OvValue headers = NAMED("headers");
    OvValue timeout = NAMED("timeout");
    OvValue ctype = NAMED("content_type");
    OvValue named_params = NAMED("params");
    if (!ov_isnil(named_params)) params = named_params;
    return http_call(m, url, headers, params, json_body, body, ctype, timeout);
  }

  // --------------------------------------------------------------- strings
  if (q == "upper")   return ov_str(ov_str_upper(ARG(0).str));
  if (q == "lower")   return ov_str(ov_str_lower(ARG(0).str));
  if (q == "trim")    return ov_str(ov_str_trim(ARG(0).str));
  if (q == "split") {
    std::string sep = str_arg(ARG(1));
    return ov_str_split(ARG(0).str, mk_str(sep));
  }
  if (q == "join") {
    /* Delegate to the same helper the native backend emits, so a join over a
     * specialized array (which the native codegen produces for a homogeneous
     * list literal) and over a plain OvList (what the interpreter builds)
     * produce byte-identical output. */
    return ov_h_join(ARG(0), ARG(1));
  }
  if (q == "startswith") return ov_bool(ov_str_find(ARG(0).str, ARG(1).str, 0) == 0);
  if (q == "endswith") {
    OvStr* s = ARG(0).str;
    OvStr* t = ARG(1).str;
    int64_t at = ov_str_find(s, t, 0);
    return ov_bool(at >= 0 && (size_t)at + t->len == s->len);
  }
  if (q == "contains") {
    OvValue needle = ARG(0), hay = ARG(1);
    if (ov_tagof(hay) == OV_STRING && ov_tagof(needle) == OV_STRING)
      return ov_bool(ov_str_contains(hay.str, needle.str));
    return ov_bool(ov_in(needle, hay));
  }
  if (q == "replace") return ov_str(ov_str_replace(ARG(0).str, ARG(1).str, ARG(2).str));
  if (q == "indexof" || q == "find") {
    OvValue needle = ARG(0), hay = ARG(1);
    /* find(s, sub, from): the third argument is the search offset. It was
     * dropped before, so a caller scanning forward got the first hit forever
     * and could never advance past it. */
    int64_t from = 0;
    if (args.size() > 2) {
      OvValue f = ARG(2);
      from = (ov_tagof(f) == OV_FLOAT) ? (int64_t)f.f : f.i;
    }
    if (ov_tagof(hay) == OV_STRING) {
      if (ov_tagof(needle) != OV_STRING) return bad("find() needs a String pattern");
      return ov_int(ov_str_find(hay.str, needle.str, from));
    }
    if (ov_tagof(hay) == OV_LIST)   return ov_int(ov_list_index(hay.list, needle));
    return ov_int(-1);
  }
  if (q == "repeat")  return ov_str(ov_str_repeat(ARG(0).str, ARG(1).i));
  if (q == "pad") {
    if (ov_tagof(ARG(0)) != OV_STRING) return bad("pad() needs a String first argument");
    OvValue w = ARG(1);
    int64_t width = ov_tagof(w) == OV_FLOAT ? (int64_t)w.f : w.i;
    /* The optional fourth argument names the side; the default pads on the
     * left, matching the s.pad(width) method form. */
    int left = 1;
    if (args.size() > 3 && str_arg(ARG(3)) == "right") left = 0;
    return ov_str(ov_str_pad(ARG(0).str, width, ARG(2).i, left));
  }

  // ------------------------------------------------------------------ lists
  if (q == "range") {
    int64_t lo = 0, hi = 0, step = 1;
    if (args.empty()) return ov_nil();
    if (args.size() == 1) hi = ARG(0).i;
    else if (args.size() == 2) { lo = ARG(0).i; hi = ARG(1).i; }
    else { lo = ARG(0).i; hi = ARG(1).i; step = ARG(2).i; }
    return ov_range(lo, hi, step);
  }
  if (q == "push" || q == "append") {
    OvValue list = ARG(0), v = ARG(1);
    if (ov_tagof(list) != OV_LIST) return bad("push() needs a List");
    ov_list_push(list.list, v);
    return list;
  }
  if (q == "pop")    return ov_list_pop(ARG(0).list);
  if (q == "sort")   { OvValue l = ARG(0); ov_list_sort(l.list); return l; }
  if (q == "reverse"){ OvValue l = ARG(0); ov_list_reverse(l.list); return l; }
  if (q == "list") {
    OvValue v = ARG(0);
    if (ov_tagof(v) == OV_LIST) return v;
    if (ov_isnil(v)) return ov_list(ov_list_new());
    OvList* l = ov_list_new();
    ov_list_push(l, v);
    return ov_list(l);
  }

  // ------------------------------------------------------------------- maps
  if (q == "keys")   { OvValue m = ARG(0); return ov_tagof(m) == OV_MAP ? ov_map_keys(m.map) : ov_nil(); }
  if (q == "values") { OvValue m = ARG(0); return ov_tagof(m) == OV_MAP ? ov_map_values(m.map) : ov_nil(); }
  if (q == "items")  { OvValue m = ARG(0); return ov_tagof(m) == OV_MAP ? ov_list(ov_map_pairs(m.map)) : ov_nil(); }
  if (q == "get")    { OvValue m = ARG(0); return ov_tagof(m) == OV_MAP ? ov_map_get(m.map, ARG(1)) : ov_nil(); }
  if (q == "set") {
    OvValue m = ARG(0);
    if (ov_tagof(m) != OV_MAP) return bad("set() needs a Map");
    ov_map_set(m.map, ARG(1), ARG(2));
    return m;
  }
  if (q == "has")    { OvValue m = ARG(0); return ov_bool(ov_tagof(m) == OV_MAP && ov_map_has(m.map, ARG(1))); }
  if (q == "delete") { OvValue m = ARG(0); return ov_bool(ov_tagof(m) == OV_MAP && ov_map_del(m.map, ARG(1))); }
  if (q == "merge") {
    OvValue a = ARG(0), b = ARG(1);
    if (ov_tagof(a) == OV_MAP && ov_tagof(b) == OV_MAP) {
      OvList* pr = ov_map_pairs(b.map);
      for (uint32_t i = 0; i + 1 < pr->len; i += 2)
        ov_map_set(a.map, pr->items[i], pr->items[i + 1]);
    }
    return a;
  }

  // ------------------------------------------------------------------- math
  if (q == "abs") {
    OvValue v = to_number(ARG(0));
    return ov_tagof(v) == OV_INT ? ov_int(v.i < 0 ? -v.i : v.i) : ov_float(fabs(v.f));
  }
  if (q == "sqrt")  return ov_float(sqrt(as_double(ARG(0))));
  if (q == "floor") { OvValue v = to_number(ARG(0));
                      return ov_tagof(v) == OV_INT ? v : ov_int((int64_t)floor(as_double(v))); }
  if (q == "ceil")  { OvValue v = to_number(ARG(0));
                      return ov_tagof(v) == OV_INT ? v : ov_int((int64_t)ceil(as_double(v))); }
  if (q == "round") { OvValue v = to_number(ARG(0));
                      return ov_tagof(v) == OV_INT ? v : ov_int((int64_t)llround(as_double(v))); }
  if (q == "pow")   return ov_float(pow(as_double(ARG(0)), as_double(ARG(1))));
  if (q == "min" || q == "max") {
    OvValue a = ARG(0), b = ARG(1);
    int c = ov_cmp(a, b);
    if (c == 0) return a;
    return ((q == "min") == (c < 0)) ? a : b;
  }
  if (q == "sum") {
    OvValue v = ARG(0);
    if (ov_tagof(v) != OV_LIST) return ov_int(0);
    bool all_int = true;
    double d = 0;
    int64_t i = 0;
    for (uint32_t k = 0; k < v.list->len; k++) {
      OvValue n = to_number(v.list->items[k]);
      if (ov_tagof(n) == OV_FLOAT) all_int = false;
      double dn = ov_tagof(n) == OV_FLOAT ? n.f : (double)n.i;
      d += dn;
      i += (int64_t)dn;
    }
    return all_int ? ov_int(i) : ov_float(d);
  }

  // ------------------------------------------------------------ time / heap
  if (q == "time.clock" || q == "clock") {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ov_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
  }
  if (q == "time.now" || q == "now") {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ov_int((int64_t)ts.tv_sec);
  }
  
  // ------------------------------------------------------------ file I/O
  if (q == "file.read") {
    std::string path = str_arg(ARG(0));
    OvStr* s = ov_file_read(path.c_str());
    return ov_str(s);
  }
  if (q == "file.write") {
    std::string path = str_arg(ARG(0));
    OvValue data = ARG(1);
    std::string text = str_arg(data);
    int ok = ov_file_write(path.c_str(), text.c_str(), text.size());
    return ov_bool(ok);
  }
  if (q == "file.append") {
    std::string path = str_arg(ARG(0));
    OvValue data = ARG(1);
    std::string text = str_arg(data);
    int ok = ov_file_append(path.c_str(), text.c_str(), text.size());
    return ov_bool(ok);
  }
  if (q == "file.exists") {
    std::string path = str_arg(ARG(0));
    return ov_bool(ov_file_exists(path.c_str()));
  }
  if (q == "file.size") {
    std::string path = str_arg(ARG(0));
    return ov_int(ov_file_size(path.c_str()));
  }
  if (q == "file.delete") {
    std::string path = str_arg(ARG(0));
    return ov_bool(ov_file_delete(path.c_str()));
  }
  if (q == "dir.create") {
    std::string path = str_arg(ARG(0));
    return ov_bool(ov_dir_create(path.c_str()));
  }
  if (q == "dir.list") {
    std::string path = str_arg(ARG(0));
    return ov_dir_list(path.c_str());
  }

  if (q == "gc") {
    std::string what = args.empty() ? std::string("collect") : str_arg(eval(args[0], env));
    if (what == "stats") {
      std::string out = "live=" + std::to_string(ov_gc_live_bytes()) +
                        " heap=" + std::to_string(ov_gc_heap_bytes()) +
                        " allocs=" + std::to_string(ov_heap_alloc_count());
      return ov_s(out);
    }
    if (what == "reset") { ov_heap_reset_stats(); return ov_nil(); }
    ov_gc_collect();
    return ov_nil();
  }

  *handled = false;
  return ov_nil();
}

}  // namespace ov