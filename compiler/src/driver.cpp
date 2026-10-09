#include "driver.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "backend/codegen_c.h"
#include "lexer/diagnostic.h"

extern "C" {
#include "ovrt.h"
}

namespace ov {

Driver::~Driver() {
  if (!opt_.keep_ir) {
    for (const auto& t : temps_) ::unlink(t.c_str());
  }
}

std::string runtime_archive_path() {
  // The runtime archive ships next to the ovc binary so a compiled program is
  // self-contained: no interpreter, no runtime install, no rpath.
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    std::string exe(buf);
    size_t slash = exe.find_last_of('/');
    if (slash != std::string::npos) {
      std::string dir = exe.substr(0, slash);
      struct stat st;
      // Ordered by preference: a co-located archive (a build tree), then the
      // conventional <prefix>/lib of an installed tree, then two layouts that
      // older checkouts used.
      for (const char* rel : {"/libovrt.a", "/../lib/libovrt.a", "/../libovrt.a",
                              "/../build/libovrt.a"}) {
        std::string candidate = dir + rel;
        if (::stat(candidate.c_str(), &st) == 0) {
          // normalise /./ and /../ so the shell command quotes cleanly
          std::string out;
          std::vector<std::string> parts;
          size_t i = 0;
          while (i < candidate.size()) {
            size_t j = candidate.find('/', i);
            if (j == std::string::npos) j = candidate.size();
            std::string seg = candidate.substr(i, j - i);
            if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (seg != "." && !seg.empty()) parts.push_back(seg);
            i = j + 1;
          }
          for (size_t k = 0; k < parts.size(); k++) {
            if (parts[k].find('.') == std::string::npos) out += "/" + parts[k];
            else out += "/" + parts[k];
          }
          return out;
        }
      }
    }
  }
  return "libovrt.a";
}


// The runtime headers live in <root>/runtime/include. Resolve them relative to
// the ovc binary the same way the archive is resolved, so a build tree and an
// installed tree both work without configuration.
std::string runtime_include_dir() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    std::string exe(buf);
    size_t slash = exe.find_last_of('/');
    if (slash != std::string::npos) {
      std::string dir = exe.substr(0, slash);
      struct stat st;
      for (const char* rel : {"/../runtime/include", "/../../runtime/include",
                              "/../include"}) {
        std::string c = dir + rel;
        if (::stat((c + "/ovrt.h").c_str(), &st) == 0) return c;
      }
    }
  }
  return "runtime/include";
}

// Quote a filesystem path for the POSIX shell. Every path that reaches
// Driver::run goes through this -- an unquoted path with spaces (or shell
// metacharacters) turns "clang -I/my dir/..." into two words and the build
// dies with a shell syntax error.
static std::string sh_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

bool Driver::run(const std::vector<std::string>& argv, std::string& err) {
  std::string joined;
  for (const auto& a : argv) joined += a + " ";

  FILE* pipe = ::popen(joined.c_str(), "r");
  if (!pipe) {
    err = "cannot start: " + joined;
    return false;
  }
  char line[4096];
  std::string output;
  while (fgets(line, sizeof(line), pipe)) output += line;
  int rc = ::pclose(pipe);
  if (rc != 0) {
    err = output.empty() ? ("command failed: " + joined) : output;
    return false;
  }
  return true;
}

bool Driver::emit_ir(const ast::Program& program, Sema& sema, std::string& ir_path,
                     std::string& err) {
  std::string ir = emit_c_source(program, sema, "", err);
  if (!err.empty()) return false;
  std::ofstream out(ir_path);
  if (!out) {
    err = "cannot write " + ir_path;
    return false;
  }
  out << ir;
  return true;
}

static std::string temp_path(const std::string& input, const char* suffix) {
  size_t slash = input.find_last_of('/');
  std::string base = slash == std::string::npos ? input : input.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  const char* tmp = ::getenv("TMPDIR");
  std::string dir = (tmp && *tmp) ? tmp : "/tmp";
  return dir + "/ov_" + base + "_" + std::to_string(::getpid()) + suffix;
}

