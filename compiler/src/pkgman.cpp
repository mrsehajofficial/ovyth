// Ovyth :: compiler/src/pkgman.cpp
//
// `ovc pkg ...` — the package manager.
//
// A package is a directory of `.ov` files. `ovc pkg install` copies one into
// the global store ($OVYTH_HOME/packages/<name>, default ~/.ovyth/packages),
// records its origin in <name>/.ovpkg.json, and the module loader falls back
// to that store for any `import` that is not found next to the importing
// file. Python's site-packages, for Ovyth.
//
// Where a package comes from, in order of preference:
//
//   * a local directory  — `ovc pkg install /path/to/pkg` (copy)
//   * a git repository    — registry entry with "repo"        (git clone)
//   * a tarball URL       — registry entry with "url"         (GET + tar x)
//
// The registry is a JSON index. It is fetched over HTTP and cached in the
// store for kCacheTtl seconds; `--refresh` forces a fetch. A `file://` URL
// reads the index straight off disk, which is what tests/pkg.sh uses to drive
// the whole pipeline without a network.

#include "pkgman.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lexer/diagnostic.h"
#include "tools/ui.h"

extern "C" {
#include "ovrt.h"
}

namespace ov {
namespace {

// ---------------------------------------------------------------------------
// filesystem helpers
// ---------------------------------------------------------------------------

bool fs_exists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

bool fs_is_dir(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fs_is_file(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool fs_readable(const std::string& p) {
  return ::access(p.c_str(), R_OK) == 0;
}

// Metadata file the package manager writes into an installed package. Copied
// around with the package itself, never by the user.
constexpr const char* kPkgMeta = ".ovpkg.json";

// The registry index, cached in the store root. A fresh index is fetched at
// most every kCacheTtl seconds; --refresh bypasses the cache entirely.
constexpr const char* kRegistryCache = "registry.json";
constexpr long kCacheTtl = 3600;

// Default registry index, served from this repository over GitHub's raw CDN.
//
// It is deliberately not a service of its own: the index is one small JSON
// file that lives in git beside the compiler, is versioned with it, and is
// served by infrastructure that already exists. Package *code* never touches
// it either -- each entry points at the author's own repository, so the
// downloads are absorbed by whoever hosts the package. Moving to a dedicated
// host later is a change to this one constant.
constexpr const char* kRegistryUrl =
    "https://raw.githubusercontent.com/mrsehajofficial/ovyth/main/registry/"
    "registry.json";

bool mkdir_one(const std::string& p) {
  return ::mkdir(p.c_str(), 0755) == 0 || errno == EEXIST;
}

// A directory can only be created if its parent exists, so creating
// $OVYTH_HOME/packages walks the path one segment at a time.
bool mkdir_p(const std::string& p) {
  if (p.empty()) return false;
  std::string built;
  size_t i = 0;
  if (p[0] == '/') {
    built = "/";
    i = 1;
  }
  while (i < p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    std::string seg = p.substr(i, j - i);
    if (!seg.empty()) {
      if (!built.empty() && built.back() != '/') built += "/";
      built += seg;
      if (!mkdir_one(built)) return false;
    }
    i = j + 1;
  }
  return true;
}

bool write_file(const std::string& path, const char* data, size_t len,
                std::string& err) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    err = "cannot write " + path;
    return false;
  }
  size_t n = std::fwrite(data, 1, len, f);
  std::fclose(f);
  if (n != len) {
    err = "short write to " + path;
    return false;
  }
  return true;
}

bool write_file(const std::string& path, const std::string& body,
                std::string& err) {
  return write_file(path, body.data(), body.size(), err);
}

// Read a whole file. Binary-safe: the length comes from the byte count, not
// from strlen, so tarballs and other payloads survive the round trip.
bool read_file_bytes(const std::string& path, std::string& out,
                     std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot read " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// Depth-first delete. Written by hand rather than shelling out so a package
// name can never turn into a shell command.
bool remove_tree(const std::string& p) {
  struct stat st;
  if (::lstat(p.c_str(), &st) != 0) return true;  // already gone
  if (S_ISDIR(st.st_mode)) {
    DIR* d = ::opendir(p.c_str());
    if (!d) return false;
    bool ok = true;
    while (struct dirent* e = ::readdir(d)) {
      if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
        continue;
      if (!remove_tree(p + "/" + e->d_name)) ok = false;
    }
    ::closedir(d);
    return ok && ::rmdir(p.c_str()) == 0;
  }
  return ::unlink(p.c_str()) == 0;
}

// Recursive copy that skips the package manager's own metadata and a source
// repository's history, so reinstalling over a checkout, or installing a git
// clone, does not drag .git and .ovpkg.json into the store.
bool copy_tree(const std::string& src, const std::string& dst,
               std::string& err) {
  if (!mkdir_p(dst)) {
    err = "cannot create " + dst;
    return false;
  }
  DIR* d = ::opendir(src.c_str());
  if (!d) {
    err = "cannot read directory " + src;
    return false;
  }
  bool ok = true;
  while (struct dirent* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == ".." || name == kPkgMeta || name == ".git")
      continue;
    std::string from = src + "/" + name;
    std::string to = dst + "/" + name;
    if (fs_is_dir(from)) {
      if (!copy_tree(from, to, err)) ok = false;
      continue;
    }
    std::string body;
    if (!read_file_bytes(from, body, err)) {
      ok = false;
      continue;
    }
    if (!write_file(to, body, err)) ok = false;
  }
  ::closedir(d);
  return ok;
}

std::string base_name(const std::string& p) {
  size_t end = p.find_last_of('/');
  return end == std::string::npos ? p : p.substr(end + 1);
}

std::string home_dir() {
  const char* env = ::getenv("HOME");
  if (env && *env) return env;
  return std::string();
}

// $OVYTH_HOME, defaulting to ~/.ovyth. One function so the loader and the
// package manager can never disagree about where packages live.
const std::string& ovyth_home() {
  static std::string home;
  static bool once = false;
  if (!once) {
    once = true;
    const char* env = ::getenv("OVYTH_HOME");
    if (env && *env) {
      home = env;
    } else {
      home = home_dir();
      if (!home.empty()) home += "/.ovyth";
    }
    while (home.size() > 1 && home.back() == '/') home.pop_back();
  }
  return home;
}

std::string utc_now() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[40];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

// ---------------------------------------------------------------------------
// JSON — the runtime's parser does the work; ovc already links libovrt.
// ---------------------------------------------------------------------------

bool json_is_map(OvValue v) { return ov_tagof(v) == OV_MAP; }

// One map entry. ov_map_pairs() hands back [k1, v1, k2, v2, ...], which is
// easier to pair up here than to walk twice.
std::vector<std::pair<std::string, OvValue>> json_entries(OvValue m) {
  std::vector<std::pair<std::string, OvValue>> out;
  if (!json_is_map(m)) return out;
  OvList* pairs = ov_map_pairs(m.map);
  if (!pairs) return out;
  for (uint32_t i = 0; i + 1 < pairs->len; i += 2) {
    OvValue k = pairs->items[i];
    OvValue v = pairs->items[i + 1];
    if (ov_tagof(k) != OV_STRING) continue;
    out.push_back({std::string(ov_str_data(k.str), (size_t)k.str->len), v});
  }
  return out;
}

// "" when the key is absent or not a string — a caller wanting an error for a
// missing key says so explicitly (see index_find).
std::string json_str(OvValue m, const char* key) {
  if (!json_is_map(m)) return std::string();
  OvValue v = ov_map_get(m.map, ov_str_val(key));
  if (ov_tagof(v) != OV_STRING) return std::string();
  return std::string(ov_str_data(v.str), (size_t)v.str->len);
}

// Quote a string for the metadata file we write. Package names, versions and
// paths are tame, but they come from a file on the internet.
std::string json_escape(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += "\"";
  return out;
}

// Lowest-first compare of dotted numeric versions ("1.10.0" > "1.9.0");
// non-numeric suffixes compare as strings so 1.0.0-rc1 sorts before 1.0.0.
int version_cmp(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    long na = 0, nb = 0;
    bool ha = false, hb = false;
    while (i < a.size() && std::isdigit((unsigned char)a[i]))
      na = na * 10 + (a[i++] - '0'), ha = true;
    while (j < b.size() && std::isdigit((unsigned char)b[j]))
      nb = nb * 10 + (b[j++] - '0'), hb = true;
    if (ha || hb) {
      if (na != nb) return na < nb ? -1 : 1;
    } else {
      // Neither side has digits here: compare one character at a time.
      if (i < a.size() && j < b.size()) {
        if (a[i] != b[j]) return (unsigned char)a[i] < (unsigned char)b[j] ? -1 : 1;
        i++;
        j++;
        continue;
      }
      break;
    }
    if (i < a.size() && a[i] == '.') i++;
    if (j < b.size() && b[j] == '.') j++;
  }
  if (i < a.size() || j < b.size()) return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// import resolution (called from ModuleLoader::resolve)
// ---------------------------------------------------------------------------

const std::string& packages_root() {
  static std::string root;
  static bool once = false;
  if (!once) {
    once = true;
    if (!ovyth_home().empty()) root = ovyth_home() + "/packages";
  }
  return root;
}

namespace {

// The entry module of a package directory: `<name>.ov`, or `main.ov`. Used
// both to pick the module an `import "<name>"` resolves to and to reject a
// source that cannot be imported under its own name.
std::string package_entry(const std::string& dir, const std::string& name) {
  const std::string main = "main.ov";
  for (const std::string& rel : {name + ".ov", main}) {
    std::string p = dir + "/" + rel;
    if (fs_readable(p)) return p;
  }
  return std::string();
}

}  // namespace

std::string resolve_package_import(const std::string& path) {
  const std::string& root = packages_root();
  if (root.empty() || path.empty()) return std::string();

  // `import "pkg/mod"` must not be confused with `import "pkg"` — the last
  // segment decides which file the import is for, the earlier ones which
  // package provides it.
  std::string p = root + "/" + path;
  if (fs_readable(p) && fs_is_file(p)) return p;
  if (fs_readable(p) && fs_is_dir(p)) {
    std::string entry = package_entry(p, base_name(path));
    if (!entry.empty()) return entry;
  }
  if (fs_readable(p + ".ov") && fs_is_file(p + ".ov")) return p + ".ov";
  return std::string();
}

namespace {

// ---------------------------------------------------------------------------
// subprocess helper — git clone and tar extraction
// ---------------------------------------------------------------------------

std::string sh_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

// Mirrors Driver::run: capture stdout+stderr together so git's chatty output
// lands in the error message when it fails.
bool run_command(const std::string& cmd, std::string& out, std::string& err) {
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) {
    err = "cannot start: " + cmd;
    return false;
  }
  char line[4096];
  while (std::fgets(line, sizeof(line), pipe)) out += line;
  int rc = ::pclose(pipe);
  if (rc != 0) {
    err = out.empty() ? ("command failed: " + cmd) : out;
    return false;
  }
  return true;
}

bool have_tool(const std::string& tool) {
  std::string out, err;
  return run_command("command -v " + tool + " >/dev/null 2>&1", out, err);
}

// ---------------------------------------------------------------------------
// fetching bytes — file:// for a local index, HTTP otherwise
// ---------------------------------------------------------------------------

// The runtime's HTTP client needs its one-line init before use; the package
// manager runs inside ovc, which is not a compiled Ovyth program.
bool http_get(const std::string& url, std::string& body, std::string& err) {
  if (url.rfind("file://", 0) == 0)
    return read_file_bytes(url.substr(7), body, err);

  ov_runtime_init(0, nullptr);
  OvHttpResponse* r =
      ov_http_request("GET", url.c_str(), nullptr, nullptr, nullptr, nullptr, 30.0);
  if (!r) {
    err = "no response for " + url;
    return false;
  }
  if (r->error) {
    err = std::string(ov_str_data(r->error), (size_t)r->error->len);
    std::free(r);
    return false;
  }
  if (r->status != 200) {
    err = url + " returned HTTP " + std::to_string((long long)r->status);
    std::free(r);
    return false;
  }
  // Binary-safe: the length comes from the response, not from strlen.
  body.assign(ov_str_data(r->body), (size_t)r->body->len);
  std::free(r);
  return true;
}

// ---------------------------------------------------------------------------
// the registry index
// ---------------------------------------------------------------------------

// One version of a package and where to get it. Exactly one of the three
// sources is set; "path" wins over "repo" wins over "url" because a nearby
// directory beats a network round trip.
struct SourceRef {
  std::string path;
  std::string repo;
  std::string url;
  bool any() const { return !path.empty() || !repo.empty() || !url.empty(); }
};

// A registry entry: every published version, oldest first, plus the metadata
// `search` and `info` display.
struct IndexEntry {
  std::string name;
  std::string description;
  std::string license;
  std::string homepage;
  std::vector<std::pair<std::string, SourceRef>> versions;  // (version, source)
};

bool index_parse(const std::string& json, std::vector<IndexEntry>& out,
                 std::string& err) {
  OvValue root = ov_json_parse(json.data(), json.size());
  if (!json_is_map(root)) {
    err = "registry index is not a JSON object";
    return false;
  }
  OvValue packages = ov_map_get(root.map, ov_str_val("packages"));
  if (!json_is_map(packages)) {
    err = "registry index has no \"packages\" object";
    return false;
  }
  for (const auto& [name, val] : json_entries(packages)) {
    IndexEntry e;
    e.name = name;
    e.description = json_str(val, "description");
    e.license = json_str(val, "license");
    e.homepage = json_str(val, "homepage");

    SourceRef base;
    base.path = json_str(val, "path");
    base.repo = json_str(val, "repo");
    base.url = json_str(val, "url");

    OvValue versions = ov_map_get(val.map, ov_str_val("versions"));
    if (json_is_map(versions)) {
      for (const auto& [ver, src] : json_entries(versions)) {
        SourceRef s;
        s.path = json_str(src, "path");
        s.repo = json_str(src, "repo");
        s.url = json_str(src, "url");
        e.versions.push_back({ver, s});
      }
    } else if (base.any()) {
      // A single-version entry: no "versions" map, just the source inline.
      e.versions.push_back({json_str(val, "version"), base});
    }
    std::stable_sort(e.versions.begin(), e.versions.end(),
                     [](const std::pair<std::string, SourceRef>& a,
                        const std::pair<std::string, SourceRef>& b) {
                       return version_cmp(a.first, b.first) < 0;
                     });
    out.push_back(e);
  }
  std::stable_sort(out.begin(), out.end(),
                  [](const IndexEntry& a, const IndexEntry& b) { return a.name < b.name; });
  return true;
}

const IndexEntry* index_find(const std::vector<IndexEntry>& index,
                             const std::string& name) {
  for (const auto& e : index)
    if (e.name == name) return &e;
  return nullptr;
}

// A package the user asked for by the wrong name gets the close matches, not
// a shrug.
std::vector<std::string> index_similar(const std::vector<IndexEntry>& index,
                                      const std::string& name) {
  std::vector<std::string> out;
  for (const auto& e : index)
    if (e.name.find(name) != std::string::npos ||
        name.find(e.name) != std::string::npos)
      out.push_back(e.name);
  return out;
}

std::string registry_cache_path() {
  const std::string& home = ovyth_home();
  return home.empty() ? std::string() : home + "/" + kRegistryCache;
}

// Set by --registry=...; wins over the environment, which wins over the
// default. Parsing happens once in run_pkg_command.
std::string& registry_override() {
  static std::string url;
  return url;
}

std::string registry_url() {
  if (!registry_override().empty()) return registry_override();
  const char* env = ::getenv("OVYTH_REGISTRY");
  if (env && *env) return env;
  return kRegistryUrl;
}

// The index is wrapped before it is cached:
//
//   {"url": "...", "fetched_at": 1760000000, "index": "<the index JSON>"}
//
// Recording the URL is the point: --registry and OVYTH_REGISTRY can point at
// any index, and a cache written for one must never answer for another.
std::string wrap_cache(const std::string& url, const std::string& index) {
  return "{\"url\":" + json_escape(url) +
         ",\"fetched_at\":" + std::to_string((long long)std::time(nullptr)) +
         ",\"index\":" + json_escape(index) + "}\n";
}

// Read the cached index for `url`. `require_fresh` drops it past the TTL; the
// offline fallback reads it at any age.
bool read_cache(const std::string& url, bool require_fresh, std::string& out) {
  std::string cache = registry_cache_path();
  if (cache.empty() || !fs_is_file(cache)) return false;
  std::string body, rerr;
  if (!read_file_bytes(cache, body, rerr)) return false;
  OvValue v = ov_json_parse(body.data(), body.size());
  if (!json_is_map(v)) return false;
  if (json_str(v, "url") != url) return false;
  int64_t fetched = 0;
  OvValue at = ov_map_get(v.map, ov_str_val("fetched_at"));
  if (ov_tagof(at) == OV_INT) fetched = at.i;
  else if (ov_tagof(at) == OV_FLOAT) fetched = (int64_t)at.f;
  if (require_fresh &&
      (fetched <= 0 || std::time(nullptr) - (time_t)fetched >= kCacheTtl))
    return false;
  out = json_str(v, "index");
  return !out.empty();
}

// Load the index: from cache while it is fresh, otherwise fetch it. A failed
// fetch falls back to the cached index of the *same* URL, so `ovc pkg list`
// keeps working with no network; only when there is neither a usable cache
// nor a fetch is this an error.
bool load_index(bool refresh, std::vector<IndexEntry>& out, std::string& err,
                bool* from_cache = nullptr) {
  const std::string& home = ovyth_home();
  if (home.empty()) {
    err = "no home directory: set OVYTH_HOME";
    return false;
  }
  if (!fs_is_dir(home) && !mkdir_p(home)) {
    err = "cannot create " + home;
    return false;
  }

  std::string url = registry_url();
  std::string json;
  if (!refresh && read_cache(url, true, json)) {
    if (from_cache) *from_cache = true;
    return index_parse(json, out, err);
  }

  std::string ferr;
  if (!http_get(url, json, ferr)) {
    if (read_cache(url, false, json)) {
      ov::cli_info("cannot reach " + url + " (" + ferr +
                   "); using the cached index");
      if (from_cache) *from_cache = true;
      return index_parse(json, out, err);
    }
    err = "cannot fetch the registry index from " + url + ": " + ferr +
          "\npoint at another index with --registry=<url> or OVYTH_REGISTRY";
    return false;
  }

  std::string cache = registry_cache_path();
  std::string werr;
  write_file(cache, wrap_cache(url, json), werr);  // best effort
  if (from_cache) *from_cache = false;
  return index_parse(json, out, err);
}

// ---------------------------------------------------------------------------
// installing
// ---------------------------------------------------------------------------

struct InstalledMeta {
  std::string name;
  std::string version;
  std::string source;
  std::string installed_at;
  bool valid = false;
};

InstalledMeta read_meta(const std::string& dir) {
  InstalledMeta m;
  std::string body, err;
  if (!read_file_bytes(dir + "/" + kPkgMeta, body, err)) return m;
  OvValue v = ov_json_parse(body.data(), body.size());
  if (!json_is_map(v)) return m;
  m.name = json_str(v, "name");
  m.version = json_str(v, "version");
  m.source = json_str(v, "source");
  m.installed_at = json_str(v, "installed_at");
  m.valid = !m.name.empty();
  return m;
}

bool write_meta(const std::string& dir, const std::string& name,
                const std::string& version, const std::string& source) {
  std::string body = "{\n";
  body += "  \"name\": " + json_escape(name) + ",\n";
  body += "  \"version\": " + json_escape(version) + ",\n";
  body += "  \"source\": " + json_escape(source) + ",\n";
  body += "  \"installed_at\": " + json_escape(utc_now()) + "\n";
  body += "}\n";
  std::string err;
  return write_file(dir + "/" + kPkgMeta, body, err);
}

std::vector<std::string> dir_children(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;
  while (struct dirent* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    out.push_back(name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

// A package name becomes a directory name under the store, so it is
// restricted to what cannot escape it: no separators, no leading dot, no
// whitespace. A registry entry with a hostile name is rejected at the door
// rather than writing outside $OVYTH_HOME/packages.
bool valid_package_name(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) {
    if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-'))
      return false;
  }
  if (name.front() == '.' || name.find("..") != std::string::npos) return false;
  return true;
}

// "name@1.2.3" — the version is optional everywhere it is accepted.
void parse_spec(const std::string& spec, std::string& name,
                std::string& version) {
  size_t at = spec.rfind('@');
  if (at == std::string::npos) {
    name = spec;
    version.clear();
    return;
  }
  name = spec.substr(0, at);
  version = spec.substr(at + 1);
}

// A local directory is spelled like a path, a registry package like a name.
bool looks_like_path(const std::string& s) {
  return s == "." || s == ".." || s.rfind("/", 0) == 0 ||
         s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0 || s.rfind("~/", 0) == 0;
}

// Long absolute paths are the norm in this UI (~/.ovyth/packages/x), and they
// do not fit a step line. Collapse the home prefix so the interesting part —
// the package name — survives the clip.
std::string shorten_path(const std::string& p) {
  const std::string home = home_dir();
  if (!home.empty() && p.rfind(home, 0) == 0) return "~" + p.substr(home.size());
  return p;
}

std::string expand_user(const std::string& s) {
  if (s.rfind("~/", 0) != 0) return s;
  const std::string home = home_dir();
  if (home.empty()) return s;
  return home + s.substr(1);
}

// Where the installed package will live, and a place to stage it first. The
// staging directory is a sibling of its destination so the final step is a
// rename inside one filesystem, never a cross-device copy.
std::string install_dest(const std::string& name) {
  return packages_root() + "/" + name;
}

std::string staging_dir(const std::string& name) {
  return packages_root() + "/.staging-" + name + "-" +
         std::to_string((long long)::getpid());
}

bool take_destination(const std::string& staged, const std::string& dest,
                      bool force, std::string& err) {
  if (fs_exists(dest)) {
    if (!force) {
      err = "already installed (use --force to reinstall)";
      return false;
    }
    if (!remove_tree(dest)) {
      err = "cannot replace " + dest;
      return false;
    }
  }
  if (::rename(staged.c_str(), dest.c_str()) == 0) return true;
  // Same filesystem in practice; the copy is belt and braces.
  if (!copy_tree(staged, dest, err)) return false;
  remove_tree(staged);
  return true;
}

// --- install: local directory ---------------------------------------------

bool install_from_path(const std::string& name, const std::string& src,
                       const std::string& version, bool force,
                       std::string& err) {
  if (packages_root().empty()) {
    err = "no home directory: set OVYTH_HOME";
    return false;
  }
  if (!fs_is_dir(src)) {
    err = "'" + src + "' is not a directory";
    return false;
  }
  // Check the entry up front: a package that cannot be imported under its own
  // name is a bug in the package, not a discovery for later.
  if (package_entry(src, name).empty()) {
    err = "'" + src + "' has no " + name + ".ov or main.ov to import as \"" +
          name + "\"";
    return false;
  }
  if (!mkdir_p(packages_root())) {
    err = "cannot create " + packages_root();
    return false;
  }

  std::string staged = staging_dir(name);
  remove_tree(staged);
  if (!copy_tree(src, staged, err)) {
    remove_tree(staged);
    return false;
  }
  if (!write_meta(staged, name, version.empty() ? "0.0.0" : version, src)) {
    err = "cannot record the install in " + staged + "/" + kPkgMeta;
    remove_tree(staged);
    return false;
  }
  // A refused install (already present, no --force) fails *after* staging, so
  // the staging tree must be cleaned up here or it lingers in the store and
  // shows up in `ovc pkg list` as a phantom package.
  bool ok = take_destination(staged, install_dest(name), force, err);
  if (!ok) remove_tree(staged);
  return ok;
}

// --- install: git repository ----------------------------------------------

bool install_from_repo(const std::string& name, const std::string& repo,
                       const std::string& version, bool force,
                       std::string& err) {
  if (packages_root().empty()) {
    err = "no home directory: set OVYTH_HOME";
    return false;
  }
  if (!have_tool("git")) {
    err = "git is required to install from a repository";
    return false;
  }
  if (!mkdir_p(packages_root())) {
    err = "cannot create " + packages_root();
    return false;
  }

  std::string staged = staging_dir(name);
  remove_tree(staged);
  if (!mkdir_p(staged)) {
    err = "cannot create " + staged;
    return false;
  }

  // A shallow clone of the version's tag. Tags are the convention, but a bare
  // `v1.2.3` is common enough that it deserves a second attempt.
  bool cloned = false;
  std::string cerr;
  const std::string checkout = staged + "/checkout";
  if (version.empty()) {
    std::string cmd = "git clone --depth 1 " + sh_quote(repo) + " " +
                      sh_quote(checkout) + " 2>&1";
    cloned = run_command(cmd, cerr, cerr);
  } else {
    for (const std::string& ref : {version, "v" + version}) {
      std::string cmd = "git clone --depth 1 --branch " + sh_quote(ref) + " " +
                        sh_quote(repo) + " " + sh_quote(checkout) + " 2>&1";
      std::string out;
      if (run_command(cmd, out, cerr)) {
        cloned = true;
        break;
      }
      cerr = out;
    }
  }
  if (!cloned) {
    err = "git clone failed: " + cerr;
    remove_tree(staged);
    return false;
  }

  if (package_entry(checkout, name).empty()) {
    err = "the repository at " + repo + " has no " + name + ".ov or main.ov " +
          "to import as \"" + name + "\"";
    remove_tree(staged);
    return false;
  }
  // Copy out of the checkout rather than renaming it: that is what drops
  // .git, which the store has no use for.
  if (!copy_tree(checkout, staged + "/final", err)) {
    remove_tree(staged);
    return false;
  }
  if (!write_meta(staged + "/final", name, version.empty() ? "0.0.0" : version,
                  repo)) {
    err = "cannot record the install in " + staged + "/final/" + kPkgMeta;
    remove_tree(staged);
    return false;
  }
  bool ok = take_destination(staged + "/final", install_dest(name), force, err);
  remove_tree(staged);
  if (ok) write_meta(install_dest(name), name, version.empty() ? "0.0.0" : version, repo);
  return ok;
}

// --- install: tarball ------------------------------------------------------

bool install_from_url(const std::string& name, const std::string& url,
                      const std::string& version, bool force,
                      std::string& err) {
  if (packages_root().empty()) {
    err = "no home directory: set OVYTH_HOME";
    return false;
  }
  if (!have_tool("tar")) {
    err = "tar is required to install tarball packages";
    return false;
  }
  if (!mkdir_p(packages_root())) {
    err = "cannot create " + packages_root();
    return false;
  }

  std::string body, herr;
  if (!http_get(url, body, herr)) {
    err = herr;
    return false;
  }

  std::string staged = staging_dir(name);
  remove_tree(staged);
  if (!mkdir_p(staged)) {
    err = "cannot create " + staged;
    return false;
  }

  std::string tarball = staged + "/.pkg.tar.gz";
  {
    std::string werr;
    write_file(tarball, body, werr);
  }
  std::string out;
  if (!run_command("tar -xzf " + sh_quote(tarball) + " -C " + sh_quote(staged) +
                       " 2>&1",
                   out, err)) {
    remove_tree(staged);
    return false;
  }
  ::unlink(tarball.c_str());

  // Tarballs usually wrap everything in one top-level directory; a flat
  // archive is just as valid. Normalise to the directory holding the entry.
  std::string pkg = staged;
  std::vector<std::string> children = dir_children(staged);
  if (children.size() == 1) pkg = staged + "/" + children[0];

  if (package_entry(pkg, name).empty()) {
    err = "the tarball at " + url + " has no " + name + ".ov or main.ov " +
          "to import as \"" + name + "\"";
    remove_tree(staged);
    return false;
  }
  // Through a copy rather than a rename, so a stray VCS directory in the
  // archive is dropped the same way a git clone's is.
  if (!copy_tree(pkg, staged + "/final", err)) {
    remove_tree(staged);
    return false;
  }
  if (!write_meta(staged + "/final", name, version.empty() ? "0.0.0" : version,
                  url)) {
    err = "cannot record the install in " + staged + "/final/" + kPkgMeta;
    remove_tree(staged);
    return false;
  }
  bool ok = take_destination(staged + "/final", install_dest(name), force, err);
  remove_tree(staged);
  if (ok) write_meta(install_dest(name), name, version.empty() ? "0.0.0" : version, url);
  return ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

namespace {

void pkg_usage() {
  std::printf(
      "ovc pkg -- the Ovyth package manager\n"
      "\n"
      "usage:\n"
      "  ovc pkg install <name>[@version]  install a package globally\n"
      "  ovc pkg install /path/to/pkg      install from a local directory\n"
      "  ovc pkg list                      list installed packages\n"
      "  ovc pkg remove <name>             remove installed packages\n"
      "  ovc pkg search <term>             search the registry\n"
      "  ovc pkg info <name>               registry + installed detail\n"
      "  ovc pkg update [<name>]           refresh the index / upgrade\n"
      "\n"
      "options:\n"
      "  --registry=<url>  registry index (or OVYTH_REGISTRY)\n"
      "  --refresh         ignore the cached index\n"
      "  --force           reinstall over an existing package\n"
      "\n"
      "packages install into $OVYTH_HOME/packages (default ~/.ovyth) and\n"
      "import from any directory afterwards:\n"
      "\n"
      "  ovc pkg install greet\n"
      "  import \"greet\"    # in any .ov, anywhere on this machine\n");
}

int install_usage(bool ok) {
  std::printf(
      "usage:\n"
      "  ovc pkg install <name>[@version]   from the registry\n"
      "  ovc pkg install <a> <b> <c>        several at once\n"
      "  ovc pkg install /path/to/pkg       from a local directory\n"
      "\n"
      "options: --force   --registry=<url>   --refresh\n");
  return ok ? 0 : 2;
}

// The panel after a successful install: what the user actually wanted to know.
// Piped, it is the tip lines this command has always printed.
void print_try_it(const std::vector<std::string>& names) {
  if (names.empty()) return;
  if (!ov::ui_tty()) {
    for (const std::string& n : names)
      std::printf("\ntip: import \"%s\" now works from any directory\n", n.c_str());
    return;
  }
  std::printf("\n");
  ov::ui_panel_begin("try it");
  for (const std::string& n : names)
    ov::ui_panel_line("import \"" + n + "\"", "in any .ov file");
  ov::ui_panel_end();
}

// Install one spec. `index` is the registry, already fetched for the whole
// run; it is ignored for a local-directory spec. On success `name` and `ver`
// name what went in, so the caller can summarise.
bool install_one(const std::string& spec, bool force,
                 const std::vector<IndexEntry>& index, std::string& label,
                 std::string& name, std::string& ver, std::string& err) {
  if (looks_like_path(spec)) {
    // A local directory: the directory's own name is the package name.
    std::string src = expand_user(spec);
    std::string base = base_name(src);
    while (!base.empty() && base.back() == '/') base.pop_back();
    name = base;
    ov::ui_step("copying from " + shorten_path(src));
    bool ok = install_from_path(base, src, "0.0.0", force, err);
    if (!ok) return false;
    ov::ui_step_done();
    ov::ui_step("installing into " + shorten_path(install_dest(base)));
    ov::ui_step_done();
    ver = "0.0.0";
    label = name + " from " + shorten_path(src);
    return true;
  }

  std::string version;
  parse_spec(spec, name, version);

  const IndexEntry* entry = index_find(index, name);
  if (!entry) {
    err = "no package named '" + name + "' in the registry";
    std::vector<std::string> near = index_similar(index, name);
    if (!near.empty()) {
      err += " (did you mean: ";
      for (size_t i = 0; i < near.size() && i < 5; i++)
        err += (i ? ", " : "") + near[i];
      err += "?)";
    }
    return false;
  }

  // Pick the version: the pinned one, or the newest published.
  SourceRef src;
  if (version.empty()) {
    if (entry->versions.empty()) {
      err = "package '" + name + "' has no installable source";
      return false;
    }
    ver = entry->versions.back().first;
    src = entry->versions.back().second;
  } else {
    for (const auto& [v, s] : entry->versions) {
      if (v == version) {
        src = s;
        break;
      }
    }
    if (!src.any()) {
      err = "package '" + name + "' has no version " + version + " (have: ";
      for (size_t i = 0; i < entry->versions.size(); i++)
        err += (i ? ", " : "") + entry->versions[i].first;
      err += ")";
      return false;
    }
  }
  if (ver.empty()) ver = "0.0.0";
  label = name + " " + ver;

  ov::ui_step("resolving " + name);
  ov::ui_step_done(ver);

  bool ok = false;
  if (!src.path.empty()) {
    ov::ui_step("copying from " + shorten_path(expand_user(src.path)));
    ok = install_from_path(name, expand_user(src.path), ver, force, err);
    if (ok) ov::ui_step_done();
  } else if (!src.repo.empty()) {
    ov::ui_step("cloning " + shorten_path(src.repo));
    ok = install_from_repo(name, src.repo, ver, force, err);
    if (ok) ov::ui_step_done();
  } else if (!src.url.empty()) {
    ov::ui_step("downloading " + shorten_path(src.url));
    ok = install_from_url(name, src.url, ver, force, err);
    if (ok) ov::ui_step_done();
  } else {
    err = "package '" + name + "' " + ver + " lists no source";
  }
  if (!ok) return false;

  ov::ui_step("installing into " + shorten_path(install_dest(name)));
  ov::ui_step_done();
  return true;
}

// A ceiling on one command, so a pasted list of a hundred names cannot turn
// into a hundred clones without the user noticing what they asked for.
constexpr size_t kMaxInstallSpecs = 32;

int cmd_install(const std::vector<std::string>& args, bool refresh) {
  bool force = false;
  std::vector<std::string> specs;
  for (const std::string& a : args) {
    if (a == "--force" || a == "-f") {
      force = true;
    } else if (a == "-h" || a == "--help") {
      return install_usage(true);
    } else if (a.rfind("-", 0) == 0 && a.size() > 1) {
      ov::cli_error("unknown option '" + a + "' (see: ovc pkg install --help)");
      return 2;
    } else if (std::find(specs.begin(), specs.end(), a) == specs.end()) {
      // Repeats are dropped rather than installed twice: a list pasted with a
      // duplicate should not fail on the second copy.
      specs.push_back(a);
    }
  }
  if (specs.empty()) return install_usage(false);
  if (specs.size() > kMaxInstallSpecs) {
    ov::cli_error("that is " + std::to_string(specs.size()) +
                  " packages; install at most " +
                  std::to_string(kMaxInstallSpecs) + " at a time");
    return 2;
  }

  // Validate every name before installing anything, so a bad name in the
  // middle of a list cannot leave half a command applied.
  for (const std::string& spec : specs) {
    if (looks_like_path(spec)) {
      std::string base = base_name(expand_user(spec));
      while (!base.empty() && base.back() == '/') base.pop_back();
      if (base.empty() || base == "." || base == "..") {
        ov::cli_error("cannot name a package after '" + spec + "'");
        return 1;
      }
      continue;
    }
    std::string name, version;
    parse_spec(spec, name, version);
    if (!valid_package_name(name)) {
      ov::cli_error("'" + name + "' is not a valid package name");
      return 1;
    }
  }

  // One index fetch for the whole run, whether it is one package or ten.
  std::vector<IndexEntry> index;
  bool from_cache = false, needs_index = false;
  for (const std::string& spec : specs)
    if (!looks_like_path(spec)) needs_index = true;
  if (needs_index) {
    std::string err;
    ov::ui_step("reading the registry index");
    if (!load_index(refresh, index, err, &from_cache)) {
      ov::ui_step_fail();
      ov::cli_error(err);
      return 1;
    }
    ov::ui_step_done(from_cache ? "ok (cached)" : "ok");
  }

  std::vector<std::string> labels;      // "greet 1.0.0", for one line each
  std::vector<std::string> names;       // "greet", for the import lines
  int failed = 0;
  for (const std::string& spec : specs) {
    std::string label, name, ver, err;
    if (install_one(spec, force, index, label, name, ver, err)) {
      labels.push_back(label);
      names.push_back(name);
      // Piped: one line per package, exactly as a single-package install
      // printed it (icon included, via ui_success's piped form). A terminal
      // gets the summary below instead.
      if (!ov::ui_tty()) std::printf("%s installed %s\n", ov::kOk, label.c_str());
      continue;
    }
    failed++;
    ov::ui_step_fail();
    ov::cli_error(err);
  }

  if (labels.empty()) return 1;

  if (ov::ui_tty()) {
    if (labels.size() == 1) {
      ov::ui_success("installed " + labels[0] +
                     (from_cache ? "  (index cached)" : ""));
    } else {
      std::string msg = "installed " + std::to_string(labels.size()) + " packages" +
                        (from_cache ? "  (index cached)" : "");
      if (failed) msg += ", " + std::to_string(failed) + " failed";
      ov::ui_success(msg);
    }
  }
  // A panel on a terminal, the tip lines this command has always printed when
  // piped -- for every package, however many were asked for.
  print_try_it(names);
  if (ov::ui_tty() && failed)
    std::printf("\n   %s %d of %d packages failed; see above\n",
                ov::ui_red(ov::kFail).c_str(), failed,
                (int)(labels.size() + failed));
  return failed ? 1 : 0;
}

int cmd_list() {
  const std::string& root = packages_root();
  std::vector<InstalledMeta> metas;
  if (!root.empty()) {
    for (const std::string& child : dir_children(root)) {
      // Staging directories and other dot-entries are package-manager
      // scratch, not packages.
      if (child.empty() || child[0] == '.') continue;
      if (!fs_is_dir(root + "/" + child)) continue;
      InstalledMeta m = read_meta(root + "/" + child);
      if (!m.valid) {
        m.name = child;
        m.version = "?";
        m.source = "unknown";
      }
      metas.push_back(m);
    }
  }
  if (metas.empty()) {
    ov::ui_info_line("no packages installed yet");
    std::printf("\ninstall one with:  ovc pkg install <name>\n");
    return 0;
  }

  // Same two column widths the raw printf used, so piped output is unchanged.
  std::vector<ov::UiColumn> cols = {{"NAME", 24}, {"VERSION", 12}, {"SOURCE", -1}};
  std::vector<std::vector<std::string>> rows;
  std::sort(metas.begin(), metas.end(),
            [](const InstalledMeta& a, const InstalledMeta& b) {
              return a.name < b.name;
            });
  for (const auto& m : metas) rows.push_back({m.name, m.version, m.source});
  ov::ui_table(cols, rows);

  if (ov::ui_tty())
    std::printf("\n   %d package%s · %s\n", (int)metas.size(),
                metas.size() == 1 ? "" : "s", packages_root().c_str());
  else
    std::printf("\n%d package(s) in %s\n", (int)metas.size(),
                packages_root().c_str());
  return 0;
}

int cmd_remove(const std::vector<std::string>& args) {
  std::vector<std::string> names;
  for (const std::string& a : args) {
    if (a == "-h" || a == "--help") {
      std::printf("usage: ovc pkg remove <name> [<name> ...]\n");
      return 0;
    }
    if (a.rfind("-", 0) == 0 && a.size() > 1) {
      ov::cli_error("unknown option '" + a + "'");
      return 2;
    }
    names.push_back(a);
  }
  if (names.empty()) {
    ov::cli_error("ovc pkg remove needs at least one package name");
    return 2;
  }
  if (packages_root().empty()) {
    ov::cli_error("no home directory: set OVYTH_HOME");
    return 1;
  }

  int rc = 0;
  for (const std::string& spec : names) {
    std::string name, version;
    parse_spec(spec, name, version);
    if (!valid_package_name(name)) {
      ov::cli_error("'" + name + "' is not a valid package name");
      rc = 1;
      continue;
    }
    std::string dir = install_dest(name);
    if (!fs_is_dir(dir)) {
      ov::cli_error("'" + name + "' is not installed");
      rc = 1;
      continue;
    }
    ov::ui_step("removing " + shorten_path(dir));
    if (!remove_tree(dir)) {
      ov::ui_step_fail();
      ov::cli_error("cannot remove " + dir);
      rc = 1;
      continue;
    }
    ov::ui_step_done();
    ov::ui_success("removed " + name);
  }
  return rc;
}

std::string lowercase(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return (char)std::tolower(c);
  });
  return out;
}

int cmd_search(const std::vector<std::string>& args, bool refresh) {
  std::string term;
  for (const std::string& a : args) {
    if (a == "-h" || a == "--help") {
      std::printf("usage: ovc pkg search <term>\n");
      return 0;
    }
    if (!term.empty()) term += " ";
    term += a;
  }
  if (term.empty()) {
    ov::cli_error("ovc pkg search needs a search term");
    return 2;
  }

  std::vector<IndexEntry> index;
  std::string err;
  if (!load_index(refresh, index, err)) {
    ov::cli_error(err);
    return 1;
  }

  std::string needle = lowercase(term);
  std::vector<ov::UiColumn> cols = {{"NAME", 24}, {"VERSION", 10}, {"DESCRIPTION", -1}};
  std::vector<std::vector<std::string>> rows;
  int hits = 0;
  for (const auto& e : index) {
    std::string hay = lowercase(e.name + " " + e.description);
    if (needle.empty() || hay.find(needle) != std::string::npos) {
      std::string ver = e.versions.empty() ? "-" : e.versions.back().first;
      rows.push_back({e.name, ver,
                      e.description.empty() ? "(no description)" : e.description});
      hits++;
    }
  }
  if (!hits) {
    if (ov::ui_tty())
      std::printf("\n   %s nothing matches '%s'\n", ov::ui_dim(ov::kNote).c_str(),
                  term.c_str());
    else
      std::printf("nothing matches '%s'\n", term.c_str());
    return 0;
  }
  ov::ui_table(cols, rows);
  if (ov::ui_tty())
    std::printf("\n   %d match%s\n", hits, hits == 1 ? "" : "es");
  return 0;
}

int cmd_info(const std::vector<std::string>& args, bool refresh) {
  if (args.empty() || args[0] == "-h" || args[0] == "--help") {
    std::printf("usage: ovc pkg info <name>\n");
    return args.empty() ? 2 : 0;
  }
  const std::string& name = args[0];

  std::string dest = install_dest(name);
  bool installed = fs_is_dir(dest);

  std::vector<IndexEntry> index;
  std::string err;
  bool have_index = load_index(refresh, index, err);

  if (ov::ui_tty()) ov::ui_panel_begin(name);
  ov::ui_field("package", name);

  if (have_index) {
    const IndexEntry* e = index_find(index, name);
    if (e) {
      std::string ver = e->versions.empty() ? "-" : e->versions.back().first;
      ov::ui_field("version", ver + " (registry)");
      if (!e->description.empty())
        ov::ui_field("description", e->description);
      if (!e->license.empty()) ov::ui_field("license", e->license);
      if (!e->homepage.empty()) ov::ui_field("homepage", e->homepage);
      const SourceRef& s = e->versions.back().second;
      if (!s.path.empty()) ov::ui_field("source", s.path);
      else if (!s.repo.empty()) ov::ui_field("source", s.repo + "  (git)");
      else if (!s.url.empty()) ov::ui_field("source", s.url);
      if (e->versions.size() > 1) {
        std::string all;
        for (size_t i = 0; i < e->versions.size(); i++)
          all += (i ? " " : "") + e->versions[i].first;
        ov::ui_field("versions", all);
      }
    } else {
      ov::ui_field("registry", "not listed");
    }
  } else {
    ov::ui_field("registry", "unavailable (" + err + ")");
  }

  if (installed) {
    InstalledMeta m = read_meta(dest);
    ov::ui_field("installed", "yes");
    ov::ui_field("location", dest);
    if (m.valid) {
      if (!m.version.empty()) ov::ui_field("version", m.version + " (installed)");
      if (!m.installed_at.empty())
        ov::ui_field("installed", m.installed_at);
      if (!m.source.empty()) ov::ui_field("from", m.source);
    }
    std::string entry = package_entry(dest, name);
    if (!entry.empty()) ov::ui_field("import", "import \"" + name + "\"");
  } else {
    ov::ui_field("installed", "no");
  }
  if (ov::ui_tty()) ov::ui_panel_end();
  if (!have_index && !installed) return 1;
  return 0;
}

int cmd_update(const std::vector<std::string>& args, bool refresh) {
  std::string name;
  for (const std::string& a : args) {
    if (a == "--force" || a == "-f" || a == "--refresh" || a == "-r") continue;
    if (a == "-h" || a == "--help") {
      std::printf("usage: ovc pkg update [<name>]\n");
      return 0;
    }
    if (a.rfind("-", 0) == 0 && a.size() > 1) {
      ov::cli_error("unknown option '" + a + "'");
      return 2;
    }
    if (name.empty()) name = a;
  }

  std::vector<IndexEntry> index;
  std::string err;
  bool from_cache = false;
  ov::ui_step("refreshing the registry index");
  if (!load_index(true, index, err, &from_cache)) {
    ov::ui_step_fail();
    ov::cli_error(err);
    return 1;
  }
  ov::ui_step_done(from_cache ? "ok (cached)" : "ok");
  ov::ui_success("registry index refreshed" +
                 std::string(from_cache ? "  (from cache)" : ""));

  if (name.empty()) {
    return cmd_list();
  }

  // Upgrade: reinstall the newest version over whatever is there.
  const IndexEntry* e = index_find(index, name);
  if (!e || e->versions.empty()) {
    ov::cli_error("'" + name + "' is not in the registry");
    return 1;
  }
  std::string had = "0.0.0";
  if (fs_is_dir(install_dest(name))) {
    InstalledMeta m = read_meta(install_dest(name));
    if (m.valid && !m.version.empty()) had = m.version;
  }
  const std::string& ver = e->versions.back().first;
  const SourceRef& src = e->versions.back().second;

  ov::ui_step("resolving " + name);
  ov::ui_step_done(ver);
  ov::ui_step("installing into " + shorten_path(install_dest(name)));

  bool ok = false;
  if (!src.path.empty())
    ok = install_from_path(name, expand_user(src.path), ver, true, err);
  else if (!src.repo.empty())
    ok = install_from_repo(name, src.repo, ver, true, err);
  else if (!src.url.empty())
    ok = install_from_url(name, src.url, ver, true, err);
  if (!ok) {
    ov::ui_step_fail();
    ov::cli_error("upgrade failed: " + err);
    return 1;
  }
  ov::ui_step_done();
  if (had == ver)
    ov::ui_success(name + " " + ver + " is already the newest version");
  else
    ov::ui_success("upgraded " + name + " " + had + " → " + ver);
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// entry point
// ---------------------------------------------------------------------------

int run_pkg_command(const std::vector<std::string>& args) {
  // Global flags are lifted out first so every subcommand accepts them in any
  // position; what is left is the subcommand and its own arguments.
  //
  // `--help` is the exception: with no subcommand it prints the overview, and
  // with one it is passed through, so `ovc pkg install --help` describes the
  // install command rather than the whole manager.
  std::vector<std::string> rest;
  bool refresh = false, want_help = false, have_sub = false;
  for (const std::string& a : args) {
    if (a == "-h" || a == "--help" || a == "help") {
      want_help = true;
    } else if (a == "--refresh" || a == "-r") {
      refresh = true;
    } else if (a.rfind("--registry=", 0) == 0) {
      registry_override() = a.substr(std::strlen("--registry="));
    } else if (a.rfind("-", 0) == 0 && a.size() > 1 && a != "-f" && a != "--force") {
      // -f/--force belong to the subcommands; anything else is unknown.
      ov::cli_error("unknown option '" + a + "' (see: ovc pkg --help)");
      return 2;
    } else {
      if (!have_sub) have_sub = true;
      rest.push_back(a);
    }
  }

  if (want_help && !have_sub) {
    pkg_usage();
    return 0;
  }
  if (rest.empty()) {
    pkg_usage();
    return 2;
  }

  const std::string sub = rest[0];
  if (want_help) rest.push_back("--help");  // the subcommand answers for it
  std::vector<std::string> sub_args(rest.begin() + 1, rest.end());

  if (sub == "install") return cmd_install(sub_args, refresh);
  if (sub == "list" || sub == "ls") return cmd_list();
  if (sub == "remove" || sub == "rm" || sub == "uninstall")
    return cmd_remove(sub_args);
  if (sub == "search") return cmd_search(sub_args, refresh);
  if (sub == "info") return cmd_info(sub_args, refresh);
  if (sub == "update" || sub == "upgrade") return cmd_update(sub_args, refresh);

  ov::cli_error("unknown pkg subcommand '" + sub + "' (see: ovc pkg --help)");
  return 2;
}

}  // namespace ov


