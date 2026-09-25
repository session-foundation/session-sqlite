#pragma once

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <functional>
#include <memory>
#include <session/sqlite.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace session::sqlite::detail {

// Transparent hash so that the statement cache can be searched by string_view without constructing
// a std::string key for every lookup.
struct string_hash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

// Internal wrapper that holds a single connection and a statement cache associated with that
// connection.
class conn {
  public:
    SQLite::Database sql;

    template <typename... Args>
    conn(Args&&... args) : sql{std::forward<Args>(args)...} {}

    void reset_thread() { _thread = std::this_thread::get_id(); }

    StatementWrapper prepared_st(std::string_view query);

    void statement_finished(std::unique_ptr<SQLite::Statement> st);

  private:
    std::thread::id _thread{std::this_thread::get_id()};

    // SQLiteCpp's statements are not thread-safe, so we prepare them thread-locally when needed
    std::unordered_map<
            std::string,
            std::vector<std::unique_ptr<SQLite::Statement>>,
            string_hash,
            std::equal_to<>>
            _st_cache;
};

}  // namespace session::sqlite::detail
