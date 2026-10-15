// Ovyth :: compiler/lsp/lsp.cpp
// The language server: the compiler's front end, exposed over LSP.
//
// `make lsp` builds editors/vscode/bin/ovyth-lsp, a JSON-RPC loop over stdio
// that reuses lexer.cpp, parser.cpp and sema.cpp directly. The diagnostics in
// the editor are the diagnostics `ovc check` prints -- from the same code, not
// a parallel implementation that will drift from the compiler.
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"

namespace {

// ---- json -----------------------------------------------------------------
// A hand-rolled reader and writer, because the messages are shallow and
// well-formed and a single header dependency is not worth the build weight
// for what amounts to string plumbing.

struct JValue {
  enum Kind { Null, Bool, Num, Str, Arr, Obj };
  Kind kind = Null;
  bool boolean = false;
  double number = 0;
  std::string str;
  std::vector<JValue> arr;
  std::vector<std::pair<std::string, JValue>> obj;

  const JValue* find(const char* key) const {
    if (kind != Obj) return nullptr;
    for (const auto& kv : obj)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

class JParser {
 public:
  explicit JParser(const std::string& src) : s_(src) {}

  // One object per frame; trailing bytes after it are ignored.
  bool parse(JValue& out) {
    skip_ws();
    return value(out);
  }

 private:
  void skip_ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' ||
                              s_[i_] == '\r'))
      ++i_;
  }
  bool end() const { return i_ >= s_.size(); }
  char peek() const { return s_[i_]; }

  bool lit(const char* word, JValue& v, JValue::Kind kind) {
    size_t n = std::strlen(word);
    if (s_.compare(i_, n, word) != 0) return false;
    i_ += n;
    v.kind = kind;
    return true;
  }

  bool string(std::string& out) {
    if (end() || peek() != '"') return false;
    ++i_;
    while (!end()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (end()) return false;
      char e = s_[i_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u':
          // URIs rarely carry raw non-ASCII, and the frames that do carry it
          // are the ones this server never replies to. Fold to '?' rather than
          // implement UTF-16 pair assembly.
          if (i_ + 4 > s_.size()) return false;
          i_ += 4;
          out += '?';
          break;
        default: return false;
      }
    }
    return false;
  }

  bool value(JValue& v) {
    skip_ws();
    if (end()) return false;
    char c = peek();
    if (c == '{') {
      ++i_;
      v.kind = JValue::Obj;
      skip_ws();
      if (!end() && peek() == '}') { ++i_; return true; }
      for (;;) {
        skip_ws();
        std::string key;
        if (!string(key)) return false;
        skip_ws();
        if (end() || peek() != ':') return false;
        ++i_;
        JValue child;
        if (!value(child)) return false;
        v.obj.emplace_back(std::move(key), std::move(child));
        skip_ws();
        if (end()) return false;
        if (peek() == ',') { ++i_; continue; }
        if (peek() == '}') { ++i_; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++i_;
      v.kind = JValue::Arr;
      skip_ws();
      if (!end() && peek() == ']') { ++i_; return true; }
      for (;;) {
        JValue child;
        if (!value(child)) return false;
        v.arr.push_back(std::move(child));
        skip_ws();
        if (end()) return false;
        if (peek() == ',') { ++i_; continue; }
        if (peek() == ']') { ++i_; return true; }
        return false;
      }
    }
    if (c == '"') {
      v.kind = JValue::Str;
      return string(v.str);
    }
    if (lit("true", v, JValue::Bool)) { v.boolean = true; return true; }
    if (lit("false", v, JValue::Bool)) { v.boolean = false; return true; }
    if (lit("null", v, JValue::Null)) return true;
    // numbers
    char* endp = nullptr;
    double n = std::strtod(s_.c_str() + i_, &endp);
    if (endp == s_.c_str() + i_) return false;
    i_ = static_cast<size_t>(endp - s_.c_str());
    v.kind = JValue::Num;
    v.number = n;
    return true;
  }

  const std::string& s_;
  size_t i_ = 0;
};

void append_escaped(std::string& out, const std::string& s) {
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
}

std::string q(const std::string& s) {
  std::string out = "\"";
  append_escaped(out, s);
  out += "\"";
  return out;
}

std::string join(const std::vector<std::string>& parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += ",";
    out += parts[i];
  }
  return out;
}

// ---- wire -----------------------------------------------------------------

std::mutex g_out_mu;

void send(const std::string& body) {
  std::lock_guard<std::mutex> lk(g_out_mu);
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
  std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
  std::cout.flush();
}

bool read_message(std::string& body) {
  int content_length = -1;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;  // blank line ends the headers
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    if (line.substr(0, colon) == "Content-Length")
      content_length = std::atoi(line.c_str() + colon + 1);
  }
  if (content_length <= 0) return false;
  body.resize(static_cast<size_t>(content_length));
  std::cin.read(body.data(), content_length);
  return static_cast<size_t>(std::cin.gcount()) == body.size();
}