int Driver::compile(const ast::Program& program, Sema& sema, const std::string& source) {
  (void)source;
  if (opt_.check_only) {
    ov::cli_ok(input_ + ": ok (no codegen)");
    return 0;
  }

  std::string err;
  std::string ir = emit_c_source(program, sema, source, err);
  if (!err.empty()) {
    ov::cli_error(err);
    return 1;
  }

  std::string ir_path = temp_path(input_, ".c");
  std::string obj = temp_path(input_, ".o");
  temps_.push_back(ir_path);
  temps_.push_back(obj);

  std::string out = opt_.output;
  if (out.empty()) {
    size_t slash = input_.find_last_of('/');
    std::string base = slash == std::string::npos ? input_ : input_.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    out = base;
  }

  // ---- optimisation flags (spec sections 4, 19, 20, 21, 29, 38) ----
  //
  // A release build layers: -O3, per-function sections so the linker's
  // --gc-sections can drop what the program never calls, LTO so the optimiser
  // sees across the generated code and libovrt together, and --target=native
  // to use this machine's ISA. A plain build stays -O2 without LTO so
  // iteration is fast and the binary stays debuggable.
  std::string rt = runtime_archive_path();
  std::vector<std::string> cflags, ldflags;
  cflags.push_back("-O" + std::to_string(opt_.opt_level));
  cflags.push_back("-fno-plt");
  cflags.push_back("-ffunction-sections");   // per-function sections
  cflags.push_back("-fdata-sections");
  cflags.push_back("-fno-semantic-interposition");

  if (opt_.release) {
    cflags.push_back("-O3");
    cflags.push_back("-flto");                // link-time optimisation
    cflags.push_back("-fomit-frame-pointer");
    if (!opt_.no_strip) ldflags.push_back("-s");
    ldflags.push_back("-flto");
  }
  if (opt_.target_native) {
    cflags.push_back("-march=native");
    cflags.push_back("-mtune=native");
  } else {
    cflags.push_back("-mtune=generic");
  }
  cflags.push_back("-Wall");
  cflags.push_back("-Wno-unused-function");
  cflags.push_back("-Wno-unused-variable");

  // The runtime is linked *normally* (not --whole-archive) so only the
  // objects the program actually references are pulled in. Combined with
  // --gc-sections, a program that never touches http.* or json.* does not
  // carry them -- or libcurl -- in its binary.
  ldflags.push_back("-Wl,--gc-sections");
  ldflags.push_back("-Wl,--as-needed");

  auto join_flags = [](const std::vector<std::string>& v, const char* prefix) {
    std::string s;
    for (const auto& f : v) {
      if (!s.empty()) s += " ";
      s += std::string(prefix) + f;
    }
    return s;
  };

  // Materialise the generated C before handing it to clang.
  {
    std::ofstream f(ir_path);
    if (!f) {
      ov::cli_error("cannot write " + ir_path);
      return 1;
    }
    f << ir;
    f.close();
  }

  std::ostringstream c1;
  c1 << "clang " << join_flags(cflags, "") << " -I" << sh_quote(runtime_include_dir())
     << " -c -o " << sh_quote(obj) << " " << sh_quote(ir_path);
  if (!run({c1.str()}, err)) {
    std::fprintf(stderr, "%s", err.c_str());
    return 1;
  }

  // libcurl/OpenSSL/zlib are referenced only through http.o, which --as-needed
  // now drops for programs that never make a request.
  std::ostringstream c2;
  c2 << "clang " << join_flags(cflags, "") << " -o " << sh_quote(out) << " "
     << sh_quote(obj) << " " << sh_quote(rt) << " " << join_flags(ldflags, "") << " "
     << "-lcurl -lssl -lcrypto -lz -lm -lpthread -ldl";
  if (!run({c2.str()}, err)) {
    std::fprintf(stderr, "%s", err.c_str());
    return 1;
  }

  ::chmod(out.c_str(), 0755);
  if (opt_.keep_ir)
    ov::cli_info("kept IR: " + ir_path);
  else
    ov::cli_ok("compiled " + input_ + " -> " + out);
  return 0;
}

}  // namespace ov