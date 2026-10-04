// Vayu :: compiler/tools/fmt.h
#pragma once

#include <string>

#include "../ast/ast.h"

namespace vy {

// Re-render a parsed program in canonical layout. The formatter works from
// the AST rather than the text so it cannot invent syntax the parser did not
// accept; string literals are emitted from their decoded values, which means
// `\n` inside a string round-trips correctly.
std::string format_program(const ast::Program& program, const std::string& source);

}  // namespace vy