std::string uri_to_path(const std::string& uri) {
  std::string p = uri;
  const std::string prefix = "file://";
  if (p.rfind(prefix, 0) == 0) p = p.substr(prefix.size());
  std::string out;
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i] == '%' && i + 2 < p.size()) {
      out += static_cast<char>(std::strtol(p.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += p[i];
    }
  }
  return out;
}

// ---- the compiler, as an analyzer -----------------------------------------

// What an analysis pass produces, copied out of the compiler's own structures
// so a completion request never needs to re-run the front end.
struct Analysis {
  std::vector<ov::Diagnostic> diags;
  std::map<std::string, ov::FuncSig> functions;   // name -> signature
  std::map<std::string, ov::Ty> globals;          // name -> inferred type
  std::set<std::string> identifiers;              // every IDENT the lexer saw
};

Analysis analyze(const std::string& src, const std::string& file) {
  Analysis out;
  try {
    ov::Lexer lexer(src, file);
    std::vector<ov::Token> toks = lexer.scan();
    // Identifiers straight from the scanner: sema.globals only records a
    // name whose type is known (an Unknown-typed assignment leaves it out),
    // and completion needs those names anyway.
    for (const auto& t : toks)
      if (t.kind == ov::Tok::IDENT) out.identifiers.insert(std::string(t.text));
    ov::Parser parser(std::move(toks), file);
    ov::ast::Program prog = parser.parse();
    for (const auto& d : lexer.diagnostics()) out.diags.push_back(d);
    for (const auto& d : prog.diags) out.diags.push_back(d);
    ov::Sema sema;
    sema.run(prog);
    for (const auto& d : sema.diagnostics()) out.diags.push_back(d);
    for (const auto& kv : sema.functions()) out.functions[kv.first] = kv.second;
    for (const auto& kv : sema.globals) out.globals[kv.first] = kv.second;
  } catch (const ov::CompileError& e) {
    out.diags.push_back(
        ov::Diagnostic{e.file, e.line, e.col, e.what(), ov::DiagKind::Error});
  } catch (const ov::ParseAbort&) {
    // parse() records its diagnostics in program.diags and unwinds here
  }
  return out;
}

// ---- what the language knows by name ---------------------------------------
// The interpreter resolves these by convention, with no declaration anywhere
// in the user's file (interp_builtins.cpp and codegen_c.cpp), so completion
// lists them from a table rather than from analysis.

const std::vector<std::string> kBareBuiltins = {
    "abs", "append", "args", "assert", "bool", "ceil", "contains", "delete",
    "endswith", "env", "env_or", "eprint", "exit", "find", "float", "floor",
    "gc", "get", "getenv", "getenv_or", "has", "indexof", "input", "int",
    "items", "join", "keys", "len", "list", "lower", "max", "merge", "min",
    "pad", "pop", "pow", "print", "push", "range", "repeat", "replace",
    "reverse", "round", "set", "setenv", "sleep", "sort", "split", "sqrt",
    "startswith", "str", "sum", "throw", "trim", "type", "upper", "values",
};

