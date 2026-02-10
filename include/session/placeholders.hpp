#pragma once

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <string_view>
#include <ranges>

namespace session::sqlite {

// Helper for producing a `x IN (?,?,?)` query with a variable number of ? placeholders using fmt.
//
// This function is not included in the main sqlite.hpp as it requires fmt to already be available
// (and fmt is not a strict requirement of this library).
//
// Usage:
//
//     auto query = fmt::format("SELECT * FROM x WHERE y IN ({})", placeholders(values.size()));
//
// Returns an empty string if given a count < 1.  Note that if this is actually being used in an SQL
// query, count must be >= 1 (because `x IN ()` is not a valid SQL expression).
inline auto placeholders(
        int count, std::string_view symbol = "?", std::string_view separator = ",") {
    return fmt::join(
#ifdef __cpp_lib_ranges_repeat
            std::views::repeat(symbol, count)
#else
            std::views::iota(0, count) | std::views::transform([symbol](int) { return symbol; })
#endif
                    ,
            separator);
}

}
