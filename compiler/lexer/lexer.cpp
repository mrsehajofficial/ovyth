#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace ov {
namespace {

bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool is_ident_part(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

const std::unordered_map<std::string_view, Tok>& keywords() {
  static const std::unordered_map<std::string_view, Tok> k = {
      {"function", Tok::KW_FUNCTION}, {"return", Tok::KW_RETURN},
      {"if", Tok::KW_IF},           {"else", Tok::KW_ELSE},
      {"for", Tok::KW_FOR},         {"in", Tok::KW_IN},
      {"while", Tok::KW_WHILE},     {"break", Tok::KW_BREAK},
      {"continue", Tok::KW_CONTINUE}, {"true", Tok::KW_TRUE},
      {"false", Tok::KW_FALSE},     {"null", Tok::KW_NULL},
      {"try", Tok::KW_TRY},         {"catch", Tok::KW_CATCH},
      {"throw", Tok::KW_THROW},     {"and", Tok::KW_AND},
      {"or", Tok::KW_OR},           {"not", Tok::KW_NOT},
      {"as", Tok::KW_AS},           {"enum", Tok::KW_ENUM},
      {"debug", Tok::KW_BREAKPOINT}, {"import", Tok::KW_IMPORT},
  };
  return k;
}

}  // namespace

char Lexer::peek(int off) const {
  size_t p = pos_ + (size_t)off;
  return p < src_.size() ? src_[p] : '\0';
}

char Lexer::advance() {
  char c = src_[pos_++];
  if (c == '\n') { line_++; col_ = 1; } else { col_++; }
  return c;
}

bool Lexer::match(char c) {
  if (peek() == c) { advance(); return true; }
  return false;
}

bool Lexer::match2(char a, char b) {
  if (peek() == a && peek(1) == b) { advance(); advance(); return true; }
  return false;
}

Token Lexer::make(Tok k, size_t start, size_t n) const {
  Token t;
  t.kind = k;
  t.text = src_.substr(start, n);
  return t;
}

void Lexer::error(const std::string& msg, size_t at) {
  int l = line_, c = col_;
  // recompute line/col if the error is slightly ahead
  (void)at;
  diags_.push_back(Diagnostic{file_, l, c, msg, DiagKind::Error});
}

// Whitespace, line comments (// ... ), block comments (/* ... */, nestable)
void Lexer::skip_trivia() {
  for (;;) {
    while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\r' ||
                         peek() == '\n')) {
      advance();
    }
    if (peek() == '/' && peek(1) == '/') {
      while (!at_end() && peek() != '\n') advance();
      continue;
    }
    if (peek() == '/' && peek(1) == '*') {
      int open_line = line_;
      advance(); advance();
      int depth = 1;
      while (!at_end() && depth > 0) {
        if (match2('*', '/')) { depth--; }
        else if (match2('/', '*')) { depth++; }
        else advance();
      }
      if (depth > 0) diags_.push_back(
          Diagnostic{file_, open_line, 1, "unterminated block comment",
                     DiagKind::Error});
      continue;
    }
    return;
  }
}

