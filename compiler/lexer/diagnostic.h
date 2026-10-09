// Ovyth :: compiler/lexer/diagnostic.h
// A source position plus a human-readable message, shared by every
// stage. Diagnostics render plain for pipes and logs, and pretty
// (calm icons + color) on a terminal, so an error reads as a
// helpful note instead of a wall of red text.
#pragma once

#include <cstdio>
#include <string>
#include <unistd.h>

namespace ov {

enum class DiagKind { Error, Warning, Info, Note };

// Colors and icons are only emitted when stderr is a terminal,
// so piped output stays byte-stable for scripts and test runs.
inline bool diag_tty() { return isatty(fileno(stderr)) != 0; }

inline const char* kColorReset  = "\033[0m";
inline const char* kColorRed    = "\033[31m";
inline const char* kColorGreen  = "\033[32m";
inline const char* kColorYellow = "\033[33m";
inline const char* kColorCyan   = "\033[36m";
inline const char* kColorDim    = "\033[90m";

inline const char* diag_icon(DiagKind k) {
  switch (k) {
    case DiagKind::Error:   return "✖";
    case DiagKind::Warning: return "▲";
    case DiagKind::Info:    return "ℹ";
    case DiagKind::Note:    return "•";
  }
  return "•";
}

inline const char* diag_color(DiagKind k) {
  switch (k) {
    case DiagKind::Error:   return kColorRed;
    case DiagKind::Warning: return kColorYellow;
    case DiagKind::Info:    return kColorCyan;
    case DiagKind::Note:    return kColorDim;
  }
  return kColorDim;
}

inline std::string diag_paint(const char* color, const std::string& text) {
  if (!diag_tty()) return text;
  return std::string(color) + text + kColorReset;
}

struct Diagnostic {
  std::string file;
  int line = 0;
  int col = 0;
  std::string message;
  DiagKind kind = DiagKind::Error;

  const char* label() const {
    switch (kind) {
      case DiagKind::Error:   return "error";
      case DiagKind::Warning: return "warning";
      case DiagKind::Info:    return "info";
      case DiagKind::Note:    return "note";
    }
    return "note";
  }

  // terminal:  main.ov:3:5: ✖ error: expected a number but got a string
  // piped:     main.ov:3:5: error: expected a number but got a string
  std::string render() const {
    std::string out = file + ":" + std::to_string(line) + ":" +
                      std::to_string(col) + ": ";
    std::string tag = std::string(diag_icon(kind)) + " " + label() + ": ";
    if (diag_tty()) tag = diag_paint(diag_color(kind), tag);
    return out + tag + message;
  }
};

// `ovc: ✖ <message>` — a problem with the command itself.
inline void cli_error(const std::string& message) {
  std::fprintf(stderr, "ovc: %s%s\n",
               diag_paint(kColorRed, "✖ ").c_str(),
               message.c_str());
}

// `✓ <message>` — something went right.
inline void cli_ok(const std::string& message) {
  std::printf("%s%s\n",
              diag_paint(kColorGreen, "✓ ").c_str(),
              message.c_str());
}

// `ℹ <message>` — progress worth mentioning, nothing more.
inline void cli_info(const std::string& message) {
  std::printf("%s%s\n",
              diag_paint(kColorCyan, "ℹ ").c_str(),
              message.c_str());
}

}  // namespace ov
