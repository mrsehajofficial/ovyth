// Vayu :: compiler/lexer/diagnostic.h
// A source position plus a human-readable message, shared by every stage.
#pragma once

#include <string>

namespace vy {

struct Diagnostic {
  std::string file;
  int line = 0;
  int col = 0;
  std::string message;
  bool is_error = true;

  std::string render() const {
    return file + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " +
           (is_error ? "error: " : "note: ") + message;
  }
};

}  // namespace vy