void Lexer::scan_string(Token& t) {
  // pos_ sits on the opening quote. t.text spans the literal.
  size_t start = pos_;
  int start_line = line_;
  (void)start_line;
  advance();  // opening quote
  std::string val;
  for (;;) {
    if (at_end() || peek() == '\n') {
      error("unterminated string literal", pos_);
      t.kind = Tok::INVALID;
      break;
    }
    if (peek() == '"') { advance(); break; }

    char c = advance();
    if (c != '\\') { val.push_back(c); continue; }

    // escape sequence
    char e = advance();
    switch (e) {
      case 'n':  val.push_back('\n'); break;
      case 't':  val.push_back('\t'); break;
      case 'r':  val.push_back('\r'); break;
      case '0':  val.push_back('\0'); break;
      case '\\': val.push_back('\\'); break;
      case '"':  val.push_back('"');  break;
      case '\'': val.push_back('\''); break;
      case '`':  val.push_back('`');  break;
      case 'b':  val.push_back('\b'); break;
      case 'f':  val.push_back('\f'); break;
      case 'v':  val.push_back('\v'); break;
      case 'u': {
        // \u{XXXX}
        if (peek() != '{') { error("expected '{' after \\u", pos_); break; }
        advance();
        std::string hex;
        while (!at_end() && peek() != '}') hex.push_back(advance());
        if (at_end()) { error("unterminated \\u{...} escape", pos_); break; }
        advance();  // '}'
        if (hex.size() > 6) hex = hex.substr(hex.size() - 6);
        uint32_t cp = (uint32_t)strtoul(hex.c_str(), nullptr, 16);
        // UTF-8 encode
        if (cp < 0x80) {
          val.push_back((char)cp);
        } else if (cp < 0x800) {
          val.push_back((char)(0xC0 | (cp >> 6)));
          val.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
          val.push_back((char)(0xE0 | (cp >> 12)));
          val.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
          val.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
          val.push_back((char)(0xF0 | (cp >> 18)));
          val.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
          val.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
          val.push_back((char)(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default:
        error(std::string("unknown escape sequence '\\") + e + "'", pos_);
        val.push_back(e);
    }
  }
  // Preserve an error kind: if the literal ran to end-of-file, scan_string()
  // already marked the token INVALID and recorded a diagnostic. Overwriting it
  // with STRING here would hand the parser a "successful" token that is really
  // half a literal, and it would then report a misleading cascade of errors.
  if (t.kind != Tok::INVALID) t.kind = Tok::STRING;
  t.text = src_.substr(start, pos_ - start);
  t.str = std::move(val);
}

void Lexer::scan_number(Token& t) {
  size_t start = pos_;
  bool is_float = false;

  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
    advance(); advance();
    while (!at_end() && (std::isxdigit((unsigned char)peek()) || peek() == '_')) advance();
    std::string digits(src_.substr(start + 2, pos_ - start - 2));
    std::string clean;
    for (char c : digits) if (c != '_') clean.push_back(c);
    t.kind = Tok::INT;
    t.ival = (int64_t)strtoull(clean.c_str(), nullptr, 16);
    t.text = src_.substr(start, pos_ - start);
    return;
  }
  if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
    advance(); advance();
    size_t ds = pos_;
    while (!at_end() && (peek() == '0' || peek() == '1' || peek() == '_')) advance();
    std::string clean;
    for (size_t i = ds; i < pos_; ++i) if (src_[i] != '_') clean.push_back(src_[i]);
    t.kind = Tok::INT;
    t.ival = (int64_t)strtoull(clean.c_str(), nullptr, 2);
    t.text = src_.substr(start, pos_ - start);
    return;
  }
  if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
    advance(); advance();
    size_t ds = pos_;
    while (!at_end() && ((peek() >= '0' && peek() <= '7') || peek() == '_')) advance();
    std::string clean;
    for (size_t i = ds; i < pos_; ++i) if (src_[i] != '_') clean.push_back(src_[i]);
    t.kind = Tok::INT;
    t.ival = (int64_t)strtoull(clean.c_str(), nullptr, 8);
    t.text = src_.substr(start, pos_ - start);
    return;
  }

  while (!at_end() && (std::isdigit((unsigned char)peek()) || peek() == '_')) advance();
  if (peek() == '.' && std::isdigit((unsigned char)peek(1))) {
    is_float = true;
    advance();
    while (!at_end() && (std::isdigit((unsigned char)peek()) || peek() == '_')) advance();
  }
  if (peek() == 'e' || peek() == 'E') {
    is_float = true;
    advance();
    if (peek() == '+' || peek() == '-') advance();
    if (!std::isdigit((unsigned char)peek())) {
      error("exponent has no digits", pos_);
    }
    while (!at_end() && std::isdigit((unsigned char)peek())) advance();
  }

  std::string text(src_.substr(start, pos_ - start));
  std::string clean;
  for (char c : text) if (c != '_') clean.push_back(c);

  t.text = src_.substr(start, pos_ - start);
  if (is_float) {
    t.kind = Tok::FLOAT;
    t.fval = strtod(clean.c_str(), nullptr);
  } else {
    t.kind = Tok::INT;
    t.ival = (int64_t)strtoll(clean.c_str(), nullptr, 10);
  }
}

std::vector<Token> Lexer::scan() {
  while (!at_end()) {
    skip_trivia();
    if (at_end()) break;

    size_t start = pos_;
    int line = line_, col = col_;
    char c = peek();

    if (c == '"') {
      Token t;
      scan_string(t);
      t.line = line;
      t.col = col;
      push(std::move(t));
      continue;
    }

    if (std::isdigit((unsigned char)c)) {
      Token t;
      scan_number(t);
      t.line = line;
      t.col = col;
      push(std::move(t));
      continue;
    }

    if (is_ident_start(c)) {
      while (!at_end() && is_ident_part(peek())) advance();
      std::string_view text = src_.substr(start, pos_ - start);
      Token t = make(Tok::IDENT, start, pos_ - start);
      auto it = keywords().find(text);
      if (it != keywords().end()) t.kind = it->second;
      t.line = line;
      t.col = col;
      push(std::move(t));
      continue;
    }

    advance();  // consume the operator char
    Token t;
    switch (c) {
      case '(': t.kind = Tok::LPAREN; break;
      case ')': t.kind = Tok::RPAREN; break;
      case '{': t.kind = Tok::LBRACE; break;
      case '}': t.kind = Tok::RBRACE; break;
      case '[': t.kind = Tok::LBRACKET; break;
      case ']': t.kind = Tok::RBRACKET; break;
      case ',': t.kind = Tok::COMMA; break;
      case ':': t.kind = Tok::COLON; break;
      case ';': t.kind = Tok::SEMICOLON; break;
      case '?': t.kind = Tok::QUESTION; break;
      case '~': t.kind = Tok::TILDE; break;
      case '.':
        if (peek() == '.' && peek(1) == '.') { advance(); advance(); t.kind = Tok::ELLIPSIS; }
        else t.kind = Tok::DOT;
        break;
      case '+':
        if (match('=')) t.kind = Tok::PLUS_EQUAL;
        else if (match('+')) t.kind = Tok::INCREMENT;
        else t.kind = Tok::PLUS;
        break;
      case '-':
        if (match('=')) t.kind = Tok::MINUS_EQUAL;
        else if (match('-')) t.kind = Tok::DECREMENT;
        else if (match('>')) t.kind = Tok::ARROW;
        else t.kind = Tok::MINUS;
        break;
      case '*':
        if (match('*')) t.kind = Tok::DOUBLE_STAR;
        else if (match('=')) t.kind = Tok::STAR_EQUAL;
        else t.kind = Tok::STAR;
        break;
      case '/':
        if (match('=')) t.kind = Tok::SLASH_EQUAL;
        else t.kind = Tok::SLASH;
        break;
      case '%':
        if (match('=')) t.kind = Tok::PERCENT_EQUAL;
        else t.kind = Tok::PERCENT;
        break;
      case '=':
        if (match('=')) t.kind = Tok::EQUAL;
        else if (match('>')) t.kind = Tok::FATARROW;
        else t.kind = Tok::ASSIGN;
        break;
      case '!':
        if (match('=')) t.kind = Tok::BANG_EQUAL;
        else t.kind = Tok::BANG;
        break;
      case '&':
        advance();  // '&&' and '&' both map to logical and / bitwise and
        t.kind = Tok::AMP;
        break;
      case '|':
        advance();  // '||' and '|' both map to logical or / bitwise or
        t.kind = Tok::PIPE;
        break;
      case '^': t.kind = Tok::CARET; break;
      case '<':
        if (match('=')) t.kind = Tok::LESS_EQUAL;
        else if (match('<')) t.kind = Tok::SHL;
        else t.kind = Tok::LESS;
        break;
      case '>':
        if (match('=')) t.kind = Tok::GREATER_EQUAL;
        else if (match('>')) t.kind = Tok::SHR;
        else t.kind = Tok::GREATER;
        break;
      default: {
        // Not a recognised operator.
        pos_ = start;
        error(std::string("unexpected character '") + c + "'", pos_);
        advance();
        continue;
      }
    }
    t.text = src_.substr(start, pos_ - start);
    t.line = line;
    t.col = col;
    push(std::move(t));
  }

  Token eof;
  eof.kind = Tok::EOF_;
  eof.text = "";
  eof.line = line_;
  eof.col = col_;
  push(std::move(eof));
  return std::move(out_);
}

}  // namespace ov