#pragma once

#include <fmt/core.h>

#include <algorithm>
#include <string_view>

namespace session::sqlite {

namespace detail {
    // What `placeholders()` returns: the repetition, deferred until it is formatted.  Holds only
    // views, so it is as safe as the arguments handed to it -- the defaults are literals, and a
    // caller passing a temporary string is subject to the usual string_view rules.
    struct placeholder_list {
        int count;
        std::string_view symbol;
        std::string_view separator;
    };
}  // namespace detail

// Helper for producing a `x IN (?,?,?)` query with a variable number of ? placeholders using fmt.
//
// This function is not included in the main sqlite.hpp as it requires fmt to already be available
// (and fmt is not a strict requirement of this library).
//
// Usage:
//
//     auto query = fmt::format("SELECT * FROM x WHERE y IN ({})", placeholders(values.size()));
//
// Formats to an empty string if given a count < 1.  Note that if this is actually being used in an
// SQL query, count must be >= 1 (because `x IN ()` is not a valid SQL expression).
//
// This returns a type of its own rather than an `fmt::join` over a generated range, which is what it
// used to do and which never worked: a join keeps only iterators, and the iterators of a
// `transform_view` -- or a `repeat_view` -- point back at the view to reach the callable or value
// they repeat.  Over a range built inside this function, that leaves the caller holding iterators
// into a destroyed temporary; formatting one reads returned stack, which ASan reports as
// stack-use-after-return and which without ASan yields garbage of unbounded length, surfacing as a
// std::bad_alloc somewhere else entirely.
inline detail::placeholder_list placeholders(
        int count, std::string_view symbol = "?", std::string_view separator = ",") {
    return {count, symbol, separator};
}

}  // namespace session::sqlite

template <>
struct fmt::formatter<session::sqlite::detail::placeholder_list> {
    constexpr auto parse(fmt::format_parse_context& ctx) const { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const session::sqlite::detail::placeholder_list& p, FormatContext& ctx) const {
        auto out = ctx.out();
        for (int i = 0; i < p.count; i++) {
            if (i > 0)
                out = std::copy(p.separator.begin(), p.separator.end(), out);
            out = std::copy(p.symbol.begin(), p.symbol.end(), out);
        }
        return out;
    }
};
