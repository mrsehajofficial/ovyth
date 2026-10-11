// Ovyth :: compiler/tools/ui.h
//
// The pretty layer.
//
// Ovyth has a face: a small sprite with sparkle eyes, drawn in the wordmark
// below. She shows up wherever a human is watching — the installer, `ovc
// version`, `ovc init`, and every `ovc pkg` command — and steps aside the
// moment output stops being for a human.
//
// That second part is the rule this file exists to enforce: **decoration is
// TTY-only, and piped output stays byte-for-byte what it was.** `tests/pkg.sh`
// greps real strings out of `ovc pkg` output, and `tests/interp.sh` compares
// backends byte for byte, so a box drawn into a pipe would break the suite and
// anyone's script. Every helper here therefore checks `ui_tty()` first and
// falls back to the plain line it replaces.
//
// `NO_COLOR` (https://no-color.org) is honoured too: set it and the structure
// survives while the colour does not.
//
// Header-only and `inline`, in the style of lexer/diagnostic.h, so nothing is
// added to the build.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lexer/diagnostic.h"  // the palette and diag_tty()

namespace ov {

// Two additions to the palette: magenta is Ovyth's colour (the sprite, the
// sparkle, the divider), bold is for emphasis inside a panel.
inline const char* kColorMagenta = "\033[35m";
inline const char* kColorBold    = "\033[1m";

// Is stdout a terminal? This is the gate on everything below. Errors already
// go through diag_tty() on stderr; this is the stdout half of the same idea.
inline bool ui_tty() { return isatty(fileno(stdout)) != 0; }

// Colour is a separate decision from decoration: NO_COLOR turns it off while
// leaving boxes, dot leaders and icons in place.
inline bool ui_color() {
  if (!ui_tty()) return false;
  const char* no = ::getenv("NO_COLOR");
  return no == nullptr || *no == '\0';
}

inline std::string ui_paint(const char* color, const std::string& text) {
  if (!ui_color()) return text;
  return std::string(color) + text + kColorReset;
}

inline std::string ui_magenta(const std::string& s) { return ui_paint(kColorMagenta, s); }
inline std::string ui_dim(const std::string& s) { return ui_paint(kColorDim, s); }
inline std::string ui_bold(const std::string& s) { return ui_paint(kColorBold, s); }
inline std::string ui_green(const std::string& s) { return ui_paint(kColorGreen, s); }
inline std::string ui_cyan(const std::string& s) { return ui_paint(kColorCyan, s); }
inline std::string ui_red(const std::string& s) { return ui_paint(kColorRed, s); }
inline std::string ui_yellow(const std::string& s) { return ui_paint(kColorYellow, s); }

// The glyphs. Geometric Unicode only — no emoji, whose width varies by
// terminal and would quietly misalign every panel.
inline const char* kSpark  = "✦";  // Ovyth's mark
inline const char* kStep   = "⬡";  // a step in progress
inline const char* kOk     = "✓";  // done
inline const char* kFail   = "✕";  // this step failed
inline const char* kHeart  = "♥";  // the installer's sign-off
inline const char* kInfo   = "ℹ";  // reuses the diagnostics icon
inline const char* kWarn   = "▲";
inline const char* kNote   = "•";

// ---------------------------------------------------------------------------
// width helpers
//
// Everything below measures display columns, not bytes.
// ---------------------------------------------------------------------------

inline constexpr int kPanelWidth = 44;

// Space to a fixed width, truncating (with an ellipsis) if it does not fit.
inline std::string ui_fit(const std::string& s, int width) {
  if ((int)s.size() <= width) return s + std::string(width - (int)s.size(), ' ');
  if (width <= 1) return std::string(std::max(0, width), '.');
  return s.substr(0, width - 1) + "…";
}

// Repeat a *string* n times. box-drawing characters are multi-byte UTF-8, so
// they cannot go in a character literal.
inline std::string ui_repeat(const std::string& unit, int n) {
  std::string out;
  for (int i = 0; i < n; i++) out += unit;
  return out;
}

inline const char* kDash = "─";

// Display columns, not bytes. `─`, `✦`, `✓` are three bytes each but one
// column wide, and every width calculation below has to agree with the
// terminal rather than with std::string::size().
inline int ui_cols(const std::string& s) {
  int n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) n++;  // count UTF-8 lead bytes only
  return n;
}

// Truncate to a display width, marking the cut.
inline std::string ui_clip(const std::string& s, int width) {
  if (ui_cols(s) <= width) return s;
  if (width <= 1) return ui_repeat(".", std::max(0, width));
  int budget = width - 1;
  std::string out;
  for (size_t i = 0; i < s.size() && budget > 0; i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c & 0xC0) != 0x80) {
      if (budget == 0) break;
      budget--;
    }
    out += s[i];
  }
  return out + "…";
}

