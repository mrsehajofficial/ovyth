// Vayu :: lexer/token.h
// Token vocabulary for the Vayu language.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vy {

enum class Tok : uint8_t {
  // ---- literals / names -------------------------------------------------
  IDENT,    // foo
  INT,      // 42
  FLOAT,    // 3.14
  STRING,   // "hello"  (decoded, escapes applied)

  // ---- keywords ---------------------------------------------------------
  KW_FUNCTION, KW_RETURN, KW_IF, KW_ELSE, KW_FOR, KW_IN,  // function return if else for in
  KW_WHILE,  KW_BREAK, KW_CONTINUE,                      // while break continue
  KW_TRUE, KW_FALSE,                                     // true false
  KW_NULL,                                               // null
  KW_TRY, KW_CATCH, KW_THROW,                            // try catch throw
  KW_AND, KW_OR, KW_NOT,                                 // and or not
  KW_AS,                                                 // as   (error as e)
  KW_ENUM,                                                // enum
  KW_BREAKPOINT,                                         // debug

  // ---- punctuation / operators -----------------------------------------
  LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET,
  COMMA, COLON, DOT, SEMICOLON, QUESTION,                // , : . ; ?
  ARROW,   // ->     (typed functions, return type)
  FATARROW,// =>
  ELLIPSIS,// ...

  ASSIGN,        // =
  PLUS, MINUS, STAR, SLASH, PERCENT,
  BANG, AMP, PIPE, CARET, TILDE,        // ! & | ^ ~
  BANG_EQUAL, EQUAL, LESS, LESS_EQUAL, GREATER, GREATER_EQUAL,  // == <= >= >

  PLUS_EQUAL, MINUS_EQUAL, STAR_EQUAL, SLASH_EQUAL, PERCENT_EQUAL,

  SHL, SHR,   // << >>
  DOUBLE_STAR,// **

  INCREMENT, DECREMENT,  // ++ --

  // ---- literals / operators (continued) --------------------------------
  EOF_,
  INVALID,
};

const char* tok_name(Tok t);

struct Token {
  Tok kind = Tok::EOF_;
  std::string_view text;   // raw source slice
  std::string str;        // decoded value for STRING
  double fval = 0.0;
  int64_t ival = 0;

  int line = 0;
  int col = 0;

  bool is(Tok k) const { return kind == k; }
};

}  // namespace vy