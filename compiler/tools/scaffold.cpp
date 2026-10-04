// Vayu :: compiler/tools/scaffold.cpp
//
// `vyc init` turns an empty directory into a project that builds. The
// templates live in the compiler rather than in a templates/ directory on
// disk, so the command behaves the same from a build tree, an installed
// prefix, or a tarball: there is no data file to lose track of.
#include "scaffold.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace vy {
namespace {

bool exists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

bool is_dir(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// A directory is usable when it does not exist, or exists and holds nothing.
// `existed` reports which of the two it was, so the caller knows whether it
// still has to mkdir.
bool dir_is_free(const std::string& p, bool& existed) {
  existed = exists(p);
  if (!existed) return true;
  if (!is_dir(p)) return false;
  DIR* d = ::opendir(p.c_str());
  if (!d) return false;
  bool empty = true;
  while (struct dirent* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
      continue;
    empty = false;
    break;
  }
  ::closedir(d);
  return empty;
}

std::string base_name(const std::string& path) {
  size_t end = path.find_last_of('/');
  return (end == std::string::npos) ? path : path.substr(end + 1);
}

bool write_file(const std::string& path, const std::string& body, std::string& err) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    err = "cannot write " + path;
    return false;
  }
  size_t n = std::fwrite(body.data(), 1, body.size(), f);
  std::fclose(f);
  if (n != body.size()) {
    err = "short write to " + path;
    return false;
  }
  return true;
}

// ---- templates -----------------------------------------------------------

std::string hello_main() {
  return R"VY(// main.vy -- a Vayu starter program
//
//   vyc run main.vy     run it instantly with the interpreter
//   vyc main.vy         compile it to a native executable

function greet(name) {
    return "Hello, " + name + "!"
}

for name in ["world", "Vayu"] {
    print(greet(name))
}
)VY";
}

std::string http_main() {
  return R"VY(// main.vy -- call an HTTP API and read the JSON reply
//
//   vyc run main.vy     try it now
//   vyc main.vy         compile a standalone executable
//
// Point `url` at your own service; the environment variable API_URL
// overrides it without touching the source.

url = env_or("API_URL", "https://api.github.com/repos/torvalds/linux")

try {
    response = http.get(url, headers = {"Accept": "application/json"})
    print("status:", response.status)

    if response.ok {
        data = json.parse(response.body)
        print("name:", data["full_name"])
        print("stars:", data["stargazers_count"])
    } else {
        print(response.body)
    }
} catch error {
    print("request failed:", error)
}
)VY";
}

std::string cli_main() {
  return R"VY(// main.vy -- a small interactive command loop
//
//   vyc run main.vy     try it now
//   vyc main.vy         compile a standalone executable

print("type something, or 'quit' to exit")

while true {
    line = input("> ")
    if line == "quit" or line == "" {
        break
    }
    print("you said:", line)
}

print("bye")
)VY";
}

std::string project_readme(const std::string& name) {
  return "# " + name + "\n\n"
         "A Vayu program.\n\n"
         "## Build and run\n\n"
         "```bash\n"
         "make run       # interpreter -- instant, best while coding\n"
         "make build     # compile to build/app\n"
         "make release   # -O3, LTO, symbols stripped\n"
         "```\n\n"
         "The generated Makefile calls `vyc`, so the compiler needs to be on\n"
         "your `PATH`. To use a compiler somewhere else:\n\n"
         "```bash\n"
         "make run VYC=/path/to/vyc\n"
         "```\n\n"
         "## Files\n\n"
         "```\n"
         "main.vy      the program\n"
         "Makefile     build commands\n"
         "```\n";
}

std::string project_gitignore() {
  return "# build output\n"
         "build/\n"
         "\n"
         "# a binary produced by a bare `vyc main.vy`\n"
         "app\n"
         "main\n"
         "\n"
         "*.o\n"
         "*.ll\n"
         "*.d\n";
}

// The generated Makefile needs real tab characters in its recipes, so this is
// assembled with escapes instead of a raw string literal.
std::string project_makefile(const std::string& name) {
  std::string s;
  s += "# " + name + " -- build with the Vayu compiler\n";
  s += "#\n";
  s += "#   make run       interpreter, instant\n";
  s += "#   make build     native executable at build/app\n";
  s += "#   make release   -O3, LTO, stripped\n";
  s += "\n";
  s += "VYC ?= vyc\n";
  s += "SRC ?= main.vy\n";
  s += "OUT ?= build/app\n";
  s += "\n";
  s += ".PHONY: run build release clean\n";
  s += "\n";
  s += "run:\n\t$(VYC) run $(SRC)\n";
  s += "\n";
  s += "build:\n\t@mkdir -p build\n\t$(VYC) $(SRC) -o $(OUT)\n";
  s += "\n";
  s += "release:\n\t@mkdir -p build\n\t$(VYC) $(SRC) --release -o $(OUT)\n";
  s += "\n";
  s += "clean:\n\trm -rf build\n";
  return s;
}

}  // namespace

std::string scaffold_templates() { return "hello, http, cli"; }

bool scaffold_project(const std::string& dir, const std::string& tmpl, std::string& err) {
  err.clear();
  if (dir.empty()) {
    err = "init needs a directory name";
    return false;
  }

  std::string main_src;
  if (tmpl == "hello")
    main_src = hello_main();
  else if (tmpl == "http")
    main_src = http_main();
  else if (tmpl == "cli")
    main_src = cli_main();
  else {
    err = "unknown template '" + tmpl + "' (available: " + scaffold_templates() + ")";
    return false;
  }

  bool existed = false;
  if (!dir_is_free(dir, existed)) {
    if (!is_dir(dir)) err = "'" + dir + "' exists and is not a directory";
    else err = "'" + dir + "' already exists and is not empty; refusing to overwrite";
    return false;
  }

  if (!existed && ::mkdir(dir.c_str(), 0755) != 0) {
    err = "cannot create directory " + dir;
    return false;
  }

  std::string path = dir;
  while (path.size() > 1 && path.back() == '/') path.pop_back();

  std::string name = base_name(path);
  if (name.empty() || name == "." || name == "..") name = "app";

  const struct {
    const char* file;
    std::string body;
  } files[] = {
      {"main.vy", main_src},
      {"README.md", project_readme(name)},
      {".gitignore", project_gitignore()},
      {"Makefile", project_makefile(name)},
  };

  for (const auto& f : files) {
    std::string p = path + "/" + f.file;
    if (!write_file(p, f.body, err)) return false;
  }
  return true;
}

}  // namespace vy
