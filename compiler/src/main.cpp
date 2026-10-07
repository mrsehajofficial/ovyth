// Vayu :: main.cpp
//
//   vyc init <dir>           create a new project from a template
//   vyc <file.vy>            compile to a native executable (default)
//   vyc run <file.vy>        run with the tree-walking backend
//   vyc build <file.vy>      alias for the default
//   vyc check <file.vy>      parse + type check only
//   vyc fmt <file.vy>        reformat (canonical layout)
//   vyc ast <file.vy>        dump the AST
//   vyc tokens <file.vy>     dump the token stream
//   vyc version
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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
      "vyc %s -- the Vayu compiler\n"
      "\n"
      "usage:\n"
      "  vyc init <dir>             create a new project from a template\n"
      "  vyc <file.vy>              compile to a native executable\n"
      "  vyc run <file.vy>          run with the tree-walking backend\n"
      "  vyc check <file.vy>        parse + type check only\n"
      "  vyc ast <file.vy>          dump the AST\n"
      "  vyc tokens <file.vy>       dump the token stream\n"
      "  vyc fmt <file.vy>          canonical formatting (stdout)\n"
      "  vyc version\n"
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

void print_diags(const std::vector<vy::Diagnostic>& diags) {
  for (const auto& d : diags) std::fprintf(stderr, "%s\n", d.render().c_str());
}

void init_usage() {
  std::printf(
      "usage:\n"
      "  vyc init <dir> [--template=<name>]\n"
      "\n"
      "creates <dir> with a starter main.vy, a Makefile, a README and a\n"
      ".gitignore. An existing directory is accepted only while it is empty.\n"
      "\n"
      "templates:\n"
      "  hello    print a greeting                      (default)\n"
      "  http     call an HTTP API and parse JSON\n"
      "  cli      a small interactive command loop\n"
      "\n"
      "examples:\n"
      "  vyc init myapp\n"
      "  vyc init scraper --template=http\n");
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
      std::fprintf(stderr, "vyc: unknown option '%s'\n", a.c_str());
      return 2;
    } else if (dir.empty()) {
      dir = a;
    } else {
      std::fprintf(stderr, "vyc: init takes a single directory\n");
      return 2;
    }
  }

  if (dir.empty()) {
    init_usage();
    return 2;
  }

  std::string err;
  if (!vy::scaffold_project(dir, tmpl, err)) {
    std::fprintf(stderr, "vyc: %s\n", err.c_str());
    return 1;
  }

  std::string path = dir;
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  std::printf("\ncreated %s/ with the '%s' template\n\n", path.c_str(), tmpl.c_str());
  std::printf("next:\n");
  std::printf("  cd %s\n", dir.c_str());
  std::printf("  vyc run main.vy      run it with the interpreter\n");
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
    std::printf("vyc %s (Vayu)\n", kVersion);
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

  vy::Options opt;
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
        std::fprintf(stderr, "vyc: unknown target '%s' (try: native)\n", t.c_str());
        return 2;
      }
    } else if (a == "--no-strip") {
      opt.no_strip = true;
    } else if (a.rfind("-", 0) == 0 && a.size() > 1) {
      std::fprintf(stderr, "vyc: unknown option '%s'\n", a.c_str());
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
    std::fprintf(stderr, "vyc: cannot open '%s'\n", input.c_str());
    return 2;
  }

  // ---- front end --------------------------------------------------------
  vy::Lexer lexer(source, input);
  std::vector<vy::Token> tokens = lexer.scan();
  if (!lexer.diagnostics().empty()) {
    print_diags(lexer.diagnostics());
    return 1;
  }

  vy::Parser parser(std::move(tokens), input);
  vy::ast::Program program = parser.parse();
  if (!program.diags.empty()) {
    print_diags(program.diags);
    return 1;
  }

  if (cmd == "tokens") {
    // re-scan for display (the parser consumed the vector)
    vy::Lexer lx(source, input);
    for (const auto& t : lx.scan())
      std::printf("%-14s %-4d:%-3d  %s\n", vy::tok_name(t.kind), t.line, t.col,
                  std::string(t.text).c_str());
    return 0;
  }

  if (cmd == "ast") {
    std::string out;
    vy::ast::dump_ast(program, out);
    std::printf("%s", out.c_str());
    return 0;
  }

  if (cmd == "fmt") {
    std::printf("%s", vy::format_program(program, source).c_str());
    return 0;
  }

  vy::Sema sema;
  bool ok = sema.run(program);
  if (!program.diags.empty()) {
    print_diags(program.diags);
    if (!ok) return 1;
  }

  if (opt.stats) {
    for (const auto& kv : sema.globals) {
      std::fprintf(stderr, "  %-20s %s\n", kv.first.c_str(),
                   vy::ty_name(kv.second));
    }
  }

  if (cmd == "check") {
    if (ok) std::printf("%s: ok\n", input.c_str());
    return ok ? 0 : 1;
  }

  // ---- backend ----------------------------------------------------------
  if (cmd == "run") {
    vy::Interp interp(program, input);
    return interp.run();
  }

  // native compilation
  vy::Driver driver(input, opt);
  return driver.compile(program, sema, source);
}