// Space to a fixed display width, truncating (with an ellipsis) if it does not
// fit. Piped tables rely on this matching the old printf widths.

// ---------------------------------------------------------------------------
// the sprite
// ---------------------------------------------------------------------------

// Ovyth herself: a small sitting sprite with sparkle eyes, beside her name.
// `version` is optional and appears as a dim chip on the name line.
//
// Nothing is drawn when output is piped — the callers keep their plain output
// (`ovc version` prints "ovc 0.1.3 (Ovyth)" and nothing else).
inline void ui_wordmark(const std::string& version = "") {
  if (!ui_tty()) return;
  const std::string mark = ui_magenta(kSpark);
  const std::string name = ui_bold("O v y t h");
  const std::string ver = version.empty() ? std::string() : "   " + ui_dim(version);
  const std::string tag = ui_dim("a tiny compiled");
  const std::string tag2 = ui_dim("language");
  const std::string heart = ui_magenta(kHeart);

  std::printf("\n");
  std::printf("   %s\n", mark.c_str());
  std::printf("       /\\      /\\          %s %s %s%s\n", mark.c_str(),
              name.c_str(), mark.c_str(), ver.c_str());
  std::printf("      /  \\____/  \\         %s\n", tag.c_str());
  std::printf("     |  *      *  |         %s\n", tag2.c_str());
  std::printf("     |      w      |\n");
  std::printf("     |   '----'   |\n");
  std::printf("      \\          /\n");
  std::printf("       '.______.'\n");
  std::printf("         |  |        %s\n", heart.c_str());
  std::printf("        (_)(_)\n");
  std::printf("\n");
}

// The one-line form, for commands too short to deserve the full sprite.
inline void ui_mark(const std::string& label = "") {
  if (!ui_tty()) return;
  std::printf("   %s %s\n", ui_magenta(kSpark).c_str(), label.c_str());
}

// A decorative rule:   ✦ ────────────── ✦
inline void ui_divider() {
  if (!ui_tty()) return;
  std::printf("   %s %s %s\n", ui_magenta(kSpark).c_str(),
              ui_dim("──────────────────────────────────").c_str(),
              ui_magenta(kSpark).c_str());
}

// ---------------------------------------------------------------------------
// steps:  `⬡ doing a thing ......... done`
// ---------------------------------------------------------------------------

// Start a step. The label is clipped to a column budget and padded with dot
// leaders so the outcome lines up; the prefix is printed immediately and
// flushed so the user sees motion during a slow build or download.
inline void ui_step(const std::string& label) {
  if (!ui_tty()) return;
  std::string l = ui_clip(label, 46);
  std::printf("   %s %s %s ", ui_cyan(kStep).c_str(), l.c_str(),
              ui_dim(ui_repeat(".", 46 - ui_cols(l))).c_str());
  std::fflush(stdout);
}

// Finish the step started by ui_step. `note` replaces "done" when there is
// something worth saying ("done (4.2s)", "ok (cached)", "1.0.0").
inline void ui_step_done(const std::string& note = "") {
  if (!ui_tty()) return;
  std::string out = note.empty() ? ui_green("done") : ui_green(note);
  std::printf("%s\n", out.c_str());
}

// Finish it badly. The step's reason stays on the same line, and the caller
// is expected to print the full error afterwards.
inline void ui_step_fail(const std::string& reason = "") {
  if (!ui_tty()) return;
  std::printf("%s %s\n", ui_red(kFail).c_str(),
              ui_red(reason.empty() ? "failed" : reason).c_str());
}

// A second-level line under a step: an installed path, a hint, a filename.
inline void ui_bullet(const std::string& line) {
  if (!ui_tty()) return;
  std::printf("       %s %s\n", ui_dim(kNote).c_str(), line.c_str());
}

// A dim hint line, no icon.
inline void ui_hint(const std::string& line) {
  if (!ui_tty()) return;
  std::printf("       %s\n", ui_dim(line).c_str());
}

// ---------------------------------------------------------------------------
// panels:  `┌─ try it ───┐` / `│  ...  │` / `└────────┘`
// ---------------------------------------------------------------------------

// Width between the two borders. Fixed, so panels align and no one needs
// termios to read the terminal size.
inline void ui_panel_begin(const std::string& title = "") {
  if (!ui_tty()) return;
  // "─ title " followed by enough dashes to fill the width.
  std::string head = title.empty() ? std::string(kDash)
                                   : (std::string(kDash) + " " + title + " ");
  int used = ui_cols(head);
  if (used > kPanelWidth) {
    head = ui_clip(head, kPanelWidth);
    used = kPanelWidth;
  }
  std::printf("   ┌%s%s┐\n", ui_dim(head).c_str(),
              ui_dim(ui_repeat(kDash, kPanelWidth - used)).c_str());
}

