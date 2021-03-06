#ifndef VAST_CONCEPT_PARSEABLE_CORE_LITERAL_H
#define VAST_CONCEPT_PARSEABLE_CORE_LITERAL_H

#include <cstddef>
#include <type_traits>
#include <string>

#include "vast/concept/parseable/core/ignore.h"
#include "vast/concept/parseable/string/char.h"
#include "vast/concept/parseable/string/string.h"

namespace vast {

inline auto operator"" _p(char c) {
  return ignore(char_parser{c});
}

inline auto operator"" _p(char const* str) {
  return ignore(string_parser{str});
}

inline auto operator"" _p(char const* str, size_t size) {
  return ignore(string_parser{{str, size}});
}

inline auto operator"" _p(unsigned long long int x) {
  return ignore(string_parser{std::to_string(x)});
}

inline auto operator"" _p(long double x) {
  return ignore(string_parser{std::to_string(x)});
}

} // namespace vast

#endif
