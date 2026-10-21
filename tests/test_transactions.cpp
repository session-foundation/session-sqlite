#include <sqlite3.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>
#include <session/sqlite.hpp>
#include <string>

using namespace session::sqlite;

namespace {

struct TempPath {
    std::filesystem::path path;
    TempPath() :
            path{std::filesystem::temp_directory_path() /
                 ("session-sqlite-tx-" + std::to_string(std::random_device{}()) + ".db")} {}
    ~TempPath() {
        std::error_code ec;
        for (const auto* suffix : {"", "-wal", "-shm"})
            std::filesystem::remove(path.string() + suffix, ec);
    }
};

// Whether another connection, which does not wait, can take the write lock right now
bool can_write_elsewhere(const std::filesystem::path& path) {
    sqlite3* other = nullptr;
    REQUIRE(sqlite3_open_v2(path.c_str(), &other, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    sqlite3_busy_timeout(other, 0);
    bool ok = sqlite3_exec(other, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (ok)
        sqlite3_exec(other, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(other);
    return ok;
}

}  // namespace

TEST_CASE("begin_immediate takes the write lock at BEGIN, begin does not", "[transactions]") {
    TempPath tmp;
    Database db{tmp.path};
    db.conn().sql.exec("CREATE TABLE foo (x INTEGER)");

    {
        auto c = db.conn();
        auto tx = c.begin();
        CHECK(can_write_elsewhere(tmp.path));
    }
    {
        auto c = db.conn();
        auto tx = c.begin_immediate();
        // Before any statement has run in it
        CHECK_FALSE(can_write_elsewhere(tmp.path));
        c.sql.exec("INSERT INTO foo VALUES (1)");
        tx.commit();
    }
    CHECK(can_write_elsewhere(tmp.path));
    CHECK(db.conn().prepared_get<int>("SELECT count(*) FROM foo") == 1);
}

TEST_CASE(
        "a deferred transaction cannot write after another connection commits; an immediate one "
        "waits its turn",
        "[transactions]") {
    // Why a transaction that may write wants begin_immediate: a deferred one reads from a snapshot,
    // and once another connection has committed it can never write.
    TempPath tmp;
    Database db{tmp.path, busy_timeout{std::chrono::milliseconds{2000}}};
    db.conn().sql.exec("CREATE TABLE foo (x INTEGER)");

    auto mine = db.unique_conn();
    auto other = db.unique_conn();

    auto tx = mine.begin();
    CHECK(mine.prepared_get<int>("SELECT count(*) FROM foo") == 0);
    other.sql.exec("INSERT INTO foo VALUES (1)");
    try {
        mine.sql.exec("INSERT INTO foo VALUES (2)");
        FAIL("a stale snapshot was allowed to write");
    } catch (const SQLite::Exception& e) {
        CHECK((e.getErrorCode() & 0xff) == SQLITE_BUSY);
    }
    tx.rollback();

    auto itx = mine.begin_immediate();
    CHECK(mine.prepared_get<int>("SELECT count(*) FROM foo") == 1);
    mine.sql.exec("INSERT INTO foo VALUES (2)");
    itx.commit();
    CHECK(other.prepared_get<int>("SELECT count(*) FROM foo") == 2);
}
