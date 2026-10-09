#include "token.h"

namespace ov {

const char* tok_name(Tok t) {
  switch (t) {
    case Tok::IDENT: return "IDENT";
    case Tok::INT: return "INT";
    case Tok::FLOAT: return "FLOAT";
    case Tok::STRING: return "STRING";
    case Tok::KW_FUNCTION: return "function";
    case Tok::KW_RETURN: return "return";
    case Tok::KW_IF: return "if";
    case Tok::KW_ELSE: return "else";
    case Tok::KW_FOR: return "for";
    case Tok::KW_IN: return "in";
    case Tok::KW_WHILE: return "while";
    case Tok::KW_BREAK: return "break";
    case Tok::KW_CONTINUE: return "continue";
    case Tok::KW_TRUE: return "true";
    case Tok::KW_FALSE: return "false";
    case Tok::KW_NULL: return "null";
    case Tok::KW_TRY: return "try";
    case Tok::KW_CATCH: return "catch";
    case Tok::KW_THROW: return "throw";
    case Tok::KW_AND: return "and";
    case Tok::KW_OR: return "or";
    case Tok::KW_NOT: return "not";
    case Tok::KW_AS: return "as";
    case Tok::KW_ENUM: return "enum";
    case Tok::KW_BREAKPOINT: return "debug";
    case Tok::KW_IMPORT: return "import";
    case Tok::LPAREN: return "(";
    case Tok::RPAREN: return ")";
    case Tok::LBRACE: return "{";
    case Tok::RBRACE: return "}";
    case Tok::LBRACKET: return "[";
    case Tok::RBRACKET: return "]";
    case Tok::COMMA: return ",";
    case Tok::COLON: return ":";
    case Tok::DOT: return ".";
    case Tok::SEMICOLON: return ";";
    case Tok::QUESTION: return "?";
    case Tok::ARROW: return "->";
    case Tok::FATARROW: return "=>";
    case Tok::ELLIPSIS: return "...";
    case Tok::ASSIGN: return "=";
    case Tok::PLUS: return "+";
    case Tok::MINUS: return "-";
    case Tok::STAR: return "*";
    case Tok::SLASH: return "/";
    case Tok::PERCENT: return "%";
    case Tok::BANG: return "!";
    case Tok::AMP: return "&";
    case Tok::PIPE: return "|";
    case Tok::CARET: return "^";
    case Tok::TILDE: return "~";
    case Tok::BANG_EQUAL: return "!=";
    case Tok::EQUAL: return "==";
    case Tok::LESS: return "<";
    case Tok::LESS_EQUAL: return "<=";
    case Tok::GREATER: return ">";
    case Tok::GREATER_EQUAL: return ">=";
    case Tok::SHL: return "<<";
    case Tok::SHR: return ">>";
    case Tok::DOUBLE_STAR: return "**";
    case Tok::PLUS_EQUAL: return "+=";
    case Tok::MINUS_EQUAL: return "-=";
    case Tok::STAR_EQUAL: return "*=";
    case Tok::SLASH_EQUAL: return "/=";
    case Tok::PERCENT_EQUAL: return "%=";
    case Tok::INCREMENT: return "++";
    case Tok::DECREMENT: return "--";
    case Tok::EOF_: return "end of file";
    case Tok::INVALID: return "invalid token";
  }
  return "?";
}

}  // namespace ov