// The namespaces the runtime installs as globals (interp_stmt.cpp), with the
// members their backends dispatch on. Used after `ns.` is typed.
const std::map<std::string, std::vector<std::string>> kNamespaces = {
    {"os",   {"run", "exec", "pipe", "read", "write", "close", "stat", "tty"}},
    {"file", {"read", "write", "exists", "append", "delete", "open", "seek"}},
    {"json", {"parse", "stringify", "valid", "get", "get_int", "get_float", "extract"}},
    {"time", {"clock", "now"}},
    {"http", {"get", "head", "body", "headers", "params", "timeout", "content_type"}},
    {"str",  {"upper", "lower", "trim", "split", "replace", "pad", "repeat",
              "contains", "startswith", "endswith", "indexof", "find"}},
    {"math", {"abs", "ceil", "floor", "round", "sqrt", "pow", "min", "max", "sum"}},
};

// ---- importable modules ----------------------------------------------------
// Three sources, in the order the module loader tries them: the registry
// index (every published package), the installed store, and plain .ov files
// sitting next to the file being edited.

std::string ovyth_home() {
  const char* env = std::getenv("OVYTH_HOME");
  if (env && *env) return env;
  const char* home = std::getenv("HOME");
  if (!home || !*home) return std::string();
  return std::string(home) + "/.ovyth";
}

std::string dir_name_of(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::vector<std::string> list_ov_files(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".ov") == 0)
      out.push_back(name.substr(0, name.size() - 3));
  }
  ::closedir(d);
  return out;
}