// `left` is the text; `right` is an optional right-aligned annotation, so a
// panel can hold `cd myapp` with `run it instantly` beside it.
inline void ui_panel_line(const std::string& left, const std::string& right = "") {
  if (!ui_tty()) {
    // Piped: the panel is its contents, unbordered.
    if (right.empty())
      std::printf("%s\n", left.c_str());
    else
      std::printf("%s  %s\n", left.c_str(), right.c_str());
    return;
  }
  // inner is the text width between the two spaces of padding.
  const int inner = kPanelWidth - 2;
  std::string content = left;
  if (!right.empty()) {
    int gap = inner - ui_cols(left) - ui_cols(right);
    // Rather than break the border, drop the annotation and keep the text.
    if (gap >= 2) content = left + std::string(gap, ' ') + right;
  }
  content = ui_clip(content, inner);
  content += std::string(std::max(0, inner - ui_cols(content)), ' ');
  std::printf("   │ %s │\n", content.c_str());
}

// A `key value` row, as `pkg info` has always printed it: a 12-wide field then
// the value. This is the one panel line whose piped form is load-bearing —
// tests grep `installed   yes` — so the width is fixed here rather than
// derived.
inline void ui_field(const std::string& key, const std::string& value) {
  if (!ui_tty()) {
    std::printf("%-12s%s\n", key.c_str(), value.c_str());
    return;
  }
  // Padding is measured in columns, then colour is applied, so the escape
  // codes never eat into the box.
  std::string v = ui_clip(value, kPanelWidth - 3 - ui_cols(key));
  int pad = kPanelWidth - 2 - ui_cols(key) - 1 - ui_cols(v);
  if (pad < 0) pad = 0;
  std::printf("   │ %s %s%s │\n", ui_bold(key).c_str(), v.c_str(),
              std::string(pad, ' ').c_str());
}

inline void ui_panel_end() {
  if (!ui_tty()) return;
  std::printf("   └%s┘\n", ui_dim(ui_repeat(kDash, kPanelWidth)).c_str());
}

// ---------------------------------------------------------------------------
// tables
// ---------------------------------------------------------------------------

struct UiColumn {
  std::string header;
  int width;  // characters; -1 leaves the last column to run to its full length
};

// Print a table. The piped form reproduces, exactly, the printf layout
// `pkg list` and `pkg search` used before columns had a header rule: header
// row, then rows, at the same widths.
inline void ui_table(const std::vector<UiColumn>& cols,
                     const std::vector<std::vector<std::string>>& rows) {
  auto emit_row = [&](const std::vector<std::string>& r) {
    std::string line;
    for (size_t i = 0; i < cols.size(); i++) {
      if (i) line += " ";
      if (i < r.size() && !r[i].empty())
        line += (i + 1 == cols.size() || cols[i].width < 0)
                    ? r[i]
                    : ui_fit(r[i], cols[i].width);
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    std::printf("%s\n", line.c_str());
  };

  // The header is data in the piped form, decoration in the terminal one.
  std::vector<std::string> head;
  for (const auto& c : cols) head.push_back(c.header);

  if (!ui_tty()) {
    emit_row(head);
    for (const auto& r : rows) emit_row(r);
    return;
  }

  std::printf("\n");
  {
    std::string line;
    for (size_t i = 0; i < cols.size(); i++) {
      if (i) line += "  ";
      line += (i + 1 == cols.size() || cols[i].width < 0)
                  ? cols[i].header
                  : ui_fit(cols[i].header, cols[i].width);
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    std::printf("   %s\n", ui_bold(line).c_str());
  }
  std::printf("   %s\n", ui_dim(ui_repeat(kDash, 50)).c_str());
  for (const auto& r : rows) {
    std::printf("   ");
    emit_row(r);
  }
}

// ---------------------------------------------------------------------------
// status lines
// ---------------------------------------------------------------------------

// `✓ installed greet 1.0.0` — the success line of a whole command.
//
// Piped, this is the line the caller used to print through cli_ok(), icon and
// all, so nothing that greps output notices the difference.
inline void ui_success(const std::string& message) {
  if (!ui_tty()) {
    std::printf("%s %s\n", kOk, message.c_str());
    return;
  }
  std::printf("\n   %s %s\n", ui_green(kOk).c_str(), message.c_str());
}

// Piped: the cli_info() line, unchanged.
inline void ui_info_line(const std::string& message) {
  if (!ui_tty()) {
    std::printf("%s %s\n", kInfo, message.c_str());
    return;
  }
  std::printf("   %s %s\n", ui_cyan(kInfo).c_str(), message.c_str());
}

}  // namespace ov
