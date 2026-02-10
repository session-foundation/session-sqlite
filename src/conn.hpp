#pragma once

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <memory>
#include <session/sqlite.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace session::sqlite::detail {

// Internal wrapper that holds a single connection and a statement cache associated with that
// connection.
class conn {
  public:
    SQLite::Database sql;

    template <typename... Args>
    conn(Args&&... args) : sql{std::forward<Args>(args)...} {}

    void reset_thread() { _thread = std::this_thread::get_id(); }

    StatementWrapper prepared_st(const std::string& query);

    void statement_finished(std::unique_ptr<SQLite::Statement> st);

  private:
    std::thread::id _thread{std::this_thread::get_id()};

    // SQLiteCpp's statements are not thread-safe, so we prepare them thread-locally when needed
    std::unordered_map<std::string, std::vector<std::unique_ptr<SQLite::Statement>>> _st_cache;
};

}  // namespace session::sqlite::detail
