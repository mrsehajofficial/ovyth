// Ovyth :: main.cpp
//
//   ovc init <dir>           create a new project from a template
//   ovc <file.ov>            compile to a native executable (default)
//   ovc run <file.ov>        run with the tree-walking backend
//   ovc build <file.ov>      alias for the default
//   ovc check <file.ov>      parse + type check only
//   ovc fmt <file.ov>        reformat (canonical layout)
//   ovc ast <file.ov>        dump the AST
//   ovc tokens <file.ov>     dump the token stream
//   ovc version
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "ast/ast.h"
#include "backend/interp.h"
#include "backend/codegen_c.h"
#include "driver.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/sema.h"
#include "tools/fmt.h"
#include "tools/scaffold.h"

namespace {

constexpr const char* kVersion = "0.1.3";

void usage() {
  std::printf(
      "ovc %s -- the Ovyth compiler\n"
      "\n"
      "usage:\n"
      "  ovc init <dir>             create a new project from a template\n"
      "  ovc <file.ov>              compile to a native executable\n"
      "  ovc run <file.ov>          run with the tree-walking backend\n"
      "  ovc check <file.ov>        parse + type check only\n"
      "  ovc ast <file.ov>          dump the AST\n"
      "  ovc tokens <file.ov>       dump the token stream\n"
      "  ovc fmt <file.ov>          canonical formatting (stdout)\n"
      "  ovc version\n"
      "\n"
      "options:\n"
      "  -O<n>        optimisation level (0-3, default 2)\n"
      "  --release    optimised build: -O3, LTO, strip (spec section 20)\n"
      "  --target=native  optimise for this machine's CPU (section 21)\n"
      "  --no-strip   keep symbols even in --release\n"
      "  -o <path>    output path for the executable\n"
      "  --keep-ir    keep the generated .ll file\n"
      "  --stats      print heap / allocation stats at exit\n"
      "  --check-only skip code generation (with a compile command)\n",
      kVersion);
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// ---------------------------------------------------------------------------
// Module loading.
//
// `import "path"` is resolved at load time: the imported file is parsed
// (recursively loading its own imports first) and its top-level
// statements are concatenated into the program in dependency order.
// Imports are side-effect-free -- a file is definitions only -- so a
// file imported more than once is merged once, and import cycles are an
// error. Top-level names (functions, globals) must be unique across all
// modules; the owner map catches collisions, including against the
// entry file itself. After loading, no Import statements remain and
// every later phase sees one flat program.
// ---------------------------------------------------------------------------
struct ModuleLoader {
  std::vector<std::string> stack;           // in-progress files (cycle detection)
  std::set<std::string> loaded;             // already-merged files
  std::map<std::string, std::string> owner; // top-level name -> defining file
  std::vector<ov::Diagnostic> diags;

  static void defined_names(const ov::ast::Stmt* s, std::vector<std::string>& out) {
    if (!s) return;
    if (s->kind == ov::ast::StmtKind::FuncDecl) {
      out.push_back(s->name);
    } else if (s->kind == ov::ast::StmtKind::VarDecl) {
      for (const auto& n : s->names) out.push_back(n);
    }
  }

  bool on_stack(const std::string& f) const {
    for (const auto& s : stack) if (s == f) return true;
    return false;
  }

  // Resolve an import path against the importing file's directory.
  // Absolute paths are used as-is; relative paths are anchored to the
  // importing file. If the file does not exist and the path has no
  // ".ov" extension, ".ov" is appended and retried.
  std::string resolve(const std::string& from_file, const std::string& path) const {
    std::string p = path;
    if (p.empty() || p[0] != '/') {
      size_t slash = from_file.find_last_of('/');
      p = (slash == std::string::npos ? std::string()
                                      : from_file.substr(0, slash + 1)) + p;
    }
    if (::access(p.c_str(), R_OK) != 0) {
      std::string with_ext = p;
      if (with_ext.size() < 4 || with_ext.substr(with_ext.size() - 4) != ".ov")
        with_ext += ".ov";
      if (::access(with_ext.c_str(), R_OK) == 0) p = with_ext;
    }
    return p;
  }

  // Parse `file` and merge its statements into `out`, after merging the
  // files it imports. `via_file`/`via_pos` locate the import statement
  // that pulled this file in (the entry file passes its own name and a
  // zero position) so load errors point at the right line. Returns false
  // (and records diagnostics) on any load, cycle, or collision error.
  bool load(const std::string& file, ov::ast::Program& out,
            const std::string& via_file = "", int via_line = 0, int via_col = 0) {
    if (loaded.count(file)) return true;  // merged already

    std::string source;
    if (!read_file(file, source)) {
      diags.push_back(ov::Diagnostic{
          via_file, via_line, via_col,
          "cannot open imported file '" + file + "'",
          ov::DiagKind::Error});
      return false;
    }

    ov::Lexer lexer(source, file);
    ov::Parser parser(lexer.scan(), file);
    ov::ast::Program prog = parser.parse();
    for (const auto& d : lexer.diagnostics()) diags.push_back(d);
    for (const auto& d : prog.diags) diags.push_back(d);

    stack.push_back(file);
    bool ok = true;
    for (ov::ast::Stmt* s : prog.statements) {
      if (s && s->kind == ov::ast::StmtKind::Import) {
        std::string target = resolve(file, s->import_path);
        if (on_stack(target)) {
          diags.push_back(ov::Diagnostic{
              file, s->pos.line, s->pos.col,
              "import cycle: '" + target + "' imports itself",
              ov::DiagKind::Error});
          ok = false;
        } else if (!load(target, out, file, s->pos.line, s->pos.col)) {
          ok = false;
        }
        continue;  // the import itself adds no statements
      }

      // Top-level names must be unique across modules.
      std::vector<std::string> names;
      defined_names(s, names);
      for (const auto& n : names) {
        auto it = owner.find(n);
        if (it != owner.end() && it->second != file) {
          diags.push_back(ov::Diagnostic{
              file, s->pos.line, s->pos.col,
              "'" + n + "' is already defined in " + it->second,
              ov::DiagKind::Error});
          ok = false;
        } else {
          owner[n] = file;
        }
      }
      out.statements.push_back(s);
    }
    stack.pop_back();
    loaded.insert(file);
    return ok;
  }
};

void print_diags(const std::vector<ov::Diagnostic>& diags) {
  int errs = 0, warns = 0, infos = 0, notes = 0;
  for (const auto& d : diags) {
    std::fprintf(stderr, "%s\n", d.render().c_str());
    switch (d.kind) {
      case ov::DiagKind::Error:   errs++;  break;
      case ov::DiagKind::Warning: warns++; break;
      case ov::DiagKind::Info:    infos++; break;
      case ov::DiagKind::Note:    notes++; break;
    }
  }
  std::string sum;
  if (errs) {
    sum += ov::diag_paint(ov::kColorRed,
           "✖ " + std::to_string(errs) + (errs == 1 ? " error" : " errors"));
  }
  if (warns) {
    if (!sum.empty()) sum += ", ";
    sum += ov::diag_paint(ov::kColorYellow,
           "▲ " + std::to_string(warns) + (warns == 1 ? " warning" : " warnings"));
  }
  if (infos) {
    if (!sum.empty()) sum += ", ";
    sum += ov::diag_paint(ov::kColorCyan,
           "ℹ " + std::to_string(infos) + (infos == 1 ? " info" : " infos"));
  }
  if (notes) {
    if (!sum.empty()) sum += ", ";
    sum += ov::diag_paint(ov::kColorDim,
           "• " + std::to_string(notes) + (notes == 1 ? " note" : " notes"));
  }
  if (!sum.empty()) std::fprintf(stderr, "%s\n", sum.c_str());
  if (errs > 0) {
    std::fprintf(stderr, "%s\n",
                 ov::diag_paint(ov::kColorDim,
                     "tip: fix the issues above, then run ovc again").c_str());
  }
}

void init_usage() {
  std::printf(
      "usage:\n"
      "  ovc init <dir> [--template=<name>]\n"
      "\n"
      "creates <dir> with a starter main.ov, a Makefile, a README and a\n"
      ".gitignore. An existing directory is accepted only while it is empty.\n"
      "\n"
      "templates:\n"
      "  hello    print a greeting                      (default)\n"
      "  http     call an HTTP API and parse JSON\n"
      "  cli      a small interactive command loop\n"
      "\n"
      "examples:\n"
      "  ovc init myapp\n"
      "  ovc init scraper --template=http\n");
}

int run_init(const std::vector<std::string>& args, size_t i) {
  std::string dir, tmpl = "hello";
  for (; i < args.size(); i++) {
    const std::string& a = args[i];
    if (a.rfind("--template=", 0) == 0) {
      tmpl = a.substr(11);
    } else if ((a == "--template" || a == "-t") && i + 1 < args.size()) {
      tmpl = args[++i];
    } else if (a == "-h" || a == "--help") {
      init_usage();
      return 0;
    } else if (a.rfind("-", 0) == 0 && a.size() > 1) {
      ov::cli_error("unknown option '" + a + "' (see: ovc --help)");
      return 2;
    } else if (dir.empty()) {
      dir = a;
    } else {
      ov::cli_error("init takes a single directory");
      return 2;
    }
  }

  if (dir.empty()) {
    init_usage();
    return 2;
  }

  std::string err;
  if (!ov::scaffold_project(dir, tmpl, err)) {
    ov::cli_error(err);
    return 1;
  }

  std::string path = dir;
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  std::printf("\n");
  ov::cli_ok("created " + path + "/ with the '" + tmpl + "' template");
  std::printf("\nnext:\n");
  std::printf("  cd %s\n", dir.c_str());
  std::printf("  ovc run main.ov      run it with the interpreter\n");
  std::printf("  make build           compile to %s/build/app\n", path.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);

  if (args.empty()) {
    usage();
    return 0;
  }
  if (args[0] == "version" || args[0] == "--version" || args[0] == "-v") {
    std::printf("ovc %s (Ovyth)\n", kVersion);
    return 0;
  }
  if (args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
    usage();
    return 0;
  }

  // ---- split command from options ---------------------------------------
  std::string cmd = "build";
  size_t i = 0;
  static const char* kCommands[] = {"init", "run", "build", "check",
                                    "ast",  "tokens", "fmt"};
  for (const char* c : kCommands) {
    if (args[0] == c) {
      cmd = c;
      i = 1;
      break;
    }
  }

  // `init` has its own tiny option set (--template) that would trip the
  // compiler option parser below, so it is handled before it.
  if (cmd == "init") return run_init(args, i);

  ov::Options opt;
  std::string input;
  for (; i < args.size(); i++) {
    const std::string& a = args[i];
    if (a == "-o" && i + 1 < args.size()) {
      opt.output = args[++i];
    } else if (a.rfind("-O", 0) == 0) {
      opt.opt_level = std::atoi(a.c_str() + 2);
    } else if (a == "--keep-ir") {
      opt.keep_ir = true;
    } else if (a == "--stats") {
      opt.stats = true;
    } else if (a == "--check-only") {
      opt.check_only = true;
    } else if (a == "--release") {
      opt.release = true;
      if (opt.opt_level < 3) opt.opt_level = 3;
    } else if (a.rfind("--target=", 0) == 0) {
      std::string t = a.substr(9);
      if (t == "native") {
        opt.target_native = true;
        } else {
          ov::cli_error("unknown target '" + t + "' (try: native)");
          return 2;
        }
    } else if (a == "--no-strip") {
      opt.no_strip = true;
    } else if (a.rfind("-", 0) == 0 && a.size() > 1) {
      ov::cli_error("unknown option '" + a + "' (see: ovc --help)");
      return 2;
    } else if (input.empty()) {
      input = a;
    } else {
      opt.extra_args.push_back(a);
    }
  }

  if (input.empty()) {
    usage();
    return 2;
  }

  std::string source;
  if (!read_file(input, source)) {
    ov::cli_error("cannot open '" + input + "'");
    return 2;
  }

  // ---- front end --------------------------------------------------------
  ov::Lexer lexer(source, input);
  std::vector<ov::Token> tokens = lexer.scan();
  if (!lexer.diagnostics().empty()) {
    print_diags(lexer.diagnostics());
    return 1;
  }

  ov::Parser parser(std::move(tokens), input);
  ov::ast::Program program = parser.parse();
  if (!program.diags.empty()) {
    print_diags(program.diags);
    return 1;
  }

  if (cmd == "tokens") {
    // re-scan for display (the parser consumed the vector)
    ov::Lexer lx(source, input);
    for (const auto& t : lx.scan())
      std::printf("%-14s %-4d:%-3d  %s\n", ov::tok_name(t.kind), t.line, t.col,
                  std::string(t.text).c_str());
    return 0;
  }

  if (cmd == "ast") {
    std::string out;
    ov::ast::dump_ast(program, out);
    std::printf("%s", out.c_str());
    return 0;
  }

  if (cmd == "fmt") {
    std::printf("%s", ov::format_program(program, source).c_str());
    return 0;
  }

  // ---- modules ------------------------------------------------------------
  // Flatten imports into one program before any later phase runs.
  // `fmt`, `ast`, and `tokens` above already returned on the
  // single-file view, so they still see the import statements.
  {
    ModuleLoader loader;
    ov::ast::Program merged;
    loader.load(input, merged);
    if (!loader.diags.empty()) {
      print_diags(loader.diags);
      return 1;
    }
    program.statements.swap(merged.statements);
  }

  ov::Sema sema;
  bool ok = sema.run(program);
  if (!program.diags.empty()) {
    print_diags(program.diags);
    if (!ok) return 1;
  }

  if (opt.stats) {
    for (const auto& kv : sema.globals) {
      char buf[512];
      std::snprintf(buf, sizeof(buf), "  %-20s %s",
                    kv.first.c_str(), ov::ty_name(kv.second));
      std::fprintf(stderr, "%s\n",
                   ov::diag_paint(ov::kColorDim, buf).c_str());
    }
  }

  if (cmd == "check") {
    if (ok) ov::cli_ok(input + ": ok");
    return ok ? 0 : 1;
  }

  // ---- backend ----------------------------------------------------------
  if (cmd == "run") {
    ov::Interp interp(program, input);
    return interp.run();
  }

  // native compilation
  ov::Driver driver(input, opt);
  return driver.compile(program, sema, source);
}
