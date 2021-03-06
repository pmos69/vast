#ifndef VAST_CONCEPT_PARSEABLE_VAST_TIME_H
#define VAST_CONCEPT_PARSEABLE_VAST_TIME_H

#include "vast/access.h"
#include "vast/time.h"
#include "vast/concept/parseable/core.h"
#include "vast/concept/parseable/numeric/real.h"
#include "vast/concept/parseable/string/char_class.h"

namespace vast {

struct time_duration_parser : parser<time_duration_parser> {
  using attribute = time::duration;

  template <typename Iterator, typename Attribute>
  bool parse(Iterator& f, Iterator const& l, Attribute& a) const {
    using namespace parsers;
    auto save = f;
    int64_t i;
    if (!i64.parse(f, l, i))
      return false;
    static auto whitespace = *space;
    if (!whitespace.parse(f, l, unused)) {
      f = save;
      return false;
    }
    static auto unit
      = "nsecs"_p ->* [] { return time::nanoseconds(1); }
      | "nsec"_p  ->* [] { return time::nanoseconds(1); }
      | "ns"_p    ->* [] { return time::nanoseconds(1); }
      | "usecs"_p ->* [] { return time::microseconds(1); }
      | "usec"_p  ->* [] { return time::microseconds(1); }
      | "us"_p    ->* [] { return time::microseconds(1); }
      | "msecs"_p ->* [] { return time::milliseconds(1); }
      | "msec"_p  ->* [] { return time::milliseconds(1); }
      | "ms"_p    ->* [] { return time::milliseconds(1); }
      | "secs"_p  ->* [] { return time::seconds(1); }
      | "sec"_p   ->* [] { return time::seconds(1); }
      | "s"_p     ->* [] { return time::seconds(1); }
      | "mins"_p  ->* [] { return time::minutes(1); }
      | "min"_p   ->* [] { return time::minutes(1); }
      | "m"_p     ->* [] { return time::minutes(1); }
      | "hrs"_p   ->* [] { return time::hours(1); }
      | "hours"_p ->* [] { return time::hours(1); }
      | "hour"_p  ->* [] { return time::hours(1); }
      | "h"_p     ->* [] { return time::hours(1); }
      | "days"_p  ->* [] { return time::hours(24); }
      | "day"_p   ->* [] { return time::hours(24); }
      | "d"_p     ->* [] { return time::hours(24); }
      | "weeks"_p ->* [] { return time::hours(24 * 7); }
      | "week"_p  ->* [] { return time::hours(24 * 7); }
      | "w"_p     ->* [] { return time::hours(24 * 7); }
      | "years"_p ->* [] { return time::hours(24 * 365); }
      | "year"_p  ->* [] { return time::hours(24 * 365); }
      | "y"_p     ->* [] { return time::hours(24 * 365); }
      ;
    if (! unit.parse(f, l, a)) {
      f = save;
      return false;
    }
    a *= i;
    return true;
  }
};

template <>
struct parser_registry<time::duration> {
  using type = time_duration_parser;
};

namespace parsers {

auto const time_duration = time_duration_parser{};

} // namespace parsers

struct ymdhms_parser : vast::parser<ymdhms_parser> {
  using attribute = time::point;

  static auto make() {
    auto year = integral_parser<int, 4, 4>{}
                  .with([](auto x) { return x >= 1900; })
                  ->* [](int y) { return y - 1900; };
    auto mon = integral_parser<int, 2, 2>{}
                 .with([](auto x) { return x >= 1 && x <= 12; })
                  ->* [](int m) { return m - 1; };
    auto day = integral_parser<int, 2, 2>{}
                 .with([](auto x) { return x >= 1 && x <= 31; });
    auto hour = integral_parser<int, 2, 2>{}
                 .with([](auto x) { return x >= 0 && x <= 23; });
    auto min = integral_parser<int, 2, 2>{}
                 .with([](auto x) { return x >= 0 && x <= 59; });
    auto sec = integral_parser<int, 2, 2>{}
                 .with([](auto x) { return x >= 0 && x <= 60; }); // leap sec
    return year >> '-' >> mon
        >> ~('-' >> day >> ~('+' >> hour >> ~(':' >> min >> ~(':' >> sec))));
  }

  template <typename Iterator>
  bool parse(Iterator& f, Iterator const& l, unused_type) const {
    static auto p = make();
    return p.parse(f, l, unused);
  }

  template <typename Iterator>
  bool parse(Iterator& f, Iterator const& l, time::point& tp) const {
    static auto p = make();
    std::tm tm;
    std::memset(&tm, 0, sizeof(tm));
    tm.tm_mday = 1;
    auto ms = std::tie(tm.tm_min, tm.tm_sec);
    auto hms = std::tie(tm.tm_hour, ms);
    auto dhms = std::tie(tm.tm_mday, hms);
    auto ymdhms = std::tie(tm.tm_year, tm.tm_mon, dhms);
    if (!p.parse(f, l, ymdhms))
      return false;
    tp = time::point::from_tm(tm);
    return true;
  }
};

namespace parsers {

auto const ymdhms = ymdhms_parser{};

/// Parses a fractional seconds-timestamp as UNIX epoch.
auto const epoch = real_opt_dot
  ->* [](double d) { return time::point{time::double_seconds{d}}; };

} // namespace parsers

struct time_point_parser : parser<time_point_parser> {
  using attribute = time::point;

  template <typename Iterator, typename Attribute>
  bool parse(Iterator& f, Iterator const& l, Attribute& a) const {
    static auto plus = [](time::duration d) {
      return time::now() + d;
    };
    static auto minus = [](time::duration d) {
      return time::now() - d;
    };
    static auto ws = ignore(*parsers::space);
    static auto p
      = parsers::ymdhms
      | '@' >> parsers::epoch
      | "now" >> ws >> ( '+' >> ws >> parsers::time_duration ->* plus
                       | '-' >> ws >> parsers::time_duration ->* minus )
      | "now"_p ->* []() { return time::now(); }
      | "in" >> ws >> parsers::time_duration ->* plus
      | (parsers::time_duration ->* minus) >> ws >> "ago"
      ;
    return p.parse(f, l, a);
  }
};

template <>
struct parser_registry<time::point> {
  using type = time_point_parser;
};

namespace parsers {

static auto const time_point = make_parser<time::point>();

} // namespace parsers

} // namespace vast

#endif
