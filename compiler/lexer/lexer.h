// Ovyth :: lexer/lexer.h
// Hand-written scanner. Produces a vector<Token> for the whole file.
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.h"
#include "token.h"

namespace ov {

struct CompileError : std::runtime_error {
  CompileError(const std::string& msg, int line, int col)
      : std::runtime_error(msg), line(line), col(col) {}
  int line;
  int col;
  std::string file;
};

class Lexer {
 public:
  Lexer(std::string_view src, std::string filename = "<input>")
      : src_(src), file_(std::move(filename)) {}

  std::vector<Token> scan();
  const std::vector<Diagnostic>& diagnostics() const { return diags_; }

 private:
  Token make(Tok k, size_t start, size_t n) const;
  void  push(Token t) { out_.push_back(std::move(t)); }
  bool  match(char c);   // consume-and-match: advances if next char is `c`
  bool  match2(char a, char b);
  char  peek(int off = 0) const;
  char  advance();
  bool  at_end() const { return pos_ >= src_.size(); }
  void  skip_trivia();       // whitespace + comments
  void  scan_string(Token& t);
  void  scan_number(Token& t);
  void  error(const std::string& msg, size_t at);

  std::string_view src_;
  std::string file_;
  size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;
  std::vector<Token> out_;
  std::vector<Diagnostic> diags_;
};

}  // namespace ov