std::vector<std::string> list_dirs(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;
  while (dirent* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;   // .staging-*, .ovpkg.json
    struct stat st;
    if (::stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      out.push_back(name);
  }
  ::closedir(d);
  return out;
}

std::vector<std::string> registry_packages() {
  std::string home = ovyth_home();
  if (home.empty()) return {};
  std::string path = home + "/registry.json";
  std::string body;
  {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    body = ss.str();
  }
  JValue root;
  JParser jp(body);
  if (!jp.parse(root)) return {};
  const JValue* pkgs = root.find("packages");
  if (!pkgs || pkgs->kind != JValue::Obj) return {};
  std::vector<std::string> out;
  for (const auto& kv : pkgs->obj) out.push_back(kv.first);
  return out;
}

// The module list changes only when packages are installed or files appear,
// so it is cached for a few seconds instead of being rebuilt per keystroke.
std::vector<std::string> importable_modules(const std::string& doc_path) {
  static std::map<std::string, std::vector<std::string>> cache;
  static std::map<std::string, time_t> cached_at;
  static std::mutex mu;

  std::lock_guard<std::mutex> lk(mu);
  time_t now = std::time(nullptr);
  auto hit = cache.find(doc_path);
  if (hit != cache.end() && now - cached_at[doc_path] < 5) return hit->second;

  std::set<std::string> names;
  for (const auto& n : registry_packages()) names.insert(n);
  std::string home = ovyth_home();
  if (!home.empty())
    for (const auto& n : list_dirs(home + "/packages")) names.insert(n);
  for (const auto& n : list_ov_files(dir_name_of(doc_path))) names.insert(n);

  std::vector<std::string> out(names.begin(), names.end());
  cache[doc_path] = out;
  cached_at[doc_path] = now;
  return out;
}

// ---- documents ------------------------------------------------------------
// didOpen/didChange only mark a document dirty; analysis happens on a worker
// thread after a short debounce. Re-lexing on every keystroke is fine for a
// 40-line script and visibly laggy for tui.ov.

struct Docs {
  std::mutex mu;
  std::map<std::string, std::string> text;    // uri -> contents
  std::map<std::string, long long> version;   // uri -> document version
  std::set<std::string> dirty;
  std::condition_variable cv;
  bool stopping = false;
};

// The last analysis per document, so completion answers from data the front
// end already produced rather than re-running it on every request.
std::mutex g_analysis_mu;
std::map<std::string, Analysis> g_analysis;

Analysis analysis_for(const std::string& uri) {
  std::lock_guard<std::mutex> lk(g_analysis_mu);
  auto it = g_analysis.find(uri);
  return it == g_analysis.end() ? Analysis{} : it->second;
}

void publish(const std::string& uri, long long version,
             const std::vector<ov::Diagnostic>& diags) {
  std::string body =
      "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
      "\"params\":{\"uri\":" + q(uri) + ",\"version\":" +
      std::to_string(version) + ",\"diagnostics\":[";
  for (size_t i = 0; i < diags.size(); ++i) {
    const ov::Diagnostic& d = diags[i];
    long line = d.line > 0 ? d.line - 1 : 0;   // LSP is 0-based
    long col = d.col > 0 ? d.col - 1 : 0;
    int sev = d.kind == ov::DiagKind::Error     ? 1
              : d.kind == ov::DiagKind::Warning ? 2
              : d.kind == ov::DiagKind::Info    ? 3
                                                : 4;
    if (i) body += ",";
    body += "{\"range\":{\"start\":{\"line\":" + std::to_string(line) +
            ",\"character\":" + std::to_string(col) + "},\"end\":{\"line\":" +
            std::to_string(line) + ",\"character\":" + std::to_string(col + 1) +
            "}},\"severity\":" + std::to_string(sev) + ",\"source\":\"ovyth\",\"message\":" +
            q(d.message) + "}";
  }
  body += "]}}";
  send(body);
}

void analyze_worker(Docs& docs) {
  for (;;) {
    std::vector<std::tuple<std::string, long long, std::string>> batch;
    {
      std::unique_lock<std::mutex> lk(docs.mu);
      docs.cv.wait(lk, [&] { return docs.stopping || !docs.dirty.empty(); });
      if (docs.stopping && docs.dirty.empty()) return;
      for (const auto& uri : docs.dirty) {
        auto it = docs.text.find(uri);
        if (it == docs.text.end()) continue;
        batch.emplace_back(uri, docs.version[uri], it->second);
      }
      docs.dirty.clear();
    }
    // Analyze outside the lock: the snapshot is already taken, and a change
    // that arrives mid-analysis marks the document dirty again, so the last
    // state is always what ends up published.
    for (const auto& [uri, version, src] : batch) {
      Analysis result = analyze(src, uri_to_path(uri));
      publish(uri, version, result.diags);
      std::lock_guard<std::mutex> lk(g_analysis_mu);
      g_analysis[uri] = std::move(result);
    }
  }
}

// ---- completion ------------------------------------------------------------
// LSP positions are line/character, where character counts UTF-16 code units.
// Ovyth sources are UTF-8 bytes, so a character offset needs converting before
// it can index the text.
size_t utf16_to_byte(const std::string& s, size_t utf16) {
  size_t b = 0, u = 0;
  while (b < s.size() && u < utf16) {
    unsigned char c = static_cast<unsigned char>(s[b]);
    size_t adv = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    u += (c < 0x80 || adv < 4) ? 1 : 2;
    b += adv;
  }
  return b;
}

std::string json_item(const std::string& label, int kind, const std::string& detail,
                      const std::string& doc = "") {
  std::string out = "{\"label\":" + q(label) + ",\"kind\":" + std::to_string(kind);
  if (!detail.empty()) out += ",\"detail\":" + q(detail);
  if (!doc.empty()) out += ",\"documentation\":" + q(doc);
  out += "}";
  return out;
}

std::string complete(const std::string& uri, const std::string& text, long line,
                     long character) {
  // turn line/character into an offset into the document text
  size_t offset = 0;
  long l = 0;
  size_t line_start = std::string::npos;
  for (size_t i = 0; i <= text.size() && l <= line; ++i) {
    if (l == line) {
      line_start = i;
      break;
    }
    if (i < text.size() && text[i] == '\n') ++l;
  }
  if (line_start == std::string::npos) return "[]";
  size_t line_end = text.find('\n', line_start);
  if (line_end == std::string::npos) line_end = text.size();
  std::string line_text = text.substr(line_start, line_end - line_start);
  offset = line_start + utf16_to_byte(line_text, static_cast<size_t>(character));
  std::string prefix = text.substr(line_start, offset - line_start);
  std::string word;

  std::vector<std::string> items;

  // 1. `import "` -- the module names the loader can resolve. The keyword
  // must start the line (whitespace aside) so `import` appearing in, say, a
  // string does not turn every subsequent token into a module.
  size_t p = prefix.find_first_not_of(" \t");
  if (p != std::string::npos && prefix.compare(p, 6, "import") == 0 &&
      (p + 6 == prefix.size() || prefix[p + 6] == ' ' || prefix[p + 6] == '\t')) {
    size_t qpos = prefix.find('"', p + 6);
    if (qpos != std::string::npos && prefix.find('"', qpos + 1) == std::string::npos) {
      word = prefix.substr(qpos + 1);
      for (const auto& m : importable_modules(uri_to_path(uri)))
        if (m.compare(0, word.size(), word) == 0)
          items.push_back(json_item(m, 9, "module", "import \""));
      return "[" + join(items) + "]";
    }
  }

  // 2. `<namespace>.` -- the methods that namespace dispatches.
  size_t dot = prefix.find_last_of('.');
  if (dot != std::string::npos) {
    size_t s = dot;
    while (s > 0 && (std::isalnum(static_cast<unsigned char>(prefix[s - 1])) ||
                     prefix[s - 1] == '_'))
      --s;
    std::string ns = prefix.substr(s, dot - s);
    auto it = kNamespaces.find(ns);
    if (it != kNamespaces.end()) {
      for (const auto& m : it->second) items.push_back(json_item(ns + "." + m, 3, ns + " method"));
      return "[" + join(items) + "]";
    }
  }

  // 3. an identifier -- sema's view of this file plus the bare builtins.
  size_t w = offset;
  while (w > line_start &&
         (std::isalnum(static_cast<unsigned char>(text[w - 1])) || text[w - 1] == '_'))
    --w;
  word = text.substr(w, offset - w);

  Analysis a = analysis_for(uri);
  std::set<std::string> seen;
  for (const auto& kv : a.functions) {
    const ov::FuncSig& f = kv.second;
    if (kv.first.compare(0, word.size(), word) != 0) continue;
    seen.insert(kv.first);
    std::string detail = "(";
    for (size_t i = 0; i < f.params.size(); ++i) {
      if (i) detail += ", ";
      detail += f.params[i];
    }
    detail += f.variadic ? ", ...)" : ")";
    if (f.ret != ov::Ty::Unknown) detail += std::string(" -> ") + ov::ty_name(f.ret);
    items.push_back(json_item(kv.first, 3, detail, "declared on line " + std::to_string(f.decl_line)));
  }
  for (const auto& kv : a.globals) {
    if (kv.first.compare(0, word.size(), word) != 0) continue;
    seen.insert(kv.first);
    items.push_back(json_item(kv.first, 6, ov::ty_name(kv.second), "global variable"));
  }
  for (const auto& b : kBareBuiltins) {
    if (b.compare(0, word.size(), word) != 0) continue;
    seen.insert(b);
    items.push_back(json_item(b, 3, "builtin"));
  }
  for (const auto& kv : kNamespaces) {
    if (kv.first.compare(0, word.size(), word) != 0) continue;
    seen.insert(kv.first);
    std::string detail = "namespace: ";
    for (size_t i = 0; i < kv.second.size(); ++i) {
      if (i) detail += ", ";
      detail += kv.second[i];
    }
    items.push_back(json_item(kv.first, 9, detail));
  }
  for (const auto& id : a.identifiers) {
    if (seen.count(id) || id.compare(0, word.size(), word) != 0) continue;
    seen.insert(id);
    items.push_back(json_item(id, 6, "identifier"));
  }
  return "[" + join(items) + "]";
}

// ---- handlers -------------------------------------------------------------

void respond(const std::string& id_json, const std::string& result) {
  if (id_json.empty()) return;
  send("{\"jsonrpc\":\"2.0\",\"id\":" + id_json + ",\"result\":" + result + "}");
}

const std::string* str_at(const JValue* obj, const char* key) {
  const JValue* v = obj ? obj->find(key) : nullptr;
  return v && v->kind == JValue::Str ? &v->str : nullptr;
}

}  // namespace

