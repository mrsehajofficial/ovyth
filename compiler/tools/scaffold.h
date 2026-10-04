// Vayu :: compiler/tools/scaffold.h
//
// `vyc init` -- create a new Vayu project from a built-in template.
#pragma once

#include <string>

namespace vy {

// Create `dir` and fill it with a starter project. `dir` is created when it
// does not exist; an existing directory is reused only while it is empty, so
// `vyc init` can never clobber someone's files.
//
// `tmpl` picks the starter -- see scaffold_templates() for the names.
//
// Returns true on success and leaves `err` empty. On failure it returns false,
// writes a human-readable reason to `err`, and leaves the filesystem untouched
// whenever it can (the directory check happens before anything is written).
bool scaffold_project(const std::string& dir, const std::string& tmpl, std::string& err);

// The available template names, comma-separated, for --help and error text.
std::string scaffold_templates();  // "hello, http, cli"

}  // namespace vy
