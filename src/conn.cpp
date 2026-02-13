#include "conn.hpp"

#include <session/sqlite.hpp>

namespace session::sqlite::detail {

StatementWrapper conn::prepared_st(const std::string& query) {
    // If you hit this assert hang your head in shame for not following the ridiculous number of
    // times you have been told not to use a Connection across threads:
    assert(std::this_thread::get_id() == _thread);

    std::unique_ptr<SQLite::Statement> st;
    if (auto it = _st_cache.find(query); it != _st_cache.end() && !it->second.empty()) {
        st = std::move(it->second.back());
        it->second.pop_back();
    } else {
        st = std::make_unique<SQLite::Statement>(sql, query, SQLite::PREPARE_PERSISTENT);
    }

    return {*this, std::move(st)};
}

void conn::statement_finished(std::unique_ptr<SQLite::Statement> st) {
    if (!st)
        return;
    _st_cache[st->getQuery()].push_back(std::move(st));
}

}  // namespace session::sqlite::detail