int main() {
  std::ios::sync_with_stdio(false);
  Docs docs;
  std::thread worker(analyze_worker, std::ref(docs));

  auto mark_dirty = [&](const std::string& uri) {
    std::lock_guard<std::mutex> lk(docs.mu);
    docs.dirty.insert(uri);
    docs.cv.notify_one();
  };

  std::string body;
  while (read_message(body)) {
    JParser jp(body);
    JValue msg;
    if (!jp.parse(msg)) continue;

    std::string id_json;
    if (const JValue* id = msg.find("id")) {
      if (id->kind == JValue::Num) id_json = std::to_string(static_cast<long long>(id->number));
      else if (id->kind == JValue::Str) id_json = q(id->str);
    }

    const JValue* params = msg.find("params");
    const std::string* method = str_at(&msg, "method");
    if (!method) continue;
    const std::string& m = *method;

    if (m == "initialize") {
      respond(id_json,
              "{\"capabilities\":{"
              "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
              "\"completionProvider\":{\"triggerCharacters\":[\".\",\"\\\"\"]},"
              "\"positionEncoding\":\"utf-16\"},"
              "\"serverInfo\":{\"name\":\"ovyth-lsp\",\"version\":\"0.1.0\"}}");
    } else if (m == "initialized" || m == "$/setTrace" ||
               m == "workspace/didChangeConfiguration" ||
               m == "window/workDoneProgress/cancel") {
      // notifications with nothing to do
    } else if (m == "shutdown") {
      respond(id_json, "null");
    } else if (m == "exit") {
      break;
    } else if (m == "textDocument/didOpen") {
      const JValue* td = params ? params->find("textDocument") : nullptr;
      const std::string* uri = str_at(td, "uri");
      const std::string* text = str_at(td, "text");
      if (uri && text) {
        std::lock_guard<std::mutex> lk(docs.mu);
        docs.text[*uri] = *text;
        docs.version[*uri] =
            td->find("version") ? static_cast<long long>(td->find("version")->number) : 0;
      }
      if (uri) mark_dirty(*uri);
    } else if (m == "textDocument/didChange") {
      // change == 1 (Full): the whole text arrives in contentChanges[0].
      const JValue* td = params ? params->find("textDocument") : nullptr;
      const JValue* changes = params ? params->find("contentChanges") : nullptr;
      const std::string* uri = str_at(td, "uri");
      const std::string* text =
          (changes && !changes->arr.empty()) ? str_at(&changes->arr[0], "text") : nullptr;
      if (uri && text) {
        std::lock_guard<std::mutex> lk(docs.mu);
        docs.text[*uri] = *text;
        const JValue* v = td->find("version");
        docs.version[*uri] = v ? static_cast<long long>(v->number) : 0;
      }
      if (uri) mark_dirty(*uri);
    } else if (m == "textDocument/didClose") {
      const JValue* td = params ? params->find("textDocument") : nullptr;
      const std::string* uri = str_at(td, "uri");
      if (uri) {
        {
          std::lock_guard<std::mutex> lk(docs.mu);
          docs.text.erase(*uri);
          docs.version.erase(*uri);
        }
        {
          std::lock_guard<std::mutex> lk(g_analysis_mu);
          g_analysis.erase(*uri);
        }
        publish(*uri, -1, {});  // clear the squiggles for a closed file
      }
    } else if (m == "textDocument/completion") {
      const JValue* td = params ? params->find("textDocument") : nullptr;
      const JValue* pos = params ? params->find("position") : nullptr;
      const std::string* uri = str_at(td, "uri");
      std::string text;
      if (uri) {
        std::lock_guard<std::mutex> lk(docs.mu);
        auto it = docs.text.find(*uri);
        if (it != docs.text.end()) text = it->second;
      }
      const JValue* line_v = pos ? pos->find("line") : nullptr;
      const JValue* char_v = pos ? pos->find("character") : nullptr;
      if (uri && line_v && char_v) {
        long line = static_cast<long>(line_v->number);
        long character = static_cast<long>(char_v->number);
        respond(id_json, complete(*uri, text, line, character));
      }
    } else if (!id_json.empty()) {
      // Unknown request: answer null rather than leave the client waiting.
      respond(id_json, "null");
    }
  }

  {
    std::lock_guard<std::mutex> lk(docs.mu);
    docs.stopping = true;
  }
  docs.cv.notify_all();
  worker.join();
  return 0;